#!/bin/sh
# Cross-build only: no package download and no host x86 Qt.
set -eu

usage() { echo "Usage: $0 [--env BUILD_ENV]" >&2; exit 2; }
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
while [ "$#" -gt 0 ]; do
  case "$1" in
    --env) shift; [ "$#" -gt 0 ] || usage; . "$1" ;;
    *) usage ;;
  esac
  shift
done
sdk_dir=${SDK_DIR:-/home/summary/linux/luckfox-pico}
qmake=${QMAKE:-$sdk_dir/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake}
build_dir=${BUILD_DIR:-$project_dir/build}
sender_dir=${SENDER_DIR:-$project_dir/rv1106-sender/media-sender}

test -x "$qmake" || { echo "qmake not found: $qmake" >&2; exit 2; }
mkdir -p "$build_dir/bin" "$build_dir/qt-console"

test -f "$sender_dir/Makefile" || { echo "media-sender not found: $sender_dir" >&2; exit 2; }
make -C "$sender_dir" SDK_DIR="$sdk_dir"
cp "$sender_dir/out/simple_vi_get_frame_send_vo_rv1106" "$build_dir/bin/media-sender"

make -C "$project_dir/rv1106-sender/camera-daemon" SDK_DIR="$sdk_dir"
cp "$project_dir/rv1106-sender/camera-daemon/camera-daemon" "$build_dir/bin/camera-daemon"

make -C "$project_dir/rv1106-sender/rtsp-preview-bridge" SDK_DIR="$sdk_dir"
cp "$project_dir/rv1106-sender/rtsp-preview-bridge/rtsp-preview-bridge" "$build_dir/bin/rtsp-preview-bridge"

(cd "$build_dir/qt-console" && "$qmake" "$project_dir/rv1106-sender/qt-console/rv1106_ai_ui.pro" && make)
cp "$build_dir/qt-console/rv1106_ai_ui" "$build_dir/bin/rv1106_ai_ui"

echo "Build completed: $build_dir/bin"
echo "SDK: $sdk_dir"
