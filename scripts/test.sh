#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

echo "[1/6] Python syntax"
python3 -m compileall -q rock2a_receiver/tools/qwen_vision rock2a_receiver/tools/face_attendance rock2a_receiver/tests

echo "[2/6] Python unit tests"
python3 -m unittest discover -s rock2a_receiver/tests -p 'test_*.py' -v
python3 -m unittest discover -s rock2a_receiver/tools/face_attendance/tests -p 'test_*.py' -v

echo "[3/6] Shell syntax"
while IFS= read -r script; do
    case "$(head -n 1 "$script")" in
        *bash*) bash -n "$script" ;;
        *) sh -n "$script" ;;
    esac
done < <(find . -path './.git' -prune -o -path '*/build' -prune -o -type f -name '*.sh' -print)

echo "[4/6] Host IoU tracker"
make -C rv1106_sender tracker-test

echo "[5/6] RV1106 cross builds"
make -C rv1106_sender npu
rv1106_ai_ui/scripts/build.sh >/tmp/ai-camera-qt-build.log
tail -n 12 /tmp/ai-camera-qt-build.log

echo "[6/6] ROCK receiver build"
if pkg-config --exists libavformat libavcodec libavutil libswscale; then
    cmake -S rock2a_receiver -B /tmp/ai-camera-rock-build
    cmake --build /tmp/ai-camera-rock-build -j2
else
    echo "SKIP: local FFmpeg development packages are unavailable; build on ROCK 2A."
fi

echo "All available checks passed."
