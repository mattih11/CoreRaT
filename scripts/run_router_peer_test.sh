#!/usr/bin/env bash
# scripts/run_router_peer_test.sh
#
# Starts two corerat-router-tcp instances peered together, runs the peer
# integration test binary, then kills both routers.
#
# Router A: port 2000 --peer 127.0.0.1:2001
# Router B: port 2001 --peer 127.0.0.1:2000
#
# Called from CTest via:
#   add_test(NAME test_router_peer
#       COMMAND bash <script> <router_exe> <test_exe>)

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <router_exe> <test_exe>" >&2
    exit 1
fi

ROUTER_EXE="$1"
TEST_EXE="$2"

# ---- Start router A (port 2000) ----
"$ROUTER_EXE" --port 2000 --peer 127.0.0.1:2001 &
ROUTER_A_PID=$!

# ---- Start router B (port 2001) ----
"$ROUTER_EXE" --port 2001 --peer 127.0.0.1:2000 &
ROUTER_B_PID=$!

cleanup() {
    kill "$ROUTER_A_PID" 2>/dev/null || true
    kill "$ROUTER_B_PID" 2>/dev/null || true
    wait "$ROUTER_A_PID" 2>/dev/null || true
    wait "$ROUTER_B_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Give routers time to bind ports and establish the peer connection
sleep 0.5

# ---- Run the peer integration test ----
"$TEST_EXE"
