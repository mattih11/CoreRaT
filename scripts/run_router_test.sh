#!/usr/bin/env bash
# scripts/run_router_test.sh
#
# Starts corerat-router-tcp, runs the test binary, and kills the router.
# Called from CTest via:
#   add_test(NAME test_router_tcp
#       COMMAND bash <script> <router_exe> <test_exe>)
#
# Replaces the old run_router_test.cmake which used cmake execute_process
# TIMEOUT — that approach killed the router before the test could connect.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <router_exe> <test_exe>" >&2
    exit 1
fi

ROUTER_EXE="$1"
TEST_EXE="$2"

# ---- Start router in background ----
"$ROUTER_EXE" --port 2000 &
ROUTER_PID=$!

cleanup() {
    kill "$ROUTER_PID" 2>/dev/null || true
    wait "$ROUTER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Give router time to bind the port
sleep 0.3

# ---- Run the smoke test ----
"$TEST_EXE"
