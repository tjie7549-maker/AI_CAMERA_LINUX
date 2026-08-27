#!/usr/bin/env bash

set -euo pipefail

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$PROJECT_DIR/build"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S "$PROJECT_DIR" -B "$BUILD_DIR"
fi

cmake --build "$BUILD_DIR" -j"$(nproc)"

# Run continuously unless the caller supplies a later --duration option.
exec "$PROJECT_DIR/run_ai_monitor.sh" --duration 0 "$@"
