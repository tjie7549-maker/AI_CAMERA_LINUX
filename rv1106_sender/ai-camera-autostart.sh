#!/bin/sh

PERSIST_ROOT=/userdata
FLAG=$PERSIST_ROOT/ai_camera/.autostart-disabled
SERVICE=/etc/init.d/S95ai-camera

usage() {
    echo "Usage: $0 {disable|enable|status}"
}

case "${1:-}" in
    disable)
        touch "$FLAG" || exit 1
        "$SERVICE" stop
        echo "ai-camera autostart disabled; it will remain stopped after reboot."
        ;;
    enable)
        rm -f "$FLAG"
        if grep -q " $SERVICE " /proc/mounts 2>/dev/null; then
            umount "$SERVICE" || exit 1
        fi
        "$SERVICE" start
        echo "ai-camera autostart enabled and service started."
        ;;
    status)
        if [ -f "$FLAG" ]; then
            echo "ai-camera autostart: disabled"
        else
            echo "ai-camera autostart: enabled"
        fi
        "$SERVICE" status
        ;;
    *)
        usage
        exit 1
        ;;
esac
