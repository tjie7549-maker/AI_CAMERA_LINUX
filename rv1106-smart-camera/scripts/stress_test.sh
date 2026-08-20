#!/bin/sh
# Run on board after daemon is started. Uses camera-daemon's built-in Socket client.
set -eu

root=/userdata/rv1106-smart-camera
socket=$root/run/camera-daemon.sock
loops=${1:-10}
case "$loops" in *[!0-9]*|'') echo "Usage: $0 [positive-loop-count]" >&2; exit 2;; esac
[ "$loops" -gt 0 ] || exit 2
[ -S "$socket" ] || { echo "daemon socket missing: $socket" >&2; exit 4; }
result=$root/logs/stress-$(date +%Y%m%dT%H%M%S).log
echo "loops=$loops started=$(date)" >"$result"
i=1
while [ "$i" -le "$loops" ]; do
  if "$root/bin/camera-daemon" --request "$socket" '{"cmd":"restart_pipeline"}' >>"$result" 2>&1; then
    echo "loop=$i request=ok" >>"$result"
  else
    echo "loop=$i request=failed" >>"$result"
  fi
  sleep 2
  i=$((i + 1))
done
echo "completed=$(date); inspect events.jsonl and this log before making any stability claim" >>"$result"
echo "Result: $result"
