#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")
SENDER_DIR="$SCRIPT_DIR"
UI_DIR="$PROJECT_DIR/rv1106_ai_ui"
SENDER_BIN="$SENDER_DIR/out/simple_vi_get_frame_send_vo_rv1106"
UI_BIN="$UI_DIR/build/rv1106_ai_ui"
TARGET=${RV1106_TARGET:-root@172.32.0.93}
TARGET_DIR=/root/userdata/ai_camera
FONT=/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf

if [ ! -x "$SENDER_BIN" ] || [ ! -x "$UI_BIN" ]; then
    echo "Build rv1106_sender and rv1106_ai_ui before deployment." >&2
    exit 1
fi
if [ ! -r "$FONT" ]; then
    echo "CJK font not found: $FONT" >&2
    exit 1
fi

ssh "$TARGET" "mkdir -p '$TARGET_DIR/rv1106_sender' '$TARGET_DIR/rv1106_ai_ui/fonts' '$TARGET_DIR/logs'"

scp "$SENDER_BIN" \
    "$SENDER_DIR/out/run_simple_isp_vi_to_lcd_rv1106.sh" \
    "$SENDER_DIR/out/run_ai_headless_preview.sh" \
    "$TARGET:$TARGET_DIR/rv1106_sender/"
scp "$SENDER_DIR/run_ai_terminal.sh" "$TARGET:$TARGET_DIR/"
scp "$UI_BIN" "$UI_DIR/scripts/run.sh" "$TARGET:$TARGET_DIR/rv1106_ai_ui/"
scp "$FONT" "$TARGET:$TARGET_DIR/rv1106_ai_ui/fonts/"

ssh "$TARGET" "chmod +x '$TARGET_DIR/run_ai_terminal.sh' '$TARGET_DIR/rv1106_sender/'* '$TARGET_DIR/rv1106_ai_ui/'*"

echo "Deployed: $TARGET_DIR"
echo "Start on RV1106: cd $TARGET_DIR && ./run_ai_terminal.sh"
