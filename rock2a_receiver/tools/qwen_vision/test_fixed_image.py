#!/usr/bin/env python3
"""Run one Qwen vision request against a fixed local image."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import sys
from pathlib import Path

from qwen_vision_client import QwenVisionClient


def load_qwen_env() -> None:
    """Load simple KEY=VALUE entries without printing their values."""
    env_path = Path.home() / ".config" / "ai_cam" / "qwen.env"
    if not env_path.is_file():
        return
    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, separator, value = line.partition("=")
        if not separator or not key or key in os.environ:
            continue
        try:
            values = shlex.split(value, comments=True, posix=True)
        except ValueError:
            continue
        if len(values) == 1:
            os.environ[key] = values[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze one fixed image with Qwen vision.")
    parser.add_argument("--image", required=True, help="JPEG, PNG, or WEBP image path")
    args = parser.parse_args()

    load_qwen_env()
    result = QwenVisionClient().analyze_image(args.image)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
