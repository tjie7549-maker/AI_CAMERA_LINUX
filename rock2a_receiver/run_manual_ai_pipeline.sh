#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RECEIVER="$PROJECT_DIR/build/rock2a_rtsp_receiver"
PYTHON="$PROJECT_DIR/.venv-qwen/bin/python"
ENV_FILE="$HOME/.config/ai_cam/qwen.env"
RUNTIME_DIR="$PROJECT_DIR/runtime/ai_cam"
RESULT_PATH="$RUNTIME_DIR/latest_result.json"
FRAME_DIR="$PROJECT_DIR/artifacts/frames/manual_monitor"
URL="${RTSP_URL:-rtsp://192.168.50.2:554/live/1}"
SERVER_PORT="${MANUAL_RECOGNIZE_PORT:-9001}"

for required in "$RECEIVER" "$PYTHON" "$ENV_FILE"; do
    if [ ! -e "$required" ]; then
        echo "Missing required file: $required" >&2
        exit 1
    fi
done

mkdir -p "$RUNTIME_DIR" "$FRAME_DIR"
rm -f "$RESULT_PATH"

set -a
. "$ENV_FILE"
set +a

receiver_pid=""
server_pid=""
tcp_pid=""
stopping=0

stop_child() {
    local pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_child "$receiver_pid"
    stop_child "$server_pid"
    stop_child "$tcp_pid"
    for pid in "$receiver_pid" "$server_pid" "$tcp_pid"; do
        if [ -n "$pid" ]; then wait "$pid" 2>/dev/null || true; fi
    done
}

on_signal() {
    if [ "$stopping" -ne 0 ]; then return; fi
    stopping=1
    trap '' INT TERM
    echo
    echo "Stopping manual recognition, TCP result server, and receiver..."
    cleanup
    exit 130
}
trap on_signal INT TERM

setsid "$RECEIVER" --url "$URL" --output "$FRAME_DIR" --interval-ms 5000 \
    --duration 0 >"$RUNTIME_DIR/manual_receiver.log" 2>&1 &
receiver_pid=$!
setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/manual_recognize_server.py" \
    --host 0.0.0.0 --port "$SERVER_PORT" --result-path "$RESULT_PATH" \
    >"$RUNTIME_DIR/manual_server.log" 2>&1 &
server_pid=$!
setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/send_result_tcp.py" \
    --input "$RESULT_PATH" --host "${RESULT_HOST:-0.0.0.0}" \
    --port "${RESULT_PORT:-9000}" >"$RUNTIME_DIR/result_tcp.log" 2>&1 &
tcp_pid=$!

for _ in $(seq 1 15); do
    if "$PYTHON" -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:${SERVER_PORT}/health', timeout=1).read()" >/dev/null 2>&1; then
        echo "Manual recognition ready: http://0.0.0.0:${SERVER_PORT}/recognize"
        echo "Press Ctrl+C to stop."
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "Manual recognition server failed. See $RUNTIME_DIR/manual_server.log" >&2
        cleanup
        exit 1
    fi
    sleep 1
done

if ! "$PYTHON" -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:${SERVER_PORT}/health', timeout=1).read()" >/dev/null 2>&1; then
    echo "Manual recognition health check timed out." >&2
    cleanup
    exit 1
fi

while kill -0 "$receiver_pid" 2>/dev/null && kill -0 "$server_pid" 2>/dev/null && kill -0 "$tcp_pid" 2>/dev/null; do
    sleep 1
done
cleanup
exit 1
