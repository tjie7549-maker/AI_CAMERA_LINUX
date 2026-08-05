#!/bin/sh

set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$APP_DIR/rv1106_ai_ui"

if [ ! -x "$APP" ]; then
    echo "Application not found or not executable: $APP" >&2
    exit 1
fi

for process in ai_cam rkipc; do
    if pidof "$process" >/dev/null 2>&1; then
        echo "$process is running and may own the LCD." >&2
        echo "Stop camera/VO services manually before starting Qt." >&2
        exit 1
    fi
done

for pid in $(pidof simple_vi_get_frame_send_vo_rv1106 2>/dev/null || true); do
    cmdline=$(tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
    case "$cmdline" in
        *"--no-vo"*) ;;
        *)
            echo "Camera process $pid owns VO. Start it with --no-vo before Qt." >&2
            exit 1
            ;;
    esac
done

backlight_power=/sys/class/backlight/backlight/bl_power
if [ -w "$backlight_power" ]; then
    echo 0 > "$backlight_power"
fi

export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FB_DRM=1
export QT_QPA_FONTDIR="$APP_DIR/fonts"
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event0

cd "$APP_DIR" || exit 1

exec ./rv1106_ai_ui \
    --server-ip "${SERVER_IP:-192.168.50.1}" \
    --server-port "${SERVER_PORT:-9000}" \
    --preview-shm "${PREVIEW_SHM:-/ai_cam_preview}" \
    --preview-timeout-ms "${PREVIEW_TIMEOUT_MS:-1000}" \
    "$@"
