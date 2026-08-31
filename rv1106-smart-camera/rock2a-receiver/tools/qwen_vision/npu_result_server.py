#!/usr/bin/env python3
"""接收 RV1106 本地 NPU 检测消息并发布最新状态。

TCP 输入按换行分帧；服务同时保存完整轨迹状态、生成 Qt 兼容显示文档，
并异步转发给事件 HTTP 服务，避免网络转发阻塞接收端。
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
import time
import queue
import urllib.error
import urllib.request
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Any, Optional


def write_json_atomically(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name = ""
    try:
        with NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=path.name + ".",
            suffix=".tmp",
            delete=False,
        ) as temporary:
            temporary_name = temporary.name
            json.dump(document, temporary, ensure_ascii=False, separators=(",", ":"))
            temporary.write("\n")
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_name, path)
    finally:
        if temporary_name and os.path.exists(temporary_name):
            os.unlink(temporary_name)


def parse_line(line: bytes) -> Optional[dict[str, Any]]:
    try:
        document = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(document, dict):
        return None
    if document.get("type") != "npu" and document.get("message_type") not in {
        "detection",
        "track.update",
    }:
        return None
    return document


def to_display_document(document: dict[str, Any]) -> dict[str, Any]:
    """Shape the wire message into the schema the RV1106 Qt UI expects."""
    try:
        people = int(
            document.get(
                "peopleCount",
                document.get(
                    "people_count",
                    (
                        len(document.get("tracks", []))
                        if isinstance(document.get("tracks"), list)
                        else 0
                    ),
                ),
            )
        )
    except (TypeError, ValueError):
        people = 0
    if people < 0:
        people = 0
    try:
        latency = int(float(document.get("latencyMs", document.get("latency_ms", 0))))
    except (TypeError, ValueError):
        latency = 0
    sentinel_value = document.get("sentinelActive", document.get("sentinel_active"))
    sentinel_active = sentinel_value if isinstance(sentinel_value, bool) else people > 0
    display_value = document.get("displayAwake", document.get("display_awake"))
    display_awake = display_value if isinstance(display_value, bool) else sentinel_active
    return {
        "type": "manual_result",
        "source": "local",
        "timestamp": document.get("timestamp", time.time() * 1000),
        "model": document.get("model", "yolov5n-320"),
        "success": True,
        "latency_ms": latency,
        "server_latency_ms": 0,
        "sentinel_active": sentinel_active,
        "display_awake": display_awake,
        "result": {
            "scene": "端侧NPU",
            "people_count": people,
            # Person presence is an observation, not an alarm condition.  The
            # person-only NPU model has no behavior or restricted-zone rules.
            "warning": False,
            "warning_reason": "",
            "summary": "本地NPU检测到%d人" % people if people > 0 else "本地NPU未检测到人员",
            "objects": ["person"] if people > 0 else [],
        },
    }


def to_state_document(document: dict[str, Any], display: dict[str, Any]) -> dict[str, Any]:
    """Keep the v1 track payload while preserving legacy local consumers."""
    state = dict(document)
    state.setdefault("message_type", "track.update")
    state["people_count"] = display["result"]["people_count"]
    state["sentinel_active"] = display["sentinel_active"]
    state["display_awake"] = display["display_awake"]
    state["latency_ms"] = display["latency_ms"]
    state.setdefault("result", display["result"])
    return state


class NpuResultServer:
    def __init__(
        self,
        host: str,
        port: int,
        result_path: Path,
        display_path: Path,
        event_path: Path,
        event_url: str = "",
        max_line: int = 65536,
    ) -> None:
        self.host = host
        self.port = port
        self.result_path = result_path
        self.display_path = display_path
        self.event_path = event_path
        self.max_line = max_line
        self.event_url = event_url
        self._lock = threading.Lock()
        self._display_key: Optional[tuple[int, bool, bool]] = None
        self._event_queue: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=128)
        if self.event_url:
            threading.Thread(target=self._event_worker, daemon=True).start()

    def _event_worker(self) -> None:
        while True:
            document = self._event_queue.get()
            try:
                data = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode(
                    "utf-8"
                )
                request = urllib.request.Request(
                    self.event_url,
                    data=data,
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                with urllib.request.urlopen(request, timeout=1.0) as response:
                    response.read(1024)
            except (OSError, urllib.error.URLError) as error:
                print(f"[npu-server] event forward failed: {error}", file=sys.stderr, flush=True)
            finally:
                self._event_queue.task_done()

    def handle(self, document: dict[str, Any]) -> None:
        people = 0
        try:
            people = int(document.get("peopleCount", 0))
        except (TypeError, ValueError):
            pass
        display = to_display_document(document)
        state = to_state_document(document, display)
        display_key = (
            display["result"]["people_count"],
            display["sentinel_active"],
            display["display_awake"],
        )
        with self._lock:
            write_json_atomically(self.result_path, state)
            if display_key != self._display_key:
                write_json_atomically(self.display_path, display)
                self.event_path.parent.mkdir(parents=True, exist_ok=True)
                with self.event_path.open("a", encoding="utf-8") as log:
                    log.write(json.dumps(display, ensure_ascii=False, separators=(",", ":")) + "\n")
                self._display_key = display_key
        if self.event_url:
            forwarded = dict(document)
            forwarded["received_at_ms"] = int(time.time() * 1000)
            try:
                self._event_queue.put_nowait(forwarded)
            except queue.Full:
                print(
                    "[npu-server] event queue full; dropping newest message",
                    file=sys.stderr,
                    flush=True,
                )
        print(
            f"[npu] people={people} active={display['sentinel_active']} "
            f"display={display['display_awake']} "
            f"latency_ms={document.get('latencyMs', '?')} updated={self.result_path}",
            flush=True,
        )

    def serve(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((self.host, self.port))
            server.listen(2)
            server.settimeout(1.0)
            print(f"[npu-server] listening on {self.host}:{self.port}", flush=True)
            while True:
                try:
                    client, address = server.accept()
                except socket.timeout:
                    continue
                threading.Thread(
                    target=self.serve_client, args=(client, address), daemon=True
                ).start()

    def serve_client(self, client: socket.socket, address: tuple) -> None:
        print(f"[npu-server] client connected: {address[0]}:{address[1]}", flush=True)
        buffer = b""
        try:
            with client:
                while True:
                    chunk = client.recv(1024)
                    if not chunk:
                        break
                    buffer += chunk
                    if len(buffer) > self.max_line and b"\n" not in buffer:
                        print(
                            "[npu-server] unterminated oversized line rejected",
                            file=sys.stderr,
                            flush=True,
                        )
                        buffer = b""
                        continue
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        if len(line) > self.max_line:
                            print(
                                "[npu-server] oversized line rejected", file=sys.stderr, flush=True
                            )
                            continue
                        document = parse_line(line)
                        if document is not None:
                            self.handle(document)
        except OSError as error:
            print(f"[npu-server] client error: {error}", file=sys.stderr, flush=True)
        print(f"[npu-server] client disconnected: {address[0]}:{address[1]}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="NPU detection result receiver")
    parser.add_argument("--host", default="192.168.50.1")
    parser.add_argument("--port", type=int, default=9010)
    parser.add_argument(
        "--result-path",
        type=Path,
        default=Path.home() / "AI_CAMERA_LINUX/rock2a_receiver/runtime/npu_latest.json",
    )
    parser.add_argument("--event-url", default="http://192.168.50.1:9011/ingest")
    parser.add_argument(
        "--event-path",
        type=Path,
        default=Path.home() / "AI_CAMERA_LINUX/rock2a_receiver/runtime/npu_events.log",
    )
    parser.add_argument(
        "--display-path",
        type=Path,
        help="low-rate Qt result path; defaults to --result-path",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = NpuResultServer(
        args.host,
        args.port,
        args.result_path,
        args.display_path or args.result_path,
        args.event_path,
        args.event_url,
    )
    try:
        server.serve()
    except KeyboardInterrupt:
        print("\n[npu-server] stopped", flush=True)
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
