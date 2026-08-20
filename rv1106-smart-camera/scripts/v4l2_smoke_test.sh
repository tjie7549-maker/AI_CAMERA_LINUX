#!/bin/sh

set -u

DEVICE=""
MEDIA_DEVICE=""
OUTPUT_DIR=""

usage() {
    cat <<'EOF'
Usage: v4l2_smoke_test.sh [-d /dev/videoN] [-m /dev/mediaN] [-o output_dir]

  -d  Explicit capture node confirmed from media topology (recommended)
  -m  Media device used for topology-based auto-discovery
  -o  Result directory; default is under /userdata/rv1106-smart-camera/tests
EOF
}

while getopts "d:m:o:h" option; do
    case "$option" in
        d) DEVICE=$OPTARG ;;
        m) MEDIA_DEVICE=$OPTARG ;;
        o) OUTPUT_DIR=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

dependency_error() {
    echo "ERROR: missing dependency: $1" >&2
    echo "Buildroot recommendation:" >&2
    echo "  BR2_PACKAGE_LIBV4L=y" >&2
    echo "  BR2_PACKAGE_LIBV4L_UTILS=y" >&2
    exit 2
}

command -v v4l2-ctl >/dev/null 2>&1 || dependency_error v4l2-ctl
if [ -z "$DEVICE" ]; then
    command -v media-ctl >/dev/null 2>&1 || dependency_error media-ctl
fi

if [ -z "$OUTPUT_DIR" ]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || date +%Y%m%d-%H%M%S)
    OUTPUT_DIR=/userdata/rv1106-smart-camera/tests/smoke-$timestamp
fi

if [ -e "$OUTPUT_DIR" ]; then
    echo "ERROR: output already exists; refusing to overwrite: $OUTPUT_DIR" >&2
    exit 1
fi
mkdir -p "$OUTPUT_DIR" || exit 1

discover_from_media() {
    media_node=$1
    topology_file=$2

    media-ctl -d "$media_node" -p >"$topology_file" 2>&1 || return 1
    awk '
        /^- entity [0-9]+:/ {
            entity=tolower($0)
        }
        /device node name \/dev\/video[0-9]+/ {
            if (entity ~ /rkisp_mainpath|mainpath|stream_cif_mipi_id0|scale_ch0/)
                print $NF
        }
    ' "$topology_file"
}

if [ -z "$DEVICE" ]; then
    candidate_file=$OUTPUT_DIR/candidates.txt
    : >"$candidate_file"

    if [ -n "$MEDIA_DEVICE" ]; then
        if [ ! -e "$MEDIA_DEVICE" ]; then
            echo "ERROR: media device does not exist: $MEDIA_DEVICE" >&2
            exit 1
        fi
        discover_from_media "$MEDIA_DEVICE" "$OUTPUT_DIR/topology-$(basename "$MEDIA_DEVICE").txt" \
            >>"$candidate_file" || true
    else
        for media_node in /dev/media*; do
            [ -e "$media_node" ] || continue
            discover_from_media "$media_node" \
                "$OUTPUT_DIR/topology-$(basename "$media_node").txt" >>"$candidate_file" || true
        done
    fi

    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        [ -e "$candidate" ] || continue
        if v4l2-ctl -d "$candidate" --all 2>&1 | \
            grep -Eq 'Video Capture|Video Capture Multiplanar'; then
            DEVICE=$candidate
            break
        fi
    done <"$candidate_file"

    if [ -z "$DEVICE" ]; then
        echo "ERROR: no unambiguous Rockchip capture node found from media topology." >&2
        echo "Inspect topology files in $OUTPUT_DIR and rerun with -d /dev/videoN." >&2
        exit 1
    fi
fi

case "$DEVICE" in
    /dev/video*) ;;
    *)
        echo "ERROR: smoke capture requires a /dev/video* capture node: $DEVICE" >&2
        exit 1
        ;;
esac
if [ ! -c "$DEVICE" ]; then
    echo "ERROR: not a character device: $DEVICE" >&2
    exit 1
fi

echo "$DEVICE" >"$OUTPUT_DIR/selected-device.txt"

run_logged() {
    output=$1
    shift
    (
        echo "command: $*"
        "$@"
        status=$?
        echo "exit_status: $status"
        exit "$status"
    ) >"$OUTPUT_DIR/$output" 2>&1
    return $?
}

failed=0
run_logged device-all.txt v4l2-ctl -d "$DEVICE" --all || failed=1
run_logged controls.txt v4l2-ctl -d "$DEVICE" --list-ctrls-menus || failed=1
run_logged format.txt v4l2-ctl -d "$DEVICE" --get-fmt-video || failed=1

frame_file=$OUTPUT_DIR/frame.raw
run_logged stream-one-frame.txt v4l2-ctl -d "$DEVICE" \
    --stream-mmap=3 --stream-count=1 --stream-to="$frame_file" || failed=1

if [ ! -s "$frame_file" ]; then
    echo "ERROR: captured frame is missing or empty: $frame_file" >&2
    failed=1
fi

if [ "$failed" -ne 0 ]; then
    echo "V4L2 smoke test FAILED; keep logs for diagnosis: $OUTPUT_DIR" >&2
    exit 1
fi

echo "V4L2 smoke test passed at command level: $OUTPUT_DIR"
echo "Board-side image correctness and control effects still require visual verification."
