#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")
APP="$PROJECT_DIR/build/rv1106_ai_ui"
TARGET=${RV1106_TARGET:-root@172.32.0.93}
TARGET_DIR=/root/userdata/rv1106_ai_ui
FONT=/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf

if [ ! -x "$APP" ]; then
    echo "Application not found: $APP" >&2
    echo "Run scripts/build.sh first." >&2
    exit 1
fi

if [ ! -r "$FONT" ]; then
    echo "CJK font not found: $FONT" >&2
    echo "Install the Debian fonts-droid-fallback package first." >&2
    exit 1
fi

ssh "$TARGET" "mkdir -p '$TARGET_DIR/fonts'"
scp "$APP" "$PROJECT_DIR/scripts/run.sh" "$PROJECT_DIR/README.md" \
    "$TARGET:$TARGET_DIR/"
scp "$FONT" "$TARGET:$TARGET_DIR/fonts/"
ssh "$TARGET" "chmod +x '$TARGET_DIR/rv1106_ai_ui' '$TARGET_DIR/run.sh'"
