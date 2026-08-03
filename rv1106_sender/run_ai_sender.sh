#!/bin/sh

set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The wrapped script handles rkipc and forwards Ctrl+C for orderly cleanup.
exec "$APP_DIR/run_simple_isp_vi_to_lcd_rv1106.sh" 180 "$@"
