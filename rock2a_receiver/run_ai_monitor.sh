#!/usr/bin/env bash

set -u

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RECEIVER="$PROJECT_DIR/build/rock2a_rtsp_receiver"
PYTHON="$PROJECT_DIR/.venv-qwen/bin/python"
ENV_FILE="$HOME/.config/ai_cam/qwen.env"
URL="rtsp://192.168.50.2:554/live/1"
DURATION=300
FRAME_DIR="$PROJECT_DIR/artifacts/frames/ai_monitor"
RUNTIME_DIR="$PROJECT_DIR/runtime/ai_cam"
LATEST_IMAGE="$RUNTIME_DIR/latest.jpg"
LATEST_RESULT="$RUNTIME_DIR/latest_result.json"
RECEIVER_LOG="$RUNTIME_DIR/receiver.log"
WATCHER_LOG="$RUNTIME_DIR/qwen_watch.log"
TCP_LOG="$RUNTIME_DIR/result_tcp.log"

usage() {
    cat <<EOF
Usage: $0 [options]

Start RTSP receiving, Qwen monitoring, and RV1106 TCP result delivery.

Options:
  --duration SECONDS  Test duration (default: $DURATION; 0 runs until Ctrl+C)
  --url URL           RTSP sub-stream URL (default: $URL)
  --help              Show this help
EOF
}

require_value() {
    if [ "$#" -lt 2 ] || [ -z "$2" ]; then
        echo "Missing value for $1" >&2
        exit 1
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --duration)
            require_value "$@"
            DURATION=$2
            shift
            ;;
        --url)
            require_value "$@"
            URL=$2
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

case "$DURATION" in
    ''|*[!0-9]*)
        echo "--duration must be a non-negative integer" >&2
        exit 1
        ;;
esac

if [ ! -x "$RECEIVER" ]; then
    echo "Missing receiver: $RECEIVER" >&2
    exit 1
fi
if [ ! -x "$PYTHON" ]; then
    echo "Missing Python environment: $PYTHON" >&2
    exit 1
fi
if [ ! -f "$ENV_FILE" ]; then
    echo "Missing Qwen environment file: $ENV_FILE" >&2
    exit 1
fi

mkdir -p "$FRAME_DIR" "$RUNTIME_DIR"

# These two files are runtime outputs for this invocation. Clearing them keeps
# an earlier run from being displayed or sent to the API as a fresh frame.
rm -f "$LATEST_IMAGE" "$LATEST_RESULT"

# qwen.env remains outside the repository and is never printed by this script.
set -a
. "$ENV_FILE"
set +a

receiver_pid=""
watcher_pid=""
tcp_pid=""

stop_children() {
    if [ -n "$tcp_pid" ] && kill -0 "$tcp_pid" 2>/dev/null; then
        kill -INT "$tcp_pid" 2>/dev/null || true
    fi
    if [ -n "$watcher_pid" ] && kill -0 "$watcher_pid" 2>/dev/null; then
        kill -INT "$watcher_pid" 2>/dev/null || true
    fi
    if [ -n "$receiver_pid" ] && kill -0 "$receiver_pid" 2>/dev/null; then
        kill -INT "$receiver_pid" 2>/dev/null || true
    fi
    if [ -n "$watcher_pid" ]; then
        wait "$watcher_pid" 2>/dev/null || true
    fi
    if [ -n "$tcp_pid" ]; then
        wait "$tcp_pid" 2>/dev/null || true
    fi
    if [ -n "$receiver_pid" ]; then
        wait "$receiver_pid" 2>/dev/null || true
    fi
}

receiver_is_running() {
    if ! kill -0 "$receiver_pid" 2>/dev/null; then
        return 1
    fi

    # kill -0 succeeds for an exited child until wait() reaps its zombie PID.
    # Treat that state as stopped so a failed RTSP connection cannot leave the
    # monitor loop waiting forever.
    receiver_state=$(ps -o stat= -p "$receiver_pid" 2>/dev/null || true)
    case "$receiver_state" in
        *Z*) return 1 ;;
    esac
    return 0
}

tcp_is_running() {
    if ! kill -0 "$tcp_pid" 2>/dev/null; then
        return 1
    fi
    tcp_state=$(ps -o stat= -p "$tcp_pid" 2>/dev/null || true)
    case "$tcp_state" in
        *Z*) return 1 ;;
    esac
    return 0
}

on_signal() {
    echo
    echo "Stopping receiver, Qwen monitor, and TCP result server..."
    stop_children
    exit 130
}

trap on_signal INT TERM

"$RECEIVER" \
    --url "$URL" \
    --output "$FRAME_DIR" \
    --interval-ms 5000 \
    --latest-image "$LATEST_IMAGE" \
    --latest-interval-ms 3000 \
    --duration "$DURATION" >"$RECEIVER_LOG" 2>&1 &
receiver_pid=$!

"$PYTHON" "$PROJECT_DIR/tools/qwen_vision/watch_latest_image.py" \
    --image "$LATEST_IMAGE" \
    --result "$LATEST_RESULT" \
    --interval-ms 5000 >"$WATCHER_LOG" 2>&1 &
watcher_pid=$!

"$PYTHON" "$PROJECT_DIR/tools/qwen_vision/send_result_tcp.py" \
    --input "$LATEST_RESULT" \
    --host "${RESULT_HOST:-0.0.0.0}" \
    --port "${RESULT_PORT:-9000}" >"$TCP_LOG" 2>&1 &
tcp_pid=$!

sleep 1
if ! tcp_is_running; then
    echo "TCP result server failed to start. See: $TCP_LOG" >&2
    stop_children
    exit 1
fi

echo "Monitoring started. Press Ctrl+C to stop."
echo "Result: $LATEST_RESULT"
echo "Logs: $RECEIVER_LOG, $WATCHER_LOG, $TCP_LOG"
echo "TCP result server: ${RESULT_HOST:-0.0.0.0}:${RESULT_PORT:-9000}"

last_fingerprint=""
while receiver_is_running && tcp_is_running; do
    if [ -f "$LATEST_RESULT" ]; then
        fingerprint=$(stat -c '%Y:%s' "$LATEST_RESULT" 2>/dev/null || true)
        if [ -n "$fingerprint" ] && [ "$fingerprint" != "$last_fingerprint" ]; then
            last_fingerprint=$fingerprint
            clear
            echo "Latest Qwen result"
            "$PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    print(json.dumps(json.load(source), ensure_ascii=False, indent=2))
' "$LATEST_RESULT" || cat "$LATEST_RESULT"
        fi
    fi
    sleep 1
done

wait "$receiver_pid"
receiver_status=$?
receiver_pid=""

if [ -n "$watcher_pid" ] && kill -0 "$watcher_pid" 2>/dev/null; then
    kill -INT "$watcher_pid" 2>/dev/null || true
fi
if [ -n "$watcher_pid" ]; then
    wait "$watcher_pid" 2>/dev/null || true
fi
if [ -n "$tcp_pid" ] && kill -0 "$tcp_pid" 2>/dev/null; then
    kill -INT "$tcp_pid" 2>/dev/null || true
fi
if [ -n "$tcp_pid" ]; then
    wait "$tcp_pid" 2>/dev/null || true
fi

echo
echo "Receiver finished with exit code $receiver_status"
echo "Receiver log: $RECEIVER_LOG"
echo "Qwen log: $WATCHER_LOG"
echo "TCP result log: $TCP_LOG"
if [ "$receiver_status" -ne 0 ] && [ -s "$RECEIVER_LOG" ]; then
    echo "Receiver error:" >&2
    tail -n 20 "$RECEIVER_LOG" >&2
fi
exit "$receiver_status"
