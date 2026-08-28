#!/bin/sh
# 当前项目的常驻启动包装器：不依赖旧 /userdata/ai_camera 服务。
set -u

root=${RV1106_SMART_CAMERA_ROOT:-/userdata/rv1106-smart-camera}
flag="$root/.autostart-disabled"
pidfile="$root/run/autostart.pid"
log="$root/logs/autostart.log"
boot_delay=${BOOT_DELAY_SECONDS:-25}

running() {
    [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile" 2>/dev/null)" 2>/dev/null
}

start() {
    mkdir -p "$root/run" "$root/logs"
    if [ -f "$flag" ]; then
        echo "rv1106-smart-camera autostart disabled"
        return 0
    fi
    if running; then
        echo "rv1106-smart-camera autostart already running"
        return 0
    fi
    if pidof camera-daemon >/dev/null 2>&1; then
        echo "camera-daemon already running; leave existing project session unchanged"
        return 0
    fi
    (
        sleep "$boot_delay"
        [ -f "$flag" ] && exit 0
        if pidof camera-daemon >/dev/null 2>&1; then
            exit 0
        fi
        exec sh "$root/scripts/run_demo.sh"
    ) >>"$log" 2>&1 &
    echo $! >"$pidfile"
    echo "rv1106-smart-camera autostart scheduled (pid $!, delay ${boot_delay}s)"
}

stop() {
    if running; then
        pid=$(cat "$pidfile")
        kill -TERM "$pid" 2>/dev/null || true
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 20 ]; do sleep 1; i=$((i + 1)); done
    fi
    rm -f "$pidfile"
    echo "rv1106-smart-camera autostart stopped"
}

status() {
    if [ -f "$flag" ]; then echo "autostart: disabled"; else echo "autostart: enabled"; fi
    if running; then echo "runner: running (pid $(cat "$pidfile"))"; return 0; fi
    if pidof camera-daemon >/dev/null 2>&1; then echo "camera: running (manual or previously started)"; return 0; fi
    echo "camera: stopped"
    return 1
}

case "${1:-}" in
    start) start ;;
    stop) stop ;;
    restart) stop; start ;;
    status) status ;;
    *) echo "Usage: $0 {start|stop|restart|status}" >&2; exit 2 ;;
esac
