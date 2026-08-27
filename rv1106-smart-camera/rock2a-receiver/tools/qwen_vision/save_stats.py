#!/usr/bin/env python3
"""Periodic save-directory statistics for acceptance and monitoring."""

import json
import shutil
import time
from pathlib import Path

ROOT = Path("/home/radxa/AI_CAMERA_LINUX/rock2a_receiver/runtime")


def count_images(base: Path) -> int:
    if not base.exists():
        return 0
    return len(list(base.rglob("image.jpg")))


def dir_size_mb(path: Path) -> float:
    if not path.exists():
        return 0.0
    total = sum(item.stat().st_size for item in path.rglob("*") if item.is_file())
    return round(total / (1024 * 1024), 2)


def main() -> None:
    saved = ROOT / "saved_results"
    cache = ROOT / "result_cache"
    stats = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "saved_total_mb": dir_size_mb(saved),
        "manual_count": count_images(saved / "manual"),
        "auto_count": count_images(saved / "auto"),
        "result_cache_mb": dir_size_mb(cache),
        "result_cache_manual": count_images(cache / "manual"),
        "result_cache_auto": count_images(cache / "auto"),
        "free_mb": round(shutil.disk_usage(ROOT).free / (1024 * 1024), 1),
    }
    print(json.dumps(stats, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
