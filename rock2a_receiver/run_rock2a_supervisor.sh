#!/usr/bin/env bash
set -u

PROJECT_DIR=/home/radxa/AI_CAMERA_LINUX/rock2a_receiver
RUNTIME_DIR=$PROJECT_DIR/runtime/ai_cam
LOG_DIR=$PROJECT_DIR/runtime/logs
RECEIVER=$PROJECT_DIR/build/rock2a_rtsp_receiver
PYTHON=$PROJECT_DIR/.venv-qwen/bin/python
ENV_FILE=/home/radxa/.config/ai_cam/qwen.env
MODE_FILE=/home/radxa/.config/ai_cam/mode
RV_HOST=rv1106-ai-camera
RV_IP=192.168.50.2
RV_PORT=554
RV_USER_EXIT=/tmp/ai_camera_user_exit

LOG_MAX=5242880
LOG_KEEP=3

MODE=$(cat "$MODE_FILE" 2>/dev/null || echo manual)
case "$MODE" in manual|auto) ;; *) MODE=manual ;; esac

AUTO_SAVE_POLICY=${AI_CAMERA_AUTO_SAVE_POLICY:-warning}
AUTO_SAVE_DEDUP_SECONDS=${AI_CAMERA_AUTO_SAVE_DEDUP_SECONDS:-60}
MIN_FREE_MB=${AI_CAMERA_MIN_FREE_MB:-1024}
AI_BACKEND=${AI_BACKEND:-cloud}
NPU_RESULT_PATH=${NPU_RESULT_PATH:-$RUNTIME_DIR/npu_latest.json}
NPU_DISPLAY_PATH=${NPU_DISPLAY_PATH:-$RUNTIME_DIR/npu_display.json}
NPU_SERVER_PORT=${NPU_SERVER_PORT:-9010}
NPU_PRESENCE_STALE_SECONDS=${NPU_PRESENCE_STALE_SECONDS:-3}

declare -A PIDS
declare -A FAILS
declare -A FIRST_TS
declare -A LAST_TS

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >>"$LOG_DIR/supervisor.log"
}

now_ts() {
    date +%s
}

mkdir -p "$LOG_DIR" "$RUNTIME_DIR"
rm -f "$NPU_RESULT_PATH" "$NPU_DISPLAY_PATH"

stop_child() {
    local name=$1
    local pid=${PIDS[$name]:-}
    [ -n "$pid" ] || return 0
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        local i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 10 ]; do
            sleep 1
            i=$((i + 1))
        done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    PIDS[$name]=""
}

stop_all() {
    stop_child watcher
    stop_child manual_server
    stop_child tcp_sender
    stop_child receiver
    stop_child npu_server
}

start_one() {
    local name=$1
    case "$name" in
        receiver)
            setsid "$RECEIVER" \
                --url "rtsp://${RV_IP}:${RV_PORT}/live/1" \
                --output "$PROJECT_DIR/artifacts/frames/ai_monitor" \
                --interval-ms 5000 \
                --latest-image "$RUNTIME_DIR/latest.jpg" \
                --latest-interval-ms 3000 \
                --duration 0 >>"$LOG_DIR/rtsp_receiver.log" 2>&1 &
            ;;
        manual_server)
            set -a; . "$ENV_FILE"; set +a
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/manual_recognize_server.py" \
                --host 0.0.0.0 --port 9001 \
                --result-path "$RUNTIME_DIR/latest_result.json" \
                --backend "$AI_BACKEND" --npu-result "$NPU_RESULT_PATH" \
                --min-free-mb "$MIN_FREE_MB" >>"$LOG_DIR/manual_server.log" 2>&1 &
            ;;
        npu_server)
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/npu_result_server.py" \
                --host 0.0.0.0 --port "$NPU_SERVER_PORT" \
                --result-path "$NPU_RESULT_PATH" \
                --display-path "$NPU_DISPLAY_PATH" \
                >>"$LOG_DIR/npu_server.log" 2>&1 &
            ;;
        watcher)
            set -a; . "$ENV_FILE"; set +a
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/watch_latest_image.py" \
                --image "$RUNTIME_DIR/latest.jpg" \
                --result "$RUNTIME_DIR/latest_result.json" \
                --interval-ms 5000 \
                --presence-result "$NPU_RESULT_PATH" \
                --presence-stale-seconds "$NPU_PRESENCE_STALE_SECONDS" \
                --auto-save-policy "$AUTO_SAVE_POLICY" \
                --auto-save-dedup-seconds "$AUTO_SAVE_DEDUP_SECONDS" \
                --min-free-mb "$MIN_FREE_MB" >>"$LOG_DIR/auto_watcher.log" 2>&1 &
            ;;
        tcp_sender)
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/send_result_tcp.py" \
                --input "$RUNTIME_DIR/latest_result.json" \
                --extra-input "$NPU_DISPLAY_PATH" \
                --host 0.0.0.0 \
                --port 9000 >>"$LOG_DIR/tcp_sender.log" 2>&1 &
            ;;
    esac
    PIDS[$name]=$!
    LAST_TS[$name]=$(now_ts)
}

start_pipeline() {
    log "starting pipeline components (mode=$MODE)"
    start_one receiver
    start_one npu_server
    if [ "$MODE" = "manual" ]; then
        start_one manual_server
    else
        start_one watcher
    fi
    start_one tcp_sender
}

child_alive() {
    local pid=${PIDS[$1]:-}
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        return 1
    fi
    local state
    state=$(ps -o stat= -p "$pid" 2>/dev/null || true)
    case "$state" in *Z*) return 1 ;; esac
    return 0
}

rv_reachable() {
    timeout 3 bash -c "exec 3<>/dev/tcp/${RV_IP}/${RV_PORT}" >/dev/null 2>&1
}

rv_user_exit() {
    ssh -o BatchMode=yes -o ConnectTimeout=3 "$RV_HOST" \
        "test -f '$RV_USER_EXIT'" >/dev/null 2>&1
}

restart_child() {
    local name=$1
    if [ "$name" = "receiver" ] && ! rv_reachable; then
        # RV is offline: retry slowly without counting failures so the
        # pipeline can recover automatically when the link comes back.
        log "RV1106 unreachable; retrying receiver in 10s"
        sleep 10
        start_one receiver
        return 0
    fi
    local now
    now=$(now_ts)
    FAILS[$name]=$(( ${FAILS[$name]:-0} + 1 ))
    if [ -z "${FIRST_TS[$name]:-}" ]; then
        FIRST_TS[$name]=$now
    fi
    local count=${FAILS[$name]}
    if [ "$count" -gt 5 ] || { [ "$count" -eq 5 ] && [ $((now - FIRST_TS[$name])) -le 60 ]; }; then
        log "component $name restart limit exceeded (5 in 60s); giving up"
        return 1
    fi
    case "$count" in
        1) delay=1 ;;
        2) delay=2 ;;
        3) delay=4 ;;
        4) delay=8 ;;
        *) delay=10 ;;
    esac
    log "restarting $name (attempt $count, delay ${delay}s)"
    sleep "$delay"
    start_one "$name"
    return 0
}

rotate_component_log() {
    local name=$1
    local file="$LOG_DIR/$name.log"
    [ -f "$file" ] || return 0
    local size
    size=$(wc -c <"$file" 2>/dev/null || echo 0)
    [ "$size" -lt "$LOG_MAX" ] && return 0
    log "rotating $file (${size} bytes)"
    stop_child "$name"
    local n=$LOG_KEEP
    while [ "$n" -gt 1 ]; do
        local prev=$((n - 1))
        [ -f "$file.$prev" ] && mv -f "$file.$prev" "$file.$n"
        n=$prev
    done
    [ -f "$file" ] && mv -f "$file" "$file.1"
    : >"$file"
    start_one "$name"
}

save_stats() {
    "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/save_stats.py" >>"$LOG_DIR/save_stats.log" 2>&1 || true
}

cleanup() {
    log "signal received; stopping all components"
    stop_all
    log "cleanup complete"
    exit 0
}

trap 'cleanup' INT TERM

log "=== ROCK 2A supervisor starting (mode=$MODE) ==="
log "auto-save-policy=$AUTO_SAVE_POLICY dedup=${AUTO_SAVE_DEDUP_SECONDS}s min-free=${MIN_FREE_MB}MB"

start_pipeline

ticks=0
while :; do
    sleep 1
    ticks=$((ticks + 1))
    now=$(now_ts)

    if [ $((ticks % 10)) -eq 0 ] && rv_user_exit; then
        log "RV1106 user exit detected; stopping pipeline"
        stop_all
        exit 0
    fi

    for name in receiver manual_server watcher tcp_sender npu_server; do
        if [ "$name" = "manual_server" ] && [ "$MODE" != "manual" ]; then
            continue
        fi
        if [ "$name" = "watcher" ] && [ "$MODE" != "auto" ]; then
            continue
        fi
        if child_alive "$name"; then
            if [ -n "${LAST_TS[$name]:-}" ] && [ $((now - LAST_TS[$name])) -gt 300 ]; then
                FAILS[$name]=0
                FIRST_TS[$name]=""
                log "backoff reset for $name (stable >5min)"
            fi
            continue
        fi
        log "component $name not alive; restarting"
        if ! restart_child "$name"; then
            log "component $name gave up; stopping supervisor"
            stop_all
            exit 1
        fi
    done

    if [ $((ticks % 600)) -eq 0 ]; then
        for name in receiver manual_server watcher tcp_sender npu_server; do
            rotate_component_log "$name"
        done
        save_stats
    fi
done
