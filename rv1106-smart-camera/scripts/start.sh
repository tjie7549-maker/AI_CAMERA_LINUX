#!/bin/sh
# Run on the board. It refuses to start a second daemon to avoid camera contention.
set -eu

root=${RV1106_SMART_CAMERA_ROOT:-/userdata/rv1106-smart-camera}
mkdir -p "$root/logs" "$root/run" "$root/captures"
if pidof camera-daemon >/dev/null 2>&1; then
  echo "camera-daemon is already running; refusing a second camera owner." >&2
  exit 1
fi
if pidof rkipc >/dev/null 2>&1; then
  echo "rkipc owns the camera; stop it explicitly before starting this project." >&2
  exit 1
fi
"$root/bin/camera-daemon" "$root/config.json" >>"$root/logs/camera-daemon.log" 2>&1 &
daemon_pid=$!
sleep 1
if ! kill -0 "$daemon_pid" 2>/dev/null; then
  echo "daemon failed; see $root/logs/camera-daemon.log" >&2
  exit 1
fi
echo "daemon pid=$daemon_pid; events=$root/logs/events.jsonl"
export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FB_DRM=1
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event0
exec "$root/bin/rv1106_ai_ui" --daemon-socket "$root/run/camera-daemon.sock" >>"$root/logs/qt-console.log" 2>&1
