#!/usr/bin/env python3
"""Periodically analyze a newly atomically-written camera snapshot."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from qwen_vision_client import QwenVisionClient
from test_fixed_image import load_qwen_env
from result_cache import cache, save


STOP_REQUESTED = False


def on_signal(_: int, __: object) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True


def image_fingerprint(path: Path) -> tuple[int, int] | None:
    try:
        stat = path.stat()
    except FileNotFoundError:
        return None
    if not path.is_file() or stat.st_size == 0:
        return None
    return stat.st_mtime_ns, stat.st_size


def write_json_atomically(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name = ""
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=path.name + ".",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_name = temporary.name
            json.dump(value, temporary, ensure_ascii=False, indent=2)
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_name, path)
    finally:
        if temporary_name and os.path.exists(temporary_name):
            os.unlink(temporary_name)


def print_request(request_id: int, fingerprint: tuple[int, int], result: dict[str, Any]) -> None:
    usage = result.get("usage", {})
    if result["success"]:
        state = "success"
    else:
        state = "error_type=" + result["error_type"]
    print(
        "request_id={request_id} model={model} image_mtime_ns={mtime} "
        "image_size={size} latency_ms={latency} input_tokens={input_tokens} "
        "output_tokens={output_tokens} total_tokens={total_tokens} {state}".format(
            request_id=request_id,
            model=result["model"],
            mtime=fingerprint[0],
            size=fingerprint[1],
            latency=result["latency_ms"],
            input_tokens=usage.get("input_tokens", 0),
            output_tokens=usage.get("output_tokens", 0),
            total_tokens=usage.get("total_tokens", 0),
            state=state,
        ),
        flush=True,
    )


def disk_free_mb(path: Path) -> float:
    usage = shutil.disk_usage(path)
    return usage.free / (1024 * 1024)


def dedup_key(document: dict[str, Any]) -> str:
    result = document.get("result", {})
    return json.dumps(
        [
            result.get("scene"),
            result.get("objects"),
            result.get("warning"),
            result.get("warning_reason"),
        ],
        ensure_ascii=False,
        sort_keys=True,
    )


def load_dedup_state(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_dedup_state(path: Path, key: str) -> None:
    try:
        write_json_atomically(path, {"key": key, "ts": time.time()})
    except OSError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze only new latest.jpg snapshots.")
    parser.add_argument("--image", required=True, help="Atomically updated latest image path")
    parser.add_argument("--result", required=True, help="Atomically updated JSON result path")
    parser.add_argument("--interval-ms", type=int, default=5000, help="Minimum API interval")
    parser.add_argument(
        "--auto-save-policy",
        choices=("none", "warning", "all"),
        default="warning",
        help="Auto-save policy: none=never, warning=only warning results, all=every result",
    )
    parser.add_argument(
        "--auto-save-dedup-seconds",
        type=int,
        default=60,
        help="Skip auto-saving the same scene again within this window",
    )
    parser.add_argument(
        "--min-free-mb",
        type=int,
        default=1024,
        help="Minimum free disk MiB required for auto-save",
    )
    args = parser.parse_args()
    if args.interval_ms <= 0 or args.auto_save_dedup_seconds < 0 or args.min_free_mb < 0:
        parser.error("interval/dedup/min-free must be non-negative")

    load_qwen_env()
    client = QwenVisionClient()
    image_path = Path(args.image)
    result_path = Path(args.result)
    runtime_root = result_path.parent.parent
    dedup_path = runtime_root / "auto_save_dedup.json"
    last_fingerprint: tuple[int, int] | None = None
    last_request_started = 0.0
    request_id = 0
    successes = failures = invalid_json = 0
    total_latency_ms = total_input_tokens = total_output_tokens = total_tokens = 0

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    interval_seconds = args.interval_ms / 1000.0

    while not STOP_REQUESTED:
        fingerprint = image_fingerprint(image_path)
        now = time.monotonic()
        if (
            fingerprint is not None
            and fingerprint != last_fingerprint
            and now - last_request_started >= interval_seconds
        ):
            request_id += 1
            last_request_started = now
            try:
                image_bytes = image_path.read_bytes()
            except OSError:
                print("request_id={} image_read_error".format(request_id), flush=True)
                last_fingerprint = fingerprint
                continue
            result = client.analyze_image_bytes(image_bytes, "image/jpeg")
            request_name = "auto-{}-{:04d}".format(int(time.time() * 1000), request_id)
            document = {
                "type": "auto_result",
                "source": "auto",
                "request_id": request_name,
                "frame_id": None,
                "frame_timestamp_ns": None,
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "image_path": str(image_path),
                "model": result["model"],
                **result,
            }
            try:
                write_json_atomically(result_path, document)
                cache(runtime_root, "auto", request_name, image_bytes, document, {
                    "source": "auto",
                    "request_id": request_name,
                    "frame_id": None,
                    "frame_timestamp_ns": None,
                    "created_at": document["timestamp"],
                    "model": result.get("model"),
                    "image_bytes": len(image_bytes),
                    "image_width": None,
                    "image_height": None,
                    "server_latency_ms": result.get("latency_ms"),
                    **result.get("usage", {}),
                })
            except OSError:
                print("request_id={} result_write_error".format(request_id), flush=True)
            print_request(request_id, fingerprint, result)
            last_fingerprint = fingerprint
            total_latency_ms += result["latency_ms"]
            usage = result.get("usage", {})
            total_input_tokens += usage.get("input_tokens", 0)
            total_output_tokens += usage.get("output_tokens", 0)
            total_tokens += usage.get("total_tokens", 0)
            if result["success"]:
                successes += 1
            else:
                failures += 1
                invalid_json += int(result.get("error_type") == "invalid_json")

            # Auto-save policy (reuses the same result_cache.save() core).
            if args.auto_save_policy != "none" and result.get("success"):
                try:
                    free = disk_free_mb(runtime_root)
                    if free < args.min_free_mb:
                        print(
                            "auto-save skipped: disk low {:.0f} MiB < {} MiB".format(
                                free, args.min_free_mb
                            ),
                            flush=True,
                        )
                    else:
                        key = dedup_key(document)
                        state = load_dedup_state(dedup_path)
                        last_ts = float(state.get("ts", 0) or 0)
                        duplicated = (
                            key == state.get("key")
                            and time.time() - last_ts < args.auto_save_dedup_seconds
                        )
                        if duplicated:
                            print(
                                "auto-save skipped: duplicate scene within {}s".format(
                                    args.auto_save_dedup_seconds
                                ),
                                flush=True,
                            )
                        else:
                            if args.auto_save_policy == "warning" and not document.get("result", {}).get("warning"):
                                print("auto-save skipped: policy=warning and warning=false", flush=True)
                            else:
                                relative, _ = save(runtime_root, "auto", request_name)
                                save_dedup_state(dedup_path, key)
                                print("auto-saved {} -> {}".format(request_name, relative), flush=True)
                except Exception as exc:
                    print("auto-save error: {}".format(exc), flush=True)
        time.sleep(min(0.2, interval_seconds))

    average_latency = total_latency_ms / request_id if request_id else 0
    print(
        "summary requests={} successes={} failures={} invalid_json={} avg_latency_ms={:.1f} "
        "input_tokens={} output_tokens={} total_tokens={}".format(
            request_id,
            successes,
            failures,
            invalid_json,
            average_latency,
            total_input_tokens,
            total_output_tokens,
            total_tokens,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
