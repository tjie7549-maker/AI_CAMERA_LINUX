#!/usr/bin/env python3
"""Optionally import legacy saved-result metadata without altering old files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from event_store import EventStore
from result_cache import SAFE_REQUEST_ID, SOURCES


def inside(root: Path, value: Path) -> bool:
    try:
        return value.resolve().is_relative_to(root.resolve())
    except (OSError, ValueError):
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--index", required=True, type=Path)
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument(
        "--saved-root", type=Path, help="directory containing paths from saved_relative_path"
    )
    args = parser.parse_args()
    if not args.index.is_file():
        parser.error("--index does not exist")
    saved_root = args.saved_root or args.index.parent
    store = EventStore(args.database)
    imported = invalid = 0
    try:
        for line in args.index.read_text(encoding="utf-8").splitlines():
            try:
                item = json.loads(line)
                source = str(item.get("source", ""))
                request_id = str(item.get("request_id", ""))
                if source not in SOURCES or not SAFE_REQUEST_ID.fullmatch(request_id):
                    raise ValueError("unsafe legacy identifier")
                relative = Path(str(item.get("saved_relative_path", "")))
                record_dir = saved_root / relative
                if not inside(saved_root, record_dir):
                    raise ValueError("unsafe legacy path")
                result_path = record_dir / "result.json"
                result = (
                    json.loads(result_path.read_text(encoding="utf-8"))
                    if result_path.is_file()
                    else {}
                )
                document = {
                    "request_id": request_id,
                    "event_id": None,
                    "source": source,
                    "frame_id": int(item.get("frame_id") or result.get("frame_id") or 0),
                    "recognition_backend": "legacy",
                    "success": bool(result.get("success", True)),
                    "server_latency_ms": int(result.get("server_latency_ms", 0)),
                    "usage": result.get("usage", {}),
                    "result": result.get("result", {}),
                }
                store.record_recognition(document)
                imported += 1
            except (OSError, ValueError, TypeError, json.JSONDecodeError):
                invalid += 1
    finally:
        store.close()
    print("legacy rows processed=%d invalid=%d" % (imported, invalid))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
