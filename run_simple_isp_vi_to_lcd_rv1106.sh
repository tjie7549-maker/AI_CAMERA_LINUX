#!/bin/sh

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$APP_DIR/simple_vi_get_frame_send_vo_rv1106"

if [ "$#" -gt 1 ]; then
	echo "Usage: $0 [0|90|180|270]" >&2
	exit 1
fi

rotation=${1:-180}

case "$rotation" in
	0|90|180|270) ;;
	*)
		echo "Usage: $0 [0|90|180|270]" >&2
		exit 1
		;;
esac

restore_rkipc() {
	if pidof rkipc >/dev/null 2>&1; then
		return
	fi

	# The ISP and DRM resources are released asynchronously after the demo exits.
	sleep 2
	setsid sh -c 'LD_LIBRARY_PATH=/oem/usr/lib exec /oem/usr/bin/rkipc >/tmp/rkipc.log 2>&1' \
		</dev/null >/dev/null 2>&1 &
	sleep 1
	if ! pidof rkipc >/dev/null 2>&1; then
		echo "Failed to restore rkipc; check /tmp/rkipc.log" >&2
	fi
}

cleanup() {
	restore_rkipc
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

killall rkipc >/dev/null 2>&1
sleep 2

export LD_LIBRARY_PATH=/oem/usr/lib
"$APP" -a /oem/usr/share/iqfiles \
	-w 2304 -h 1296 -W 720 -H 720 -r "$rotation" -I 0 -l 0 -d 0 &
app_pid=$!
wait "$app_pid"
status=$?

cleanup
exit "$status"
