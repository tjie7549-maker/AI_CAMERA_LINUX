#!/usr/bin/env bash
set -u

# ROCK 2A 部署目录与运行期目录；运行期 JSON、SQLite 和日志不写回源码树。
PROJECT_DIR=/home/radxa/AI_CAMERA_LINUX/rock2a_receiver
RUNTIME_DIR=$PROJECT_DIR/runtime/ai_cam
LOG_DIR=$PROJECT_DIR/runtime/logs
RECEIVER=$PROJECT_DIR/build/rock2a_rtsp_receiver
PYTHON=$PROJECT_DIR/.venv-qwen/bin/python
ENV_FILE=/home/radxa/.config/ai_cam/qwen.env
MODE_FILE=/home/radxa/.config/ai_cam/mode
CONFIG_FILE=${AI_CAMERA_CONFIG_FILE:-/etc/ai-camera.env}

if [ -r "$CONFIG_FILE" ]; then
    set -a
    . "$CONFIG_FILE"
    set +a
fi

# RV1106 私有链路地址：RTSP 由 receiver 拉取，SSH 仅用于感知用户退出。
RV_HOST=${AI_CAMERA_RV_HOST:-rv1106-ai-camera}
RV_IP=${AI_CAMERA_RV_IP:-192.168.50.2}
RV_PORT=${AI_CAMERA_RTSP_PORT:-554}
RV_USER_EXIT=/tmp/ai_camera_user_exit

LOG_MAX=5242880
LOG_KEEP=3

MODE=${AI_CAMERA_MODE:-$(cat "$MODE_FILE" 2>/dev/null || echo event)}
case "$MODE" in event|manual|auto) ;; *) MODE=event ;; esac

AUTO_SAVE_POLICY=${AI_CAMERA_AUTO_SAVE_POLICY:-warning}
AUTO_SAVE_DEDUP_SECONDS=${AI_CAMERA_AUTO_SAVE_DEDUP_SECONDS:-60}
MIN_FREE_MB=${AI_CAMERA_MIN_FREE_MB:-1024}
AI_BACKEND=${AI_BACKEND:-cloud}
# NPU 原始状态、Qt 兼容显示状态及各服务监听端口。
NPU_RESULT_PATH=${NPU_RESULT_PATH:-$RUNTIME_DIR/npu_latest.json}
NPU_DISPLAY_PATH=${NPU_DISPLAY_PATH:-$RUNTIME_DIR/npu_display.json}
NPU_SERVER_PORT=${NPU_SERVER_PORT:-9010}
NPU_PRESENCE_STALE_SECONDS=${NPU_PRESENCE_STALE_SECONDS:-3}
BIND_ADDRESS=${AI_CAMERA_BIND_ADDRESS:-192.168.50.1}
EVENT_PORT=${AI_CAMERA_EVENT_PORT:-9011}
MANUAL_PORT=${AI_CAMERA_MANUAL_PORT:-9001}
RESULT_PORT=${AI_CAMERA_RESULT_PORT:-9000}
# 事件云识别策略：首次延迟、冷却、长事件刷新、结束宽限与最大重试次数。
EVENT_CLOUD_ENABLED=${EVENT_CLOUD_ENABLED:-1}
EVENT_INITIAL_DELAY_SECONDS=${EVENT_INITIAL_DELAY_SECONDS:-3}
EVENT_CLOUD_COOLDOWN_SECONDS=${EVENT_CLOUD_COOLDOWN_SECONDS:-120}
EVENT_LONG_REFRESH_SECONDS=${EVENT_LONG_REFRESH_SECONDS:-300}
EVENT_SCENE_CHANGE_THRESHOLD=${EVENT_SCENE_CHANGE_THRESHOLD:-0.15}
EVENT_END_GRACE_SECONDS=${EVENT_END_GRACE_SECONDS:-10}
EVENT_DETECTION_STALE_SECONDS=${EVENT_DETECTION_STALE_SECONDS:-5}
EVENT_CLOUD_MAX_RETRIES=${EVENT_CLOUD_MAX_RETRIES:-2}
FRAME_RING_DIR=${FRAME_RING_DIR:-$RUNTIME_DIR/frame_ring}

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
rm -f "$NPU_RESULT_PATH" "$NPU_DISPLAY_PATH" \
    "$RUNTIME_DIR/event_latest.json" "$RUNTIME_DIR/event_recognition_latest.json"

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
    stop_child npu_server
    stop_child manual_server
    stop_child tcp_sender
    stop_child receiver
    stop_child event_service
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
                --frame-cache-dir "$FRAME_RING_DIR" \
                --frame-cache-interval-ms 500 --frame-cache-max 24 \
                --duration 0 >>"$LOG_DIR/rtsp_receiver.log" 2>&1 &
            ;;
        manual_server)
            set -a; . "$ENV_FILE"; set +a
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/manual_recognize_server.py" \
                --host "$BIND_ADDRESS" --port "$MANUAL_PORT" \
                --result-path "$RUNTIME_DIR/latest_result.json" \
                --backend "$AI_BACKEND" --npu-result "$NPU_RESULT_PATH" \
                --min-free-mb "$MIN_FREE_MB" >>"$LOG_DIR/manual_server.log" 2>&1 &
            ;;
        npu_server)
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/npu_result_server.py" \
                --host "$BIND_ADDRESS" --port "$NPU_SERVER_PORT" \
                --result-path "$NPU_RESULT_PATH" \
                --display-path "$NPU_DISPLAY_PATH" \
                --event-url "http://${BIND_ADDRESS}:${EVENT_PORT}/ingest" \
                >>"$LOG_DIR/npu_server.log" 2>&1 &
            ;;
        event_service)
            set -a; . "$ENV_FILE"; set +a
            cloud_flag=--cloud-enabled
            if [ "$EVENT_CLOUD_ENABLED" != 1 ] || [ "$MODE" = auto ]; then
                cloud_flag=--no-cloud-enabled
            fi
            setsid "$PYTHON" "$PROJECT_DIR/tools/qwen_vision/event_service.py" \
                --host "$BIND_ADDRESS" --port "$EVENT_PORT" \
                --runtime "$RUNTIME_DIR" --latest-image "$RUNTIME_DIR/latest.jpg" \
                --frame-ring-dir "$FRAME_RING_DIR" "$cloud_flag" \
                --initial-delay-seconds "$EVENT_INITIAL_DELAY_SECONDS" \
                --cooldown-seconds "$EVENT_CLOUD_COOLDOWN_SECONDS" \
                --long-refresh-seconds "$EVENT_LONG_REFRESH_SECONDS" \
                --scene-change-threshold "$EVENT_SCENE_CHANGE_THRESHOLD" \
                --end-grace-seconds "$EVENT_END_GRACE_SECONDS" \
                --detection-stale-seconds "$EVENT_DETECTION_STALE_SECONDS" \
                --cloud-max-retries "$EVENT_CLOUD_MAX_RETRIES" \
                >>"$LOG_DIR/event_service.log" 2>&1 &
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
                --extra-input "$RUNTIME_DIR/event_latest.json" \
                --extra-input "$RUNTIME_DIR/event_recognition_latest.json" \
                --extra-input "$NPU_DISPLAY_PATH" \
                --host "$BIND_ADDRESS" \
                --port "$RESULT_PORT" >>"$LOG_DIR/tcp_sender.log" 2>&1 &
            ;;
    esac
    PIDS[$name]=$!
    LAST_TS[$name]=$(now_ts)
}

start_pipeline() {
    log "starting pipeline components (mode=$MODE)"
    start_one receiver
    start_one event_service
    start_one npu_server
    if [ "$MODE" = "event" ] || [ "$MODE" = "manual" ]; then
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

    for name in receiver event_service manual_server watcher tcp_sender npu_server; do
        if [ "$name" = "manual_server" ] && [ "$MODE" != "manual" ] && [ "$MODE" != "event" ]; then
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
        for name in receiver event_service manual_server watcher tcp_sender npu_server; do
            rotate_component_log "$name"
        done
        save_stats
    fi
done
