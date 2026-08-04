#!/bin/sh

set -eu

APP_DIR=/root/userdata/rv1106_ai_ui
APP="$APP_DIR/rv1106_ai_ui"

if [ ! -x "$APP" ]; then
    echo "Application not found or not executable: $APP" >&2
    exit 1
fi

for process in ai_cam simple_vi_get_frame_send_vo_rv1106 rkipc; do
    if pidof "$process" >/dev/null 2>&1; then
        echo "$process is running and may own the LCD." >&2
        echo "Stop camera/VO services manually before starting Qt." >&2
        exit 1
    fi
done

echo "Qt and ai_cam/rkipc VO must not use the LCD at the same time."

backlight_power=/sys/class/backlight/backlight/bl_power
if [ -w "$backlight_power" ]; then
    echo 0 > "$backlight_power"
fi

export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FB_DRM=1
export QT_QPA_FONTDIR=/usr/share/fonts/dejavu

cd "$APP_DIR" || exit 1

exec ./rv1106_ai_ui \
    --server-ip "${SERVER_IP:-192.168.50.1}" \
    --server-port "${SERVER_PORT:-9000}"
