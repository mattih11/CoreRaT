# run_router_test.cmake — starts corerat-router-tcp, runs the test binary, kills the router.
# Called by CTest via add_test(... COMMAND cmake -P run_router_test.cmake)
# Required variables (passed via -D):
#   ROUTER_EXE  — path to corerat-router-tcp
#   TEST_EXE    — path to test_router_tcp

cmake_minimum_required(VERSION 3.25)

if(NOT ROUTER_EXE)
    message(FATAL_ERROR "ROUTER_EXE not set")
endif()
if(NOT TEST_EXE)
    message(FATAL_ERROR "TEST_EXE not set")
endif()

# ---- Start router in background ----
execute_process(
    COMMAND ${ROUTER_EXE} --port 2000
    TIMEOUT 1       # returns after 1s without blocking (router stays running)
    OUTPUT_QUIET
    ERROR_QUIET
    RESULT_VARIABLE _start_result
)
# Non-zero is fine here — router is a daemon and doesn't exit on its own

# Give the router a moment to bind the port
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 0.3)

# ---- Run the smoke test ----
execute_process(
    COMMAND ${TEST_EXE}
    RESULT_VARIABLE _test_result
    TIMEOUT 10
)

# ---- Kill the router ----
find_program(PKILL pkill)
if(PKILL)
    execute_process(
        COMMAND ${PKILL} -f "corerat-router-tcp"
        OUTPUT_QUIET ERROR_QUIET
    )
endif()

if(NOT _test_result EQUAL 0)
    message(FATAL_ERROR "test_router_tcp FAILED (exit code ${_test_result})")
endif()
