#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")

for script in "$PROJECT_DIR"/scripts/*.sh "$PROJECT_DIR"/tools/*.sh; do
    sh -n "$script"
    echo "syntax OK: $script"
done

if command -v shellcheck >/dev/null 2>&1; then
    shellcheck -s sh "$PROJECT_DIR"/scripts/*.sh
    echo "shellcheck OK"
else
    echo "shellcheck not installed; POSIX sh syntax check completed only"
fi
