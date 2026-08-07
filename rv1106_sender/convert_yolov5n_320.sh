#!/usr/bin/env bash
# Convert airockchip yolov5n ONNX (320x320, RKNPU export) to an int8 RKNN model
# for RV1106. Requires the rknn-toolkit2 1.6.0 virtual environment and the
# luckfox_pico_rknn_example converter.
set -euo pipefail

ONNX=${1:-/home/summary/linux/tools/yolov5n.onnx}
DATASET=${2:-/tmp/yolov5n_320_dataset.txt}
OUT=${3:-/home/summary/linux/tools/yolov5n_320.rknn}
CONVERT_DIR=/home/summary/linux/tools/luckfox_pico_rknn_example/scripts/luckfox_onnx_to_rknn/convert
PYTHON=/home/summary/rknn-env/bin/python

[ -f "$ONNX" ] || { echo "missing onnx: $ONNX"; exit 1; }
[ -f "$DATASET" ] || { echo "missing dataset: $DATASET"; exit 1; }
cd "$CONVERT_DIR"
"$PYTHON" convert.py "$ONNX" "$DATASET" "$OUT" Yolov5
echo "converted: $OUT"
