#!/usr/bin/env python3
"""Send the latest AI result to one RV1106 client as newline-delimited JSON."""

import argparse
import json
import select
import socket
import sys
import time
from pathlib import Path
from typing import Any, Optional, Tuple


SENSITIVE_KEYS = {
    "api_key",
    "apikey",
    "authorization",
    "access_token",
    "secret",
    "qwen_api_key",
}
MAX_MESSAGE_BYTES = 64 * 1024


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send an updated AI result file as newline-delimited JSON"
    )
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--extra-input", action="append", type=Path, default=[],
                        help="additional result files to stream (repeatable)")
    parser.add_argument("--host", default="192.168.50.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument(
        "--primary-hold-seconds",
        type=float,
        default=10.0,
        help="hold the primary result on screen before sending extra inputs",
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.poll_interval <= 0:
        parser.error("--poll-interval must be positive")
    if args.primary_hold_seconds < 0:
        parser.error("--primary-hold-seconds must be non-negative")
    return args


def sanitize(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: sanitize(item)
            for key, item in value.items()
            if key.lower() not in SENSITIVE_KEYS
        }
    if isinstance(value, list):
        return [sanitize(item) for item in value]
    return value


def file_fingerprint(path: Path) -> Optional[Tuple[int, int]]:
    try:
        status = path.stat()
    except FileNotFoundError:
        return None
    return status.st_mtime_ns, status.st_size


def load_message(path: Path) -> Optional[bytes]:
    try:
        with path.open("r", encoding="utf-8") as source:
            payload = json.load(source)
    except FileNotFoundError:
        return None
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"Input error: {error}", file=sys.stderr, flush=True)
        return None

    clean_payload = sanitize(payload)
    message = (
        json.dumps(clean_payload, ensure_ascii=False, separators=(",", ":"))
        + "\n"
    ).encode("utf-8")
    if len(message) > MAX_MESSAGE_BYTES:
        print(f"Input error: {path} exceeds 64 KiB", file=sys.stderr, flush=True)
        return None
    return message


def client_closed(client: socket.socket, timeout: float) -> bool:
    readable, _, _ = select.select([client], [], [], timeout)
    if not readable:
        return False
    try:
        data = client.recv(1, socket.MSG_PEEK)
    except (ConnectionError, OSError):
        return True
    if not data:
        return True
    client.recv(4096)
    return False


def serve_client(client: socket.socket, address: Tuple[str, int],
                 input_paths: list, poll_interval: float,
                 primary_hold_seconds: float) -> None:
    print(f"Client connected: {address[0]}:{address[1]}", flush=True)
    last_fingerprints: dict = {}
    primary_path = input_paths[0]
    primary_hold_until = 0.0
    while True:
        if client_closed(client, poll_interval):
            print("Client disconnected", flush=True)
            return

        for input_path in input_paths:
            if input_path != primary_path and time.monotonic() < primary_hold_until:
                continue
            fingerprint = file_fingerprint(input_path)
            if fingerprint is None or fingerprint == last_fingerprints.get(input_path):
                continue
            last_fingerprints[input_path] = fingerprint
            message = load_message(input_path)
            if message is None:
                continue
            try:
                client.sendall(message)
            except (BrokenPipeError, ConnectionResetError, OSError):
                print("Client disconnected during send", flush=True)
                return
            print(f"Sent {len(message)} bytes from {input_path}", flush=True)
            if input_path == primary_path:
                primary_hold_until = time.monotonic() + primary_hold_seconds


def run_server(host: str, port: int, input_paths: list,
               poll_interval: float, primary_hold_seconds: float) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        server.settimeout(1.0)
        print(f"Listening on {host}:{port}", flush=True)
        for input_path in input_paths:
            print(f"Watching {input_path}", flush=True)

        while True:
            try:
                client, address = server.accept()
            except socket.timeout:
                continue
            with client:
                client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                client.settimeout(1.0)
                serve_client(
                    client,
                    address,
                    input_paths,
                    poll_interval,
                    primary_hold_seconds,
                )


def main() -> int:
    args = parse_args()
    input_paths = [args.input] + list(args.extra_input)
    try:
        run_server(
            args.host,
            args.port,
            input_paths,
            args.poll_interval,
            args.primary_hold_seconds,
        )
    except KeyboardInterrupt:
        print("\nStopped", flush=True)
        return 0
    except OSError as error:
        print(f"Server error: {error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
