#!/usr/bin/env bash

set -u

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RV1106_HOST=${RV1106_HOST:-rv1106-ai-camera}
RV1106_DIR=${RV1106_DIR:-/root/userdata/ai_camera}
RV1106_PID_FILE=${RV1106_PID_FILE:-/tmp/ai_camera_linked.pid}
RV1106_LOG=${RV1106_LOG:-/root/userdata/ai_camera/logs/linked_launcher.log}

local_pid=""
stopping=0

usage() {
    cat <<EOF
Usage: $0 [--mode manual|auto] [run_ai_pipeline.sh options]

Start the RV1106 camera/RTSP/Qt terminal and the local ROCK 2A AI pipeline.
Press Ctrl+C once to stop both boards cleanly.

Environment:
  RV1106_HOST      SSH host alias (default: $RV1106_HOST)
  RV1106_DIR       RV1106 deployment directory (default: $RV1106_DIR)
EOF
}

MODE=manual
if [ "${1:-}" = "--mode" ]; then
    MODE=${2:-}
    shift 2
elif [ "${1:-}" = --mode=* ]; then
    MODE=${1#--mode=}
    shift
fi
case "$MODE" in manual|auto) ;; *) echo "Invalid mode: $MODE" >&2; exit 1 ;; esac

remote_run() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$RV1106_HOST" "$@"
}

remote_is_running() {
    remote_run "
        test -r '$RV1106_PID_FILE' || exit 1
        read pid < '$RV1106_PID_FILE'
        case \"\$pid\" in ''|*[!0-9]*) exit 1 ;; esac
        kill -0 \"\$pid\" 2>/dev/null
    " >/dev/null 2>&1
}

start_remote() {
    remote_run "
        set -eu
        if pidof simple_vi_get_frame_send_vo_rv1106 >/dev/null 2>&1 ||
           pidof rv1106_ai_ui >/dev/null 2>&1; then
            echo 'RV1106 camera or Qt is already running.' >&2
            exit 2
        fi
        rm -f '$RV1106_PID_FILE'
        cd '$RV1106_DIR'
        setsid sh -c 'echo \$\$ > \"$RV1106_PID_FILE\"; exec ./run_ai_terminal.sh' \
            >'$RV1106_LOG' 2>&1 < /dev/null &
    "
}

stop_remote() {
    remote_run "
        if test -r '$RV1106_PID_FILE'; then
            read pid < '$RV1106_PID_FILE'
            case \"\$pid\" in ''|*[!0-9]*) exit 0 ;; esac
            if kill -0 \"\$pid\" 2>/dev/null; then
                kill -TERM \"\$pid\"
            fi
        fi
    " >/dev/null 2>&1 || true

    for _ in $(seq 1 15); do
        if ! remote_is_running; then
            return 0
        fi
        sleep 1
    done

    echo "RV1106 shutdown is still in progress; see $RV1106_LOG" >&2
    return 1
}

stop_local() {
    if [ -n "$local_pid" ] && kill -0 "$local_pid" 2>/dev/null; then
        kill -TERM "$local_pid" 2>/dev/null || true
    fi
    if [ -n "$local_pid" ]; then
        wait "$local_pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_local
    stop_remote || true
}

on_signal() {
    if [ "$stopping" -ne 0 ]; then
        return
    fi
    stopping=1
    trap '' INT TERM
    echo
    echo "Stopping ROCK 2A AI pipeline and RV1106 terminal..."
    cleanup
    exit 130
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi

if ! command -v setsid >/dev/null 2>&1; then
    echo "setsid is required on ROCK 2A." >&2
    exit 1
fi

trap on_signal INT TERM

echo "Starting RV1106 terminal through $RV1106_HOST..."
start_remote

for _ in $(seq 1 20); do
    if remote_run "test -S /tmp/ai_cam_preview.sock" >/dev/null 2>&1; then
        break
    fi
    if ! remote_is_running; then
        echo "RV1106 terminal exited during startup. See $RV1106_LOG" >&2
        exit 1
    fi
    sleep 1
done

if ! remote_run "test -S /tmp/ai_cam_preview.sock" >/dev/null 2>&1; then
    echo "RV1106 preview socket did not become ready. See $RV1106_LOG" >&2
    stop_remote || true
    exit 1
fi

echo "Starting ROCK 2A $MODE AI pipeline..."
if [ "$MODE" = "manual" ]; then
    setsid "$PROJECT_DIR/run_manual_ai_pipeline.sh" "$@" &
else
    setsid "$PROJECT_DIR/run_ai_pipeline.sh" "$@" &
fi
local_pid=$!

while kill -0 "$local_pid" 2>/dev/null; do
    if ! remote_is_running; then
        echo "RV1106 terminal stopped unexpectedly." >&2
        stop_local
        exit 1
    fi
    sleep 1
done

if wait "$local_pid"; then
    local_status=0
else
    local_status=$?
fi
local_pid=""
stop_remote || true
exit "$local_status"
