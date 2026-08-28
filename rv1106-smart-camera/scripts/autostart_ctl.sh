#!/bin/sh
# 板端手动开关。disable 会安全停止当前项目；enable 只立即调度当前项目，不影响旧项目。
set -eu

root=${RV1106_SMART_CAMERA_ROOT:-/userdata/rv1106-smart-camera}
service="$root/scripts/autostart_service.sh"
flag="$root/.autostart-disabled"

case "${1:-}" in
    disable)
        touch "$flag"
        "$service" stop
        echo "autostart disabled; it will remain off after reboot"
        ;;
    enable)
        rm -f "$flag"
        "$service" start
        echo "autostart enabled"
        ;;
    status) "$service" status ;;
    *) echo "Usage: $0 {enable|disable|status}" >&2; exit 2 ;;
esac
