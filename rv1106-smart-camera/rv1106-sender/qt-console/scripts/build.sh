#!/bin/sh

set -eu

QMAKE=/home/summary/linux/luckfox-pico/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")
BUILD_DIR="$PROJECT_DIR/build"

if [ ! -x "$QMAKE" ]; then
    echo "qmake not found: $QMAKE" >&2
    exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

"$QMAKE" ../rv1106_ai_ui.pro
make -j"$(nproc)"

file rv1106_ai_ui
readelf -l rv1106_ai_ui | grep interpreter
readelf -d rv1106_ai_ui | grep NEEDED
