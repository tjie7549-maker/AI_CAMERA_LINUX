#!/bin/sh

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$APP_DIR/simple_vi_get_frame_send_vo_rv1106"

if [ "$#" -gt 2 ]; then
	echo "Usage: $0 [0|90|180|270] [--restore]" >&2
	exit 1
fi

rotation=${1:-180}
restore_default=0

if [ "${2:-}" = "--restore" ]; then
	restore_default=1
elif [ "$#" -eq 2 ]; then
	echo "Usage: $0 [0|90|180|270] [--restore]" >&2
	exit 1
fi

case "$rotation" in
	0|90|180|270) ;;
	*)
		echo "Usage: $0 [0|90|180|270] [--restore]" >&2
		exit 1
		;;
esac

restore_rkipc() {
	if pidof rkipc >/dev/null 2>&1; then
		return
	fi

	# 等待 ISP 和显示资源完成异步释放，避免默认媒体服务抢占资源。
	sleep 2
	setsid sh -c 'LD_LIBRARY_PATH=/oem/usr/lib exec /oem/usr/bin/rkipc >/tmp/rkipc.log 2>&1' \
		</dev/null >/dev/null 2>&1 &
	count=0
	while ! pidof rkipc >/dev/null 2>&1 && [ "$count" -lt 12 ]; do
		count=$((count + 1))
		sleep 1
	done
	if ! pidof rkipc >/dev/null 2>&1; then
		echo "Failed to restore rkipc; check /tmp/rkipc.log" >&2
	fi
}

stop_rkipc() {
	killall rkipc >/dev/null 2>&1

	count=0
	while pidof rkipc >/dev/null 2>&1 && [ "$count" -lt 5 ]; do
		count=$((count + 1))
		sleep 1
	done
	if pidof rkipc >/dev/null 2>&1; then
		echo "Failed to stop rkipc" >&2
		return 1
	fi
	sleep 2
}

cleanup() {
	if [ "$restore_default" -eq 1 ]; then
		restore_rkipc
	fi
}

on_signal() {
	if [ -n "$app_pid" ]; then
		kill -INT "$app_pid" 2>/dev/null
		wait "$app_pid"
	fi
	cleanup
	exit 130
}

if [ ! -x "$APP" ]; then
	echo "Missing executable: $APP" >&2
	exit 1
fi

trap on_signal INT TERM

stop_rkipc || exit 1

export LD_LIBRARY_PATH=/oem/usr/lib
"$APP" -a /oem/usr/share/iqfiles \
	-w 2304 -h 1296 -W 720 -H 720 -r "$rotation" -I 0 -l 0 -d 0 &
app_pid=$!
wait "$app_pid"
status=$?

cleanup
exit "$status"
