#!/usr/bin/env bash
# scripts/run_pingpong_test.sh
#
# Starts (optionally) corerat-router-tcp + pong_node, runs ping_node.
#
# Arguments:
#   $1  ROUTER_EXE  — path to corerat-router-tcp, or "" to skip (EVL mode)
#   $2  PONG_EXE    — path to pong_node
#   $3  PING_EXE    — path to ping_node
#   $4  COUNT       — number of pings (default: 100)
#
# On TIMS:  starts the router, then pong + ping via TCP.
# On EVL:   pass ROUTER_EXE="" — pong and ping use direct SHM ring.

set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <router_exe|> <pong_exe> <ping_exe> [count]" >&2
    exit 1
fi

ROUTER_EXE="$1"
PONG_EXE="$2"
PING_EXE="$3"
COUNT="${4:-100}"

ROUTER_PID=""

cleanup() {
    [[ -n "$ROUTER_PID" ]] && kill "$ROUTER_PID" 2>/dev/null || true
    [[ -n "$ROUTER_PID" ]] && wait "$ROUTER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# ---- Start router (TIMS only) ----
if [[ -n "$ROUTER_EXE" && -f "$ROUTER_EXE" ]]; then
    "$ROUTER_EXE" --port 2000 &
    ROUTER_PID=$!
    sleep 0.3
fi

# ---- Start pong_node in background ----
"$PONG_EXE" --count "$COUNT" &
PONG_PID=$!

cleanup2() {
    kill "$PONG_PID" 2>/dev/null || true
    wait "$PONG_PID" 2>/dev/null || true
    cleanup
}
trap cleanup2 EXIT

# Give pong_node time to start and register its mailbox
sleep 0.5

# ---- Run ping_node (foreground) ----
"$PING_EXE" --count "$COUNT"
RESULT=$?

wait "$PONG_PID" 2>/dev/null || true
exit $RESULT
