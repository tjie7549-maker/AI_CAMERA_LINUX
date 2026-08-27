#!/bin/sh

set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SENDER="$APP_DIR/rv1106_sender/run_ai_headless_preview.sh"
UI="$APP_DIR/rv1106_ai_ui/run.sh"
LOG_DIR="$APP_DIR/logs"
SENDER_LOG="$LOG_DIR/sender.log"
UI_LOG="$LOG_DIR/ui.log"
USER_EXIT_FILE=/tmp/ai_camera_user_exit

sender_pid=""
ui_pid=""

stop_child() {
    pid=$1
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_child "$ui_pid"
    if [ -n "$ui_pid" ]; then
        wait "$ui_pid" 2>/dev/null || true
    fi
    stop_child "$sender_pid"
    if [ -n "$sender_pid" ]; then
        wait "$sender_pid" 2>/dev/null || true
    fi
}

on_signal() {
    echo
    echo "Stopping Qt and camera pipeline..."
    cleanup
    exit 130
}

if [ ! -x "$SENDER" ] || [ ! -x "$UI" ]; then
    echo "Incomplete deployment under $APP_DIR" >&2
    exit 1
fi

mkdir -p "$LOG_DIR"
rm -f "$USER_EXIT_FILE"
trap on_signal INT TERM

# Keep children out of the terminal foreground process group.  Ctrl+C then
# reaches this supervisor only, avoiding duplicate media shutdown signals.
setsid "$SENDER" --output /dev/null >"$SENDER_LOG" 2>&1 &
sender_pid=$!

attempt=0
while [ ! -S /tmp/ai_cam_preview.sock ] && [ "$attempt" -lt 20 ]; do
    if ! kill -0 "$sender_pid" 2>/dev/null; then
        echo "Camera pipeline failed. See: $SENDER_LOG" >&2
        cleanup
        exit 1
    fi
    sleep 1
    attempt=$((attempt + 1))
done

if [ ! -S /tmp/ai_cam_preview.sock ]; then
    echo "Preview socket did not become ready. See: $SENDER_LOG" >&2
    cleanup
    exit 1
fi

setsid "$UI" >"$UI_LOG" 2>&1 &
ui_pid=$!

echo "AI camera terminal started. Press Ctrl+C to stop."
echo "LCD/Qt log: $UI_LOG"
echo "Camera/RTSP log: $SENDER_LOG"

while kill -0 "$ui_pid" 2>/dev/null && kill -0 "$sender_pid" 2>/dev/null; do
    sleep 1
done

if ! kill -0 "$sender_pid" 2>/dev/null; then
    echo "Camera pipeline stopped unexpectedly. See: $SENDER_LOG" >&2
    stop_child "$ui_pid"
fi

if wait "$ui_pid"; then
    ui_status=0
else
    ui_status=$?
fi
ui_pid=""
cleanup
if [ "$ui_status" -eq 42 ]; then
    # The linked ROCK 2A supervisor consumes this marker and stops its services.
    : >"$USER_EXIT_FILE"
    echo "Qt user exit requested; camera pipeline stopped."
fi
exit "$ui_status"
