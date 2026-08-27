#!/bin/sh
# The project owns SC3336 while this demo is running; rkipc is restored only
# after the daemon stops. Ctrl+C never signals camera-daemon directly.
set -u

root=${RV1106_SMART_CAMERA_ROOT:-/userdata/rv1106-smart-camera}
daemon_pid=
ui_pid=
cleaned=0

wait_for_exit() {
  target=$1
  count=0
  while kill -0 "$target" 2>/dev/null && [ "$count" -lt 50 ]; do
    sleep 1
    count=$((count + 1))
  done
}

start_daemon() {
  setsid "$root/bin/camera-daemon" "$root/config.json" >>"$root/logs/camera-daemon.log" 2>&1 &
  daemon_pid=$!
  for n in 1 2 3 4 5 6 7 8; do
    "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" '{"cmd":"get_status"}' >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1
}

cleanup() {
  [ "$cleaned" -eq 0 ] || return
  cleaned=1
  trap - EXIT INT TERM HUP

  if [ -n "$ui_pid" ] && kill -0 "$ui_pid" 2>/dev/null; then
    kill -TERM "$ui_pid" 2>/dev/null || true
    wait_for_exit "$ui_pid"
  fi

  # This request only leaves the parameter page; daemon shutdown below releases
  # the project media children and then restores rkipc.
  "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" \
    '{"cmd":"exit_debug"}' >/dev/null 2>&1 || true

  if [ -n "$daemon_pid" ] && kill -0 "$daemon_pid" 2>/dev/null; then
    kill -TERM "$daemon_pid" 2>/dev/null || true
    wait_for_exit "$daemon_pid"
  fi
}

on_signal() { exit 130; }
trap cleanup EXIT
trap on_signal INT TERM HUP

if pidof camera-daemon >/dev/null 2>&1; then
  echo "camera-daemon is already running; refuse a second demo instance." >&2
  exit 2
fi

# A separate session keeps Ctrl+C away from the daemon and its media children.
start_daemon || {
  echo "camera-daemon did not become ready; see $root/logs/camera-daemon.log" >&2
  exit 4
}

export QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms
export QT_QPA_PLATFORM=linuxfb
export QT_QPA_FB_DRM=1
export QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS=/dev/input/event0
"$root/bin/rv1106_ai_ui" --daemon-socket "$root/run/camera-daemon.sock" &
ui_pid=$!
while kill -0 "$ui_pid" 2>/dev/null; do
  if ! "$root/bin/camera-daemon" --request "$root/run/camera-daemon.sock" '{"cmd":"get_status"}' >/dev/null 2>&1; then
    if ! pidof camera-daemon >/dev/null 2>&1; then
      echo "camera-daemon exited; restarting preview backend" >&2
      start_daemon || echo "camera-daemon restart failed; will retry" >&2
    fi
  fi
  sleep 2
done
wait "$ui_pid" || true
ui_pid=
