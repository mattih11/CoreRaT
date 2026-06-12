#!/usr/bin/env bash
# scripts/run_pingpong_test.sh
#
# Starts corerat-router-tcp + pong_node, runs ping_node, reports results.
# Called from CTest via:
#   add_test(NAME test_pingpong
#       COMMAND bash <script> <router_exe> <pong_exe> <ping_exe> [--count N])
#
# Exit code mirrors ping_node's exit code (0 = all replies received).

set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <router_exe> <pong_exe> <ping_exe> [--count N]" >&2
    exit 1
fi

ROUTER_EXE="$1"
PONG_EXE="$2"
PING_EXE="$3"
COUNT="${4:-100}"

# ---- Start router in background ----
"$ROUTER_EXE" --port 2000 &
ROUTER_PID=$!

# ---- Start pong_node in background ----
"$PONG_EXE" --count "$COUNT" &
PONG_PID=$!

cleanup() {
    kill "$PONG_PID"   2>/dev/null || true
    kill "$ROUTER_PID" 2>/dev/null || true
    wait "$PONG_PID"   2>/dev/null || true
    wait "$ROUTER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Give router + pong_node time to start and register
sleep 0.5

# ---- Run ping_node (foreground) ----
"$PING_EXE" --count "$COUNT"
RESULT=$?

# Let pong_node finish cleanly before cleanup() kills it
wait "$PONG_PID" 2>/dev/null || true

exit $RESULT
