#!/usr/bin/env bash
# End-to-end demo: build, start the server, fire orders at it, stop it,
# then replay the journal twice to show the event stream is deterministic.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null

PORT=${PORT:-9000}
JOURNAL=$(mktemp -t orderflow-XXXX.ofj)

./build/orderflow_server --port "$PORT" --journal "$JOURNAL" &
SERVER_PID=$!
trap 'kill -INT $SERVER_PID 2>/dev/null || true' EXIT
sleep 0.5

echo "== pipelined load (window 64) =="
./build/orderflow_client --port "$PORT" --orders 200000 --window 64

kill -INT "$SERVER_PID"
wait "$SERVER_PID" || true
trap - EXIT

echo
echo "== replay #1 =="
./build/orderflow_replay "$JOURNAL" | tail -1
echo "== replay #2 (hash must match) =="
./build/orderflow_replay "$JOURNAL" | tail -1
rm -f "$JOURNAL"
