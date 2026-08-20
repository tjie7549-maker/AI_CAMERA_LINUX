#!/bin/sh
# Short, reversible board-side LCD test.
#
# Safety contract: this script never stops rkipc, DHCP, or any other service it
# did not start.  Release the camera explicitly before running it.  This keeps
# USB/SSH networking independent of a camera demonstration.
set -u

root=/userdata/rv1106-smart-camera
duration=${1:-8}
case "$duration" in *[!0-9]*|'') echo "Usage: $0 [seconds]" >&2; exit 2;; esac
qt_pid= daemon_pid=
cleanup() {
  [ -n "$qt_pid" ] && kill -TERM "$qt_pid" 2>/dev/null || true
  [ -n "$daemon_pid" ] && kill -TERM "$daemon_pid" 2>/dev/null || true
  sleep 3
  for pid in $(pidof simple_vi_get_frame_send_vo_rv1106 2>/dev/null || true); do
    cmdline=$(tr '\000' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)
    case "$cmdline" in *"--no-vo"*) kill -TERM "$pid" 2>/dev/null || true;; esac
  done
}
trap cleanup EXIT INT TERM HUP

if pidof rkipc >/dev/null 2>&1; then
  echo "rkipc owns the camera. Stop it explicitly, then rerun this test." >&2
  exit 3
fi
if fuser /dev/video11 /dev/video0 >/dev/null 2>&1 || \
   netstat -ltn 2>/dev/null | grep -q '[:.]554[[:space:]]'; then
  echo "Camera or RTSP port 554 is still owned by another process; refusing to stop it." >&2
  exit 3
fi

"$root/bin/camera-daemon" "$root/config.json" >>"$root/logs/camera-daemon.log" 2>&1 &
daemon_pid=$!
waited=0
until "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" '{"cmd":"get_status"}' >/dev/null 2>&1; do
  sleep 1; waited=$((waited + 1))
  [ "$waited" -lt 10 ] || { echo "daemon Socket did not become ready" >&2; exit 4; }
done

export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FB_DRM=1
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event0
"$root/bin/rv1106_ai_ui" --daemon-socket "$root/run/camera-daemon.sock" >>"$root/logs/qt-console.log" 2>&1 &
qt_pid=$!
sleep "$duration"
if kill -0 "$qt_pid" 2>/dev/null; then
  echo "Qt console stayed alive for ${duration}s"
else
  echo "Qt console exited early; inspect $root/logs/qt-console.log" >&2
  exit 5
fi
