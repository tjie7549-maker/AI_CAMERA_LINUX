#!/usr/bin/env python3
"""Receive local NPU detection messages from the RV1106 and publish the latest."""

from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
import time
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Any, Optional


def write_json_atomically(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_name = ""
    try:
        with NamedTemporaryFile(
            mode="w", encoding="utf-8", dir=path.parent,
            prefix=path.name + ".", suffix=".tmp", delete=False,
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
    if not isinstance(document, dict) or document.get("type") != "npu":
        return None
    return document


def to_display_document(document: dict[str, Any]) -> dict[str, Any]:
    """Shape the wire message into the schema the RV1106 Qt UI expects."""
    try:
        people = int(document.get("peopleCount", 0))
    except (TypeError, ValueError):
        people = 0
    if people < 0:
        people = 0
    try:
        latency = int(float(document.get("latencyMs", 0)))
    except (TypeError, ValueError):
        latency = 0
    return {
        "type": "manual_result",
        "source": "local",
        "timestamp": document.get("timestamp", time.time() * 1000),
        "model": document.get("model", "yolov5n-320"),
        "success": True,
        "latency_ms": latency,
        "server_latency_ms": 0,
        "result": {
            "scene": "端侧NPU",
            "people_count": people,
            "warning": people > 0,
            "warning_reason": "本地NPU检测到%d人" % people if people > 0 else "无人",
            "summary": "本地NPU检测：人×%d" % people,
            "objects": ["person"] if people > 0 else [],
        },
    }


class NpuResultServer:
    def __init__(self, host: str, port: int, result_path: Path,
                 event_path: Path, max_line: int = 4096) -> None:
        self.host = host
        self.port = port
        self.result_path = result_path
        self.event_path = event_path
        self.max_line = max_line
        self._lock = threading.Lock()

    def handle(self, document: dict[str, Any]) -> None:
        people = 0
        try:
            people = int(document.get("peopleCount", 0))
        except (TypeError, ValueError):
            pass
        display = to_display_document(document)
        with self._lock:
            write_json_atomically(self.result_path, display)
            self.event_path.parent.mkdir(parents=True, exist_ok=True)
            with self.event_path.open("a", encoding="utf-8") as log:
                log.write(json.dumps(display, ensure_ascii=False, separators=(",", ":")) + "\n")
        print(
            f"[npu] people={people} latency_ms={document.get('latencyMs', '?')} "
            f"updated={self.result_path}",
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
                    while b"\n" in buffer:
                        line, buffer = buffer.split(b"\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        document = parse_line(line[: self.max_line])
                        if document is not None:
                            self.handle(document)
        except OSError as error:
            print(f"[npu-server] client error: {error}", file=sys.stderr, flush=True)
        print(f"[npu-server] client disconnected: {address[0]}:{address[1]}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="NPU detection result receiver")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9010)
    parser.add_argument(
        "--result-path",
        type=Path,
        default=Path.home() / "AI_CAMERA_LINUX/rock2a_receiver/runtime/npu_latest.json",
    )
    parser.add_argument(
        "--event-path",
        type=Path,
        default=Path.home() / "AI_CAMERA_LINUX/rock2a_receiver/runtime/npu_events.log",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    server = NpuResultServer(args.host, args.port, args.result_path, args.event_path)
    try:
        server.serve()
    except KeyboardInterrupt:
        print("\n[npu-server] stopped", flush=True)
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
