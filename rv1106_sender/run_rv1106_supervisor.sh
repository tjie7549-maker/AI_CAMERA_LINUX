#!/bin/sh
set -u

APP_DIR=/root/userdata/ai_camera
LOG_DIR=$APP_DIR/logs
SENDER_SCRIPT=$APP_DIR/rv1106_sender/run_ai_headless_preview.sh
QT_SCRIPT=$APP_DIR/rv1106_ai_ui/run.sh
SOCK=/tmp/ai_cam_preview.sock
USER_EXIT=/tmp/ai_camera_user_exit
SHM_PREVIEW=/dev/shm/ai_cam_preview
FIFO_DIR=/tmp/ai_cam_fifos
LOG_MAX=5242880
LOG_KEEP=3
SOCK_TIMEOUT=90

# Memory watchdog: if MemAvailable stays below this for 3 consecutive checks
# (30s), restart the chain to release media/kernel buffers.
MEM_LOW_KB=40960

LOGGER_PIDS=""
sender_pid=""
qt_pid=""
RESTART_COUNT=0
CONSECUTIVE_FAILS=0
FIRST_RESTART_TS=0
LAST_RESTART_TS=0
LOW_MEM_CHECKS=0

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >>"$LOG_DIR/supervisor.log"
}

now_ts() {
    awk '{print int($1)}' /proc/uptime
}

meminfo_kb() {
    awk -v key="$1" '$1==key":"{print $2}' /proc/meminfo 2>/dev/null | head -1
}

rotate_file() {
    f=$1
    [ -f "$f" ] || return 0
    size=$(wc -c <"$f" 2>/dev/null || echo 0)
    [ "$size" -lt "$LOG_MAX" ] && return 0
    n=$LOG_KEEP
    while [ "$n" -gt 1 ]; do
        prev=$((n - 1))
        [ -f "$f.$prev" ] && mv -f "$f.$prev" "$f.$n"
        n=$prev
    done
    [ -f "$f" ] && mv -f "$f" "$f.1"
    : >"$f"
}

start_logger() {
    fifo=$1
    logfile=$2
    [ -p "$fifo" ] || mkfifo "$fifo" 2>/dev/null
    (while :; do cat "$fifo" >>"$logfile"; done) &
    LOGGER_PIDS="$LOGGER_PIDS $!"
}

start_loggers() {
    LOGGER_PIDS=""
    start_logger "$FIFO_DIR/ai_cam.fifo" "$LOG_DIR/ai_cam.log"
    start_logger "$FIFO_DIR/qt.fifo" "$LOG_DIR/qt.log"
}

stop_loggers() {
    for pid in $LOGGER_PIDS; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    LOGGER_PIDS=""
    sleep 1
}

rotate_logs() {
    old_pids=$LOGGER_PIDS
    for f in supervisor.log ai_cam.log qt.log; do
        rotate_file "$LOG_DIR/$f"
    done
    start_loggers
    for pid in $old_pids; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
}

start_sender() {
    setsid sh -c 'exec >"$1" 2>&1; shift; exec "$@"' sh \
        "$FIFO_DIR/ai_cam.fifo" "$SENDER_SCRIPT" --output /dev/null &
    sender_pid=$!
}

start_qt() {
    setsid sh -c 'exec >"$1" 2>&1; shift; exec "$@"' sh \
        "$FIFO_DIR/qt.fifo" "$QT_SCRIPT" &
    qt_pid=$!
}

stop_child() {
    pid=$1
    [ -n "$pid" ] || return 0
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 10 ]; do
            sleep 1
            i=$((i + 1))
        done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

stop_chain() {
    stop_child "$qt_pid"
    qt_pid=""
    stop_child "$sender_pid"
    sender_pid=""
    rm -f "$SOCK" "$SHM_PREVIEW"
}

wait_sock() {
    i=0
    while [ "$i" -lt "$SOCK_TIMEOUT" ]; do
        if [ -S "$SOCK" ]; then
            return 0
        fi
        if [ -n "$sender_pid" ] && ! kill -0 "$sender_pid" 2>/dev/null; then
            log "sender exited during startup after ${i}s"
            return 1
        fi
        i=$((i + 1))
        sleep 1
    done
    log "preview socket not ready after ${SOCK_TIMEOUT}s (sender still alive)"
    return 1
}

wait_eth0() {
    i=0
    while [ "$i" -lt 60 ]; do
        if ip -4 addr show dev eth0 2>/dev/null | grep -q '192.168.50.2'; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

wait_devices() {
    i=0
    while [ "$i" -lt 60 ]; do
        if [ -e /dev/video11 ] && [ -e /dev/dri/card0 ] && [ -e /dev/input/event0 ]; then
            return 0
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

start_chain() {
    log "starting sender"
    start_sender
    if ! wait_sock; then
        return 1
    fi
    log "starting Qt"
    start_qt
    return 0
}

restart_chain() {
    now=$(now_ts)
    CONSECUTIVE_FAILS=$((CONSECUTIVE_FAILS + 1))
    if [ "$CONSECUTIVE_FAILS" -ge 5 ]; then
        log "5 consecutive failed starts; giving up"
        return 1
    fi
    if [ "$RESTART_COUNT" -eq 0 ]; then
        FIRST_RESTART_TS=$now
    fi
    RESTART_COUNT=$((RESTART_COUNT + 1))
    if [ "$RESTART_COUNT" -gt 5 ] || { [ "$RESTART_COUNT" -eq 5 ] && [ $((now - FIRST_RESTART_TS)) -le 60 ]; }; then
        log "restart limit exceeded (5 in 60s); giving up"
        return 1
    fi
    case "$RESTART_COUNT" in
        1) delay=1 ;;
        2) delay=2 ;;
        3) delay=4 ;;
        4) delay=8 ;;
        *) delay=10 ;;
    esac
    log "restart attempt $RESTART_COUNT, delay ${delay}s"
    sleep "$delay"
    if start_chain; then
        CONSECUTIVE_FAILS=0
        LAST_RESTART_TS=$(now_ts)
    fi
    return 0
}

cleanup() {
    log "signal received; cleaning up"
    stop_chain
    rm -f "$USER_EXIT"
    stop_loggers
    rm -rf "$FIFO_DIR"
    log "cleanup complete"
    exit 0
}

trap 'cleanup' INT TERM

mkdir -p "$LOG_DIR" "$FIFO_DIR"
rm -f "$USER_EXIT"

log "=== RV1106 supervisor starting ==="

if ! wait_devices; then
    log "device nodes missing after 60s; exiting"
    exit 1
fi
log "device nodes ready"

if ! wait_eth0; then
    log "eth0 not 192.168.50.2 after 60s; exiting"
    exit 1
fi
log "eth0 ready: 192.168.50.2"

start_loggers

if ! start_chain; then
    log "initial start failed"
    stop_loggers
    exit 1
fi
CONSECUTIVE_FAILS=0
LAST_RESTART_TS=$(now_ts)

ticks=0
while :; do
    now=$(now_ts)
    if [ "$RESTART_COUNT" -gt 0 ] && [ $((now - LAST_RESTART_TS)) -gt 300 ]; then
        RESTART_COUNT=0
        CONSECUTIVE_FAILS=0
        FIRST_RESTART_TS=0
        log "restart counter reset (stable >5min)"
    fi

    sender_alive=0
    qt_alive=0
    [ -n "$sender_pid" ] && kill -0 "$sender_pid" 2>/dev/null && sender_alive=1
    [ -n "$qt_pid" ] && kill -0 "$qt_pid" 2>/dev/null && qt_alive=1

    if [ "$sender_alive" -eq 1 ] && [ "$qt_alive" -eq 1 ]; then
        ticks=$((ticks + 1))
        if [ $((ticks % 30)) -eq 0 ]; then
            rotate_logs
        fi
        if [ $((ticks % 10)) -eq 0 ]; then
            mem_avail=$(meminfo_kb MemAvailable)
            if [ -n "$mem_avail" ] && [ "$mem_avail" -lt "$MEM_LOW_KB" ]; then
                LOW_MEM_CHECKS=$((LOW_MEM_CHECKS + 1))
            else
                LOW_MEM_CHECKS=0
            fi
            if [ "$LOW_MEM_CHECKS" -ge 3 ]; then
                LOW_MEM_CHECKS=0
                log "memory pressure (MemAvailable=${mem_avail}kB < ${MEM_LOW_KB}kB); restarting chain"
                stop_chain
                if ! restart_chain; then
                    log "restart limit reached after memory watchdog; giving up"
                    stop_loggers
                    rm -rf "$FIFO_DIR"
                    exit 1
                fi
            fi
        fi
        sleep 1
        continue
    fi

    if [ -n "$qt_pid" ] && ! kill -0 "$qt_pid" 2>/dev/null; then
        wait "$qt_pid"
        qt_code=$?
        qt_pid=""
        log "Qt exited with code $qt_code"
        if [ "$qt_code" -eq 42 ]; then
            log "user exit requested (code 42)"
            stop_child "$sender_pid"
            sender_pid=""
            rm -f "$SOCK" "$SHM_PREVIEW"
            : >"$USER_EXIT"
            stop_loggers
            rm -rf "$FIFO_DIR"
            log "supervisor exited normally (user exit)"
            exit 0
        fi
    fi

    log "chain abnormal: sender_alive=$sender_alive qt_alive=$qt_alive; restarting"
    stop_chain
    if ! restart_chain; then
        log "restart limit reached; supervisor giving up"
        stop_loggers
        rm -rf "$FIFO_DIR"
        exit 1
    fi
done
