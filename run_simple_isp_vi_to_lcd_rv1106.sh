#!/bin/sh

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$APP_DIR/simple_vi_get_frame_send_vo_rv1106"

sensor_width=2304
sensor_height=1296
lcd_width=720
lcd_height=720
rotation=180
iq_dir=/oem/usr/share/iqfiles
isp_index=0
vo_layer=0
vo_device=0
output=/dev/null
restore_default=0

usage() {
	cat <<EOF
Usage: $0 [0|90|180|270] [options]

Default pipeline: 2304x1296 sensor -> 720x720 LCD, rotate 180, no H.264 file.

Options:
  --rotation DEGREE       LCD rotation: 0, 90, 180, or 270
  --sensor-width WIDTH    Sensor input width (default: $sensor_width)
  --sensor-height HEIGHT  Sensor input height (default: $sensor_height)
  --lcd-width WIDTH       LCD output width (default: $lcd_width)
  --lcd-height HEIGHT     LCD output height (default: $lcd_height)
  --iq-dir PATH           ISP IQ directory (default: $iq_dir)
  --isp INDEX             ISP index (default: $isp_index)
  --vo-layer INDEX        VO layer (default: $vo_layer)
  --vo-device INDEX       VO device (default: $vo_device)
  --output PATH           Main H.264 output; /dev/null disables file saving
  --restore               Restart rkipc after this program exits
  --help                  Show this help
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
		0|90|180|270)
			rotation=$1
			;;
		--rotation)
			require_value "$@"
			rotation=$2
			shift
			;;
		--sensor-width)
			require_value "$@"
			sensor_width=$2
			shift
			;;
		--sensor-height)
			require_value "$@"
			sensor_height=$2
			shift
			;;
		--lcd-width)
			require_value "$@"
			lcd_width=$2
			shift
			;;
		--lcd-height)
			require_value "$@"
			lcd_height=$2
			shift
			;;
		--iq-dir)
			require_value "$@"
			iq_dir=$2
			shift
			;;
		--isp)
			require_value "$@"
			isp_index=$2
			shift
			;;
		--vo-layer)
			require_value "$@"
			vo_layer=$2
			shift
			;;
		--vo-device)
			require_value "$@"
			vo_device=$2
			shift
			;;
		--output)
			require_value "$@"
			output=$2
			shift
			;;
		--restore)
			restore_default=1
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

case "$rotation" in
	0|90|180|270) ;;
	*)
		echo "Invalid rotation: $rotation" >&2
		usage >&2
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
"$APP" -a "$iq_dir" \
	-w "$sensor_width" -h "$sensor_height" -W "$lcd_width" -H "$lcd_height" \
	-r "$rotation" -I "$isp_index" -l "$vo_layer" -d "$vo_device" \
	-o "$output" &
app_pid=$!
wait "$app_pid"
status=$?

cleanup
exit "$status"
