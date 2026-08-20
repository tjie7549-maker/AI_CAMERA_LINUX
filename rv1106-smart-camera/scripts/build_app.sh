#!/bin/sh
# Cross-build only: no package download and no host x86 Qt.
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk_dir=${SDK_DIR:-/home/summary/linux/luckfox-pico}
qmake=${QMAKE:-$sdk_dir/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/bin/qmake}
build_dir=${BUILD_DIR:-$project_dir/build}
if [ -n "${SENDER_DIR:-}" ]; then
  sender_dir=$SENDER_DIR
elif [ -f "$project_dir/../rv1106_sender/Makefile" ]; then
  sender_dir=$project_dir/../rv1106_sender
else
  sender_dir=$project_dir/../ai_cam/rv1106_sender
fi

test -x "$qmake" || { echo "qmake not found: $qmake" >&2; exit 2; }
mkdir -p "$build_dir/bin" "$build_dir/qt-console"

test -f "$sender_dir/Makefile" || { echo "rv1106_sender not found: $sender_dir" >&2; exit 2; }
make -C "$sender_dir" SDK_DIR="$sdk_dir"
cp "$sender_dir/out/simple_vi_get_frame_send_vo_rv1106" "$build_dir/bin/media-sender"

make -C "$project_dir/app/camera-daemon" SDK_DIR="$sdk_dir"
cp "$project_dir/app/camera-daemon/camera-daemon" "$build_dir/bin/camera-daemon"

make -C "$project_dir/app/rtsp-preview-bridge" SDK_DIR="$sdk_dir"
cp "$project_dir/app/rtsp-preview-bridge/rtsp-preview-bridge" "$build_dir/bin/rtsp-preview-bridge"

(cd "$build_dir/qt-console" && "$qmake" "$project_dir/app/qt-console/rv1106_ai_ui.pro" && make)
cp "$build_dir/qt-console/rv1106_ai_ui" "$build_dir/bin/rv1106_ai_ui"

echo "Build completed: $build_dir/bin"
