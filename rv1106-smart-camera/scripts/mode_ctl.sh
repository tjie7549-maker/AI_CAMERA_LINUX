#!/bin/sh
# UI 使用同一 socket；该脚本用于无 UI 的演示、复现和排障。
set -eu
root=${RV1106_SMART_CAMERA_ROOT:-/userdata/rv1106-smart-camera}
client="$root/bin/camera-daemon"
socket="$root/run/camera-daemon.sock"

usage() { echo "Usage: $0 status|debug|display|auto|manual|restore" >&2; exit 2; }
case "${1:-}" in
  status) request='{"cmd":"get_status"}' ;;
  debug) request='{"cmd":"enter_debug"}' ;;
  display) request='{"cmd":"exit_debug"}' ;;
  auto) request='{"cmd":"set_auto_ae","auto_ae":true}' ;;
  manual) request='{"cmd":"set_auto_ae","auto_ae":false}' ;;
  restore) request='{"cmd":"restore_defaults"}' ;;
  *) usage ;;
esac
exec "$client" --request "$socket" "$request"
