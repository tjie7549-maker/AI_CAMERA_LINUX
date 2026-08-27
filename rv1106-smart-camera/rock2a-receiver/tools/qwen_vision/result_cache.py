"""Atomic result cache and source-separated persistent result storage."""
from __future__ import annotations

import json
import os
import re
import shutil
import tempfile
import time
import fcntl
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SAFE_REQUEST_ID = re.compile(r"^[A-Za-z0-9_-]{1,128}$")
SOURCES = {"manual", "auto"}

def valid(source: str, request_id: str) -> bool:
    return source in SOURCES and bool(SAFE_REQUEST_ID.fullmatch(request_id))

def _write(path: Path, data: bytes) -> None:
    with path.open("wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())

def cache(root: Path, source: str, request_id: str, image: bytes,
          result: dict[str, Any], metadata: dict[str, Any]) -> Path:
    if not valid(source, request_id):
        raise ValueError("invalid source or request id")
    base = root / "result_cache" / source
    base.mkdir(parents=True, exist_ok=True)
    target = base / request_id
    if target.is_dir():
        return target
    temporary = Path(tempfile.mkdtemp(prefix=request_id + ".tmp.", dir=base))
    try:
        _write(temporary / "image.jpg", image)
        _write(temporary / "result.json", json.dumps(result, ensure_ascii=False).encode() + b"\n")
        _write(temporary / "metadata.json", json.dumps(metadata, ensure_ascii=False).encode() + b"\n")
        os.replace(temporary, target)
    finally:
        if temporary.exists(): shutil.rmtree(temporary, ignore_errors=True)
    entries = sorted((item for item in base.iterdir() if item.is_dir()), key=lambda item: item.stat().st_mtime)
    cutoff = time.time() - 24 * 3600
    for item in list(entries):
        if item == target:
            continue
        if item.stat().st_mtime < cutoff or len(entries) > 100:
            shutil.rmtree(item, ignore_errors=True)
            entries.remove(item)
    return target

def save(root: Path, source: str, request_id: str) -> tuple[str, bool]:
    if not valid(source, request_id): raise ValueError("invalid source or request id")
    record = root / "result_cache" / source / request_id
    if not all((record / name).is_file() for name in ("image.jpg", "result.json", "metadata.json")):
        raise FileNotFoundError(request_id)
    result = json.loads((record / "result.json").read_text(encoding="utf-8"))
    if result.get("source") != source or result.get("request_id") != request_id:
        raise ValueError("cached result mismatch")
    index_path = root / "saved_results" / "index.jsonl"
    index_path.parent.mkdir(parents=True, exist_ok=True)
    # The index is the durable idempotency record, shared by concurrent HTTP workers.
    with index_path.open("a+", encoding="utf-8") as index:
        fcntl.flock(index.fileno(), fcntl.LOCK_EX)
        index.seek(0)
        for line in index:
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if item.get("source") == source and item.get("request_id") == request_id:
                fcntl.flock(index.fileno(), fcntl.LOCK_UN)
                return str(item["saved_relative_path"]), True

        now = datetime.now(timezone.utc)
        relative = Path(source) / now.strftime("%Y-%m-%d") / (now.strftime("%H%M%S") + "_" + request_id)
        target = root / "saved_results" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        temp = Path(tempfile.mkdtemp(prefix=target.name + ".tmp.", dir=target.parent))
        try:
            for name in ("image.jpg", "result.json", "metadata.json"):
                shutil.copy2(record / name, temp / name)
                with (temp / name).open("rb") as value:
                    os.fsync(value.fileno())
            os.replace(temp, target)
        finally:
            if temp.exists():
                shutil.rmtree(temp, ignore_errors=True)

        index.seek(0, os.SEEK_END)
        index.write(json.dumps({"saved_at": now.isoformat(), "source": source, "request_id": request_id,
            "frame_id": result.get("frame_id"), "scene": result.get("result", {}).get("scene"),
            "warning": result.get("result", {}).get("warning"), "saved_relative_path": str(relative)}, ensure_ascii=False) + "\n")
        index.flush()
        os.fsync(index.fileno())
        fcntl.flock(index.fileno(), fcntl.LOCK_UN)
    return str(relative), False
