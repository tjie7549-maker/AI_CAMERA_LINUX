#!/bin/sh

set -u

DEFAULT_ROOT=/userdata/rv1106-smart-camera/baseline
OUTPUT_ROOT=${1:-$DEFAULT_ROOT}
TIMESTAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%d-%H%M%S)
OUTPUT_DIR=$OUTPUT_ROOT/$TIMESTAMP

if ! mkdir -p "$OUTPUT_DIR"; then
    echo "ERROR: cannot create baseline output: $OUTPUT_DIR" >&2
    exit 1
fi

record_command() {
    output=$1
    shift
    (
        echo "command: $*"
        echo "started_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)"
        "$@"
        status=$?
        echo "exit_status: $status"
        exit "$status"
    ) >"$OUTPUT_DIR/$output" 2>&1
    return $?
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

missing=""
for tool in media-ctl v4l2-ctl; do
    if ! command_exists "$tool"; then
        missing="$missing $tool"
    fi
done

{
    echo "baseline_directory=$OUTPUT_DIR"
    echo "generated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)"
    echo "missing_tools=${missing# }"
    echo "note=Collection is read-only; individual command failures are retained in files."
} >"$OUTPUT_DIR/manifest.txt"

record_command uname.txt uname -a || true

if [ -r /proc/device-tree/model ]; then
    tr '\000' '\n' </proc/device-tree/model >"$OUTPUT_DIR/device-tree-model.txt"
else
    echo "unavailable: /proc/device-tree/model" >"$OUTPUT_DIR/device-tree-model.txt"
fi
if [ -r /proc/device-tree/compatible ]; then
    tr '\000' '\n' </proc/device-tree/compatible >"$OUTPUT_DIR/device-tree-compatible.txt"
else
    echo "unavailable: /proc/device-tree/compatible" >"$OUTPUT_DIR/device-tree-compatible.txt"
fi

if command_exists dmesg; then
    record_command dmesg-all.txt dmesg || true
    if [ -r "$OUTPUT_DIR/dmesg-all.txt" ]; then
        grep -Ei 'sc3336|mipi|csi|dphy|rkcif|cif|rkisp|isp' \
            "$OUTPUT_DIR/dmesg-all.txt" >"$OUTPUT_DIR/dmesg-camera.txt" 2>&1 || true
    fi
else
    echo "dependency missing: dmesg" >"$OUTPUT_DIR/dmesg-camera.txt"
fi

if command_exists media-ctl; then
    found_media=0
    for media_node in /dev/media*; do
        [ -e "$media_node" ] || continue
        found_media=1
        media_name=$(basename "$media_node")
        record_command "media-${media_name}.txt" media-ctl -d "$media_node" -p || true
    done
    if [ "$found_media" -eq 0 ]; then
        echo "no /dev/media* nodes found" >"$OUTPUT_DIR/media-none.txt"
    fi
else
    {
        echo "dependency missing: media-ctl"
        echo "Buildroot: BR2_PACKAGE_LIBV4L=y and BR2_PACKAGE_LIBV4L_UTILS=y"
    } >"$OUTPUT_DIR/media-tool-missing.txt"
fi

if command_exists v4l2-ctl; then
    record_command v4l2-list-devices.txt v4l2-ctl --list-devices || true
    found_v4l=0
    for video_node in /dev/video* /dev/v4l-subdev*; do
        [ -e "$video_node" ] || continue
        found_v4l=1
        node_name=$(basename "$video_node")
        record_command "${node_name}-all.txt" v4l2-ctl -d "$video_node" --all || true
        record_command "${node_name}-controls.txt" \
            v4l2-ctl -d "$video_node" --list-ctrls-menus || true
        case "$video_node" in
            /dev/video*)
                record_command "${node_name}-format-video.txt" \
                    v4l2-ctl -d "$video_node" --get-fmt-video || true
                ;;
            /dev/v4l-subdev*)
                record_command "${node_name}-format-subdev.txt" \
                    v4l2-ctl -d "$video_node" --get-subdev-fmt pad=0 || true
                ;;
        esac
    done
    if [ "$found_v4l" -eq 0 ]; then
        echo "no /dev/video* or /dev/v4l-subdev* nodes found" >"$OUTPUT_DIR/v4l2-none.txt"
    fi
else
    {
        echo "dependency missing: v4l2-ctl"
        echo "Buildroot: BR2_PACKAGE_LIBV4L=y and BR2_PACKAGE_LIBV4L_UTILS=y"
    } >"$OUTPUT_DIR/v4l2-tool-missing.txt"
fi

{
    for class_node in /sys/class/video4linux/*; do
        [ -e "$class_node" ] || continue
        class_name=$(basename "$class_node")
        printf '%s' "$class_name"
        if [ -r "$class_node/name" ]; then
            printf ' name='
            tr '\n' ' ' <"$class_node/name"
        fi
        if [ -L "$class_node/device/driver" ]; then
            printf ' driver=%s' "$(basename "$(readlink "$class_node/device/driver")")"
        fi
        printf '\n'
    done
} >"$OUTPUT_DIR/sysfs-video4linux.txt" 2>&1

echo "Baseline collection complete: $OUTPUT_DIR"
if [ -n "$missing" ]; then
    echo "WARNING: missing tools:${missing}" >&2
    echo "Enable BR2_PACKAGE_LIBV4L=y and BR2_PACKAGE_LIBV4L_UTILS=y in Buildroot." >&2
fi
