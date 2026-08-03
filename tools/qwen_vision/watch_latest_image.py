#!/usr/bin/env python3
"""Periodically analyze a newly atomically-written camera snapshot."""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from qwen_vision_client import QwenVisionClient
from test_fixed_image import load_qwen_env


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze only new latest.jpg snapshots.")
    parser.add_argument("--image", required=True, help="Atomically updated latest image path")
    parser.add_argument("--result", required=True, help="Atomically updated JSON result path")
    parser.add_argument("--interval-ms", type=int, default=5000, help="Minimum API interval")
    args = parser.parse_args()
    if args.interval_ms <= 0:
        parser.error("--interval-ms must be positive")

    load_qwen_env()
    client = QwenVisionClient()
    image_path = Path(args.image)
    result_path = Path(args.result)
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
            result = client.analyze_image(str(image_path))
            document = {
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "image_path": str(image_path),
                "model": result["model"],
                **result,
            }
            try:
                write_json_atomically(result_path, document)
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
