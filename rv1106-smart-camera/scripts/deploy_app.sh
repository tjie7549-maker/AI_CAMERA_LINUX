#!/bin/sh
# Deploy only to /userdata; this script never removes remote files.
set -eu

usage() { echo "Usage: $0 --ip BOARD_IP [--user root] [--dry-run]" >&2; exit 2; }
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-$project_dir/build}
board_ip= user=root dry_run=false
while [ "$#" -gt 0 ]; do
  case "$1" in
    --ip) shift; [ "$#" -gt 0 ] || usage; board_ip=$1 ;;
    --user) shift; [ "$#" -gt 0 ] || usage; user=$1 ;;
    --dry-run) dry_run=true ;;
    *) usage ;;
  esac
  shift
done
[ -n "$board_ip" ] || usage
font_file=$project_dir/app/qt-console/fonts/DroidSansFallbackFull.ttf
for f in "$build_dir/bin/media-sender" "$build_dir/bin/camera-daemon" "$build_dir/bin/rv1106_ai_ui" "$font_file" "$project_dir/configs/config.json" "$project_dir/scripts/run_demo.sh" "$project_dir/scripts/start.sh" "$project_dir/scripts/stress_test.sh"; do
  [ -f "$f" ] || { echo "Missing build/deploy input: $f; run build_app.sh first" >&2; exit 3; }
done
echo "Target: $user@$board_ip:/userdata/rv1106-smart-camera"
echo "Files (no remote deletion):"
printf '%s\n' "$build_dir/bin/media-sender" "$build_dir/bin/camera-daemon" "$build_dir/bin/rv1106_ai_ui" "$font_file" "$project_dir/configs/config.json" "$project_dir/scripts/run_demo.sh" "$project_dir/scripts/start.sh" "$project_dir/scripts/stress_test.sh"
[ "$dry_run" = true ] && exit 0
ssh "$user@$board_ip" 'mkdir -p /userdata/rv1106-smart-camera/bin/fonts /userdata/rv1106-smart-camera/logs /userdata/rv1106-smart-camera/run /userdata/rv1106-smart-camera/captures /userdata/rv1106-smart-camera/scripts'
scp "$build_dir/bin/media-sender" "$build_dir/bin/camera-daemon" "$build_dir/bin/rv1106_ai_ui" "$user@$board_ip:/userdata/rv1106-smart-camera/bin/"
scp "$font_file" "$user@$board_ip:/userdata/rv1106-smart-camera/bin/fonts/"
scp "$project_dir/configs/config.json" "$user@$board_ip:/userdata/rv1106-smart-camera/config.json"
scp "$project_dir/scripts/run_demo.sh" "$project_dir/scripts/start.sh" "$project_dir/scripts/stress_test.sh" "$user@$board_ip:/userdata/rv1106-smart-camera/scripts/"
ssh "$user@$board_ip" 'chmod 755 /userdata/rv1106-smart-camera/bin/media-sender /userdata/rv1106-smart-camera/bin/camera-daemon /userdata/rv1106-smart-camera/bin/rv1106_ai_ui /userdata/rv1106-smart-camera/scripts/run_demo.sh /userdata/rv1106-smart-camera/scripts/start.sh /userdata/rv1106-smart-camera/scripts/stress_test.sh'
echo "Deploy completed. No system directory and no existing AI-project file was modified."
