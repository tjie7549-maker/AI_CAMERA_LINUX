#!/usr/bin/env python3
"""Authenticated HTTP service for RV1106 face-attendance requests."""

from __future__ import annotations

import argparse
import hmac
import json
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

try:  # Supports both `python service.py` on ROCK and module execution in tests.
    from .protocol import MAX_FACE_JPEG_BYTES, parse_verify_request
    from .store import AttendanceStore
    from .verifier import FaceVerifier, OpenCvLbphVerifier, UnavailableVerifier
except ImportError:
    from protocol import MAX_FACE_JPEG_BYTES, parse_verify_request
    from store import AttendanceStore
    from verifier import FaceVerifier, OpenCvLbphVerifier, UnavailableVerifier


class AttendanceServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], store: AttendanceStore,
                 verifier: FaceVerifier, token: str) -> None:
        super().__init__(address, AttendanceHandler)
        self.store = store
        self.verifier = verifier
        self.token = token.encode("utf-8")
        self.started_at = time.monotonic()


class AttendanceHandler(BaseHTTPRequestHandler):
    server: AttendanceServer

    def log_message(self, *_: Any) -> None:
        pass

    def send_json(self, status: int, document: dict[str, Any]) -> None:
        data = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def authorized(self) -> bool:
        supplied = self.headers.get("X-Attendance-Token", "").encode("utf-8")
        return bool(supplied) and hmac.compare_digest(supplied, self.server.token)

    def do_GET(self) -> None:
        if self.path != "/v1/face/health":
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        self.send_json(HTTPStatus.OK, {"status": "ok", "uptime_seconds": int(time.monotonic() - self.server.started_at)})

    def do_POST(self) -> None:
        if self.path != "/v1/face/verify":
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        if not self.authorized():
            self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "unauthorized"})
            return
        if self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower() != "image/jpeg":
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "content type must be image/jpeg"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if not 1 <= length <= MAX_FACE_JPEG_BYTES:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"error": "invalid image size"})
            return
        try:
            request = parse_verify_request({key.lower(): value for key, value in self.headers.items()}, self.rfile.read(length))
        except ValueError as error:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})
            return
        verification = self.server.verifier.verify(request.image)
        if verification.person_id is None:
            unavailable = verification.message == "face verifier is not configured"
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE if unavailable else HTTPStatus.OK, {
                "request_id": request.request_id,
                "status": "unrecognized" if not unavailable else "unavailable",
                "message": verification.message,
                "score": verification.score,
            })
            return
        try:
            outcome = self.server.store.record(request.request_id, verification.person_id,
                                               request.attendance_type, verification.score,
                                               request.camera_id, int(time.time() * 1000))
        except LookupError as error:
            self.send_json(HTTPStatus.OK, {"request_id": request.request_id, "status": "unrecognized", "message": str(error)})
            return
        self.send_json(HTTPStatus.OK, {
            "request_id": request.request_id,
            "status": outcome.status,
            "person_id": outcome.person.person_id,
            "name": outcome.person.name,
            "role": outcome.person.role,
            "attendance_type": outcome.attendance_type,
            "checked_at_ms": outcome.checked_at_ms,
            "score": outcome.score,
            "message": "attendance recorded" if outcome.status == "recorded" else "already recorded today",
        })


def main() -> int:
    parser = argparse.ArgumentParser(description="ROCK 2A face-attendance service")
    parser.add_argument("--host", default="192.168.50.1")
    parser.add_argument("--port", type=int, default=9012)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--timezone", default="Asia/Shanghai")
    parser.add_argument("--lbph-model", type=Path)
    parser.add_argument("--lbph-threshold", type=float, default=65.0)
    args = parser.parse_args()
    token = args.token_file.read_text(encoding="utf-8").strip()
    if len(token) < 16:
        parser.error("token file must contain at least 16 characters")
    verifier: FaceVerifier = UnavailableVerifier()
    if args.lbph_model:
        try:
            verifier = OpenCvLbphVerifier(args.lbph_model, args.lbph_threshold)
        except RuntimeError as error:
            parser.error(str(error))
    store = AttendanceStore(args.database, args.timezone)
    server = AttendanceServer((args.host, args.port), store, verifier, token)
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
