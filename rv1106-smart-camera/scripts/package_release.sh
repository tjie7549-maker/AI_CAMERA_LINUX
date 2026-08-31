#!/bin/sh
# 将已构建文件打包为可追溯 release；不会访问开发板。
set -eu

usage() { echo "Usage: $0 [--build-dir DIR] [--output-dir DIR] [--config FILE] [--version VERSION]" >&2; exit 2; }
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-$project_dir/build}
output_dir=${RELEASE_DIR:-$project_dir/release}
version=
config_file=${CONFIG_FILE:-$project_dir/configs/config.json}
while [ "$#" -gt 0 ]; do
  case "$1" in
    --build-dir) shift; [ "$#" -gt 0 ] || usage; build_dir=$1 ;;
    --output-dir) shift; [ "$#" -gt 0 ] || usage; output_dir=$1 ;;
    --config) shift; [ "$#" -gt 0 ] || usage; config_file=$1 ;;
    --version) shift; [ "$#" -gt 0 ] || usage; version=$1 ;;
    *) usage ;;
  esac
  shift
done
if [ -z "$version" ]; then
  version=$(git -C "$project_dir/.." rev-parse --short HEAD 2>/dev/null || echo local)
fi
case "$version" in *[!A-Za-z0-9._-]*) echo "invalid version: $version" >&2; exit 2;; esac
stage="$output_dir/rv1106-smart-camera-$version"
if [ -e "$stage" ] || [ -e "$output_dir/rv1106-smart-camera-$version.tar.gz" ]; then
  echo "release already exists; choose a new --version or --output-dir" >&2
  exit 4
fi
mkdir -p "$stage/bin/fonts" "$stage/scripts/init"
for f in media-sender camera-daemon rtsp-preview-bridge rv1106_ai_ui; do
  test -f "$build_dir/bin/$f" || { echo "missing: $build_dir/bin/$f" >&2; exit 3; }
  install -m 755 "$build_dir/bin/$f" "$stage/bin/$f"
done
install -m 644 "$project_dir/rv1106-sender/qt-console/fonts/DroidSansFallbackFull.ttf" "$stage/bin/fonts/"
install -m 644 "$config_file" "$stage/config.json"
for f in run_demo.sh start.sh stress_test.sh mode_ctl.sh autostart_service.sh autostart_ctl.sh; do
  install -m 755 "$project_dir/scripts/$f" "$stage/scripts/$f"
done
install -m 755 "$project_dir/scripts/init/S99rv1106-smart-camera" "$stage/scripts/init/S99rv1106-smart-camera"
commit=$(git -C "$project_dir/.." rev-parse HEAD 2>/dev/null || echo unknown)
{
  echo "version=$version"
  echo "git_commit=$commit"
  echo "created_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  (cd "$stage" && find bin scripts -type f -print | sort | xargs sha256sum; sha256sum config.json)
} > "$stage/manifest.txt"
tar -C "$output_dir" -czf "$output_dir/rv1106-smart-camera-$version.tar.gz" "rv1106-smart-camera-$version"
echo "Release: $output_dir/rv1106-smart-camera-$version.tar.gz"
