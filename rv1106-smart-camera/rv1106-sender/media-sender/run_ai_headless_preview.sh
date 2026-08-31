#!/bin/sh

set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec "$APP_DIR/run_simple_isp_vi_to_lcd_rv1106.sh" 180 \
    --no-vo \
    --preview-shm "${PREVIEW_SHM:-/ai_cam_preview}" \
    --preview-width "${PREVIEW_WIDTH:-384}" \
    --preview-height "${PREVIEW_HEIGHT:-216}" \
    --preview-fps "${PREVIEW_FPS:-15}" \
    "$@"
