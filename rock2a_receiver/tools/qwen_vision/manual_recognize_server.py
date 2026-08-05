#!/usr/bin/env python3
"""HTTP endpoint for exactly one RV1106 frozen-frame recognition request."""

from __future__ import annotations

import argparse
import json
import os
import threading
import time
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import NamedTemporaryFile
from typing import Any

from qwen_vision_client import QwenVisionClient
from test_fixed_image import load_qwen_env


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


class ManualRecognitionServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], handler: type[BaseHTTPRequestHandler],
                 client: QwenVisionClient, result_path: Path, max_image_bytes: int) -> None:
        super().__init__(address, handler)
        self.client = client
        self.result_path = result_path
        self.max_image_bytes = max_image_bytes
        self.recognize_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    server: ManualRecognitionServer

    def log_message(self, format: str, *args: object) -> None:
        del format, args

    def send_json(self, status: int, document: dict[str, Any]) -> None:
        data = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path != "/health":
            self.send_json(HTTPStatus.NOT_FOUND, {"success": False, "error": "not found"})
            return
        self.send_json(HTTPStatus.OK, {
            "status": "ok",
            "busy": self.server.recognize_lock.locked(),
            "model": self.server.client.model,
        })

    def do_POST(self) -> None:
        if self.path != "/recognize":
            self.send_json(HTTPStatus.NOT_FOUND, {"success": False, "error": "not found"})
            return
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "image/jpeg":
            self.send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": "content type must be image/jpeg"})
            return
        content_length = self.headers.get("Content-Length")
        try:
            size = int(content_length) if content_length is not None else 0
        except ValueError:
            size = 0
        if size <= 0:
            self.send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": "missing or invalid content length"})
            return
        if size > self.server.max_image_bytes:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"success": False, "error": "image too large"})
            return
        request_id = self.headers.get("X-Request-Id", "").strip()
        frame_id_text = self.headers.get("X-Frame-Id", "")
        timestamp_text = self.headers.get("X-Frame-Timestamp-Ns", "0")
        try:
            frame_id = int(frame_id_text)
            timestamp_ns = int(timestamp_text)
        except ValueError:
            frame_id = -1
            timestamp_ns = -1
        if not request_id or frame_id < 0 or timestamp_ns < 0:
            self.send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": "invalid request metadata"})
            return
        image = self.rfile.read(size)
        if len(image) != size or len(image) <= 4 or not image.startswith(b"\xff\xd8") or not image.endswith(b"\xff\xd9"):
            self.send_json(HTTPStatus.BAD_REQUEST, {"success": False, "error": "invalid jpeg"})
            return
        if not self.server.recognize_lock.acquire(blocking=False):
            self.send_json(HTTPStatus.TOO_MANY_REQUESTS, {"success": False, "error": "recognition busy"})
            return

        started = time.monotonic()
        try:
            result = self.server.client.analyze_image_bytes(image, "image/jpeg")
            document: dict[str, Any] = {
                "type": "manual_result",
                "source": "manual",
                "request_id": request_id,
                "frame_id": frame_id,
                "frame_timestamp_ns": timestamp_ns,
                "timestamp": datetime.now(timezone.utc).isoformat(),
                "server_latency_ms": int((time.monotonic() - started) * 1000),
                **result,
            }
            try:
                write_json_atomically(self.server.result_path, document)
            except OSError:
                self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {
                    "type": "manual_result", "source": "manual", "request_id": request_id,
                    "frame_id": frame_id, "success": False, "error": "result write failed",
                })
                return
            status = HTTPStatus.OK if result["success"] else HTTPStatus.BAD_GATEWAY
            print("manual request_id={} frame_id={} jpeg_bytes={} success={} latency_ms={}".format(
                request_id, frame_id, size, result["success"], document["server_latency_ms"]), flush=True)
            self.send_json(status, document)
        except Exception:
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {
                "type": "manual_result", "source": "manual", "request_id": request_id,
                "frame_id": frame_id, "success": False, "error": "internal server error",
            })
        finally:
            self.server.recognize_lock.release()

    def do_PUT(self) -> None:
        self.send_json(HTTPStatus.METHOD_NOT_ALLOWED, {"success": False, "error": "method not allowed"})

    def do_DELETE(self) -> None:
        self.do_PUT()


def main() -> int:
    parser = argparse.ArgumentParser(description="Serve manual RV1106 frozen-frame recognition.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9001)
    parser.add_argument("--max-image-bytes", type=int, default=1048576)
    parser.add_argument("--result-path", type=Path, default=Path("/tmp/ai_cam/latest_result.json"))
    parser.add_argument("--model", default=os.environ.get("QWEN_MODEL", "qwen3-vl-flash"))
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.max_image_bytes <= 4:
        parser.error("invalid port or max image size")

    load_qwen_env()
    os.environ["QWEN_MODEL"] = args.model
    server = ManualRecognitionServer((args.host, args.port), Handler,
                                     QwenVisionClient(), args.result_path,
                                     args.max_image_bytes)
    print("Manual recognition listening on {}:{}".format(args.host, args.port), flush=True)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
