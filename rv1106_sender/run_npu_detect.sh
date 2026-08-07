#!/bin/sh
# Standalone launcher for the RV1106 local NPU person detection daemon.
# Normally started by run_rv1106_supervisor.sh (see start_npu).
NPU_DIR=/root/userdata/npu_detect
NPU_BIN=$NPU_DIR/npu_detect
NPU_MODEL=$NPU_DIR/yolov5n_320.rknn
NPU_LOG=/root/userdata/ai_camera/logs/npu_detect.log
SERVER_IP=${NPU_SERVER_IP:-192.168.50.1}
SERVER_PORT=${NPU_SERVER_PORT:-9010}
INTERVAL_MS=${NPU_INTERVAL_MS:-300}

exec env LD_LIBRARY_PATH="$NPU_DIR" "$NPU_BIN" \
  --model "$NPU_MODEL" --server-ip "$SERVER_IP" \
  --port "$SERVER_PORT" --interval-ms "$INTERVAL_MS" \
  >>"$NPU_LOG" 2>&1
