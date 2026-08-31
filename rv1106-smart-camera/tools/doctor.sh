#!/bin/sh
# 只检查本机环境，不下载、不安装、不修改 SDK 或开发板。
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
toolchain=$sdk_dir/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
failed=0

check_file() {
  if [ -x "$2" ] || [ -f "$2" ]; then echo "OK   $1: $2"; else echo "MISS $1: $2" >&2; failed=1; fi
}
check_cmd() {
  if command -v "$1" >/dev/null 2>&1; then echo "OK   command: $1"; else echo "MISS command: $1" >&2; failed=1; fi
}

echo "Project: $project_dir"
check_cmd make
check_cmd tar
check_cmd sha256sum
check_cmd python3
check_file "SDK" "$sdk_dir/media/Makefile.param"
check_file "cross-gcc" "$toolchain"
check_file "qmake" "$qmake"
check_file "runtime config" "$project_dir/configs/config.json"
check_file "media sender" "$project_dir/rv1106-sender/media-sender/Makefile"

if [ "$failed" -ne 0 ]; then
  echo "Environment is incomplete. Copy configs/build.env.example and pass --env." >&2
  exit 1
fi
echo "Environment check passed."
