enable_testing()

# ============================================================================
# Helper: add a header-only CoreRaT unit test
# ============================================================================

function(corerat_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE corerat)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 30
        LABELS  "unit")
endfunction()

# ============================================================================
# Unit tests
# ============================================================================

corerat_test(test_message_id)
corerat_test(test_registry_utils)
corerat_test(test_wire_message_sizing)

# ============================================================================
# Router smoke test + ping-pong integration test (only when routers are built)
# ============================================================================

if(CORERAT_BUILD_ROUTERS)
    # ---- Router smoke test ----
    add_executable(test_router_tcp test_router_tcp.cpp)
    target_link_libraries(test_router_tcp PRIVATE corerat)
    if(CORERAT_IPC STREQUAL "TIMS")
        target_link_libraries(test_router_tcp PRIVATE corerat_tims)
    endif()

    add_test(NAME test_router_tcp
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/run_router_test.sh
            $<TARGET_FILE:corerat-router-tcp>
            $<TARGET_FILE:test_router_tcp>)
    set_tests_properties(test_router_tcp PROPERTIES
        TIMEOUT 15
        LABELS  "integration")

    # ---- Ping-pong integration test ----
    add_executable(ping_node ping_pong/ping_node.cpp)
    target_link_libraries(ping_node PRIVATE corerat)
    target_include_directories(ping_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ping_pong)

    add_executable(pong_node ping_pong/pong_node.cpp)
    target_link_libraries(pong_node PRIVATE corerat)
    target_include_directories(pong_node PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ping_pong)

    add_test(NAME test_pingpong
        COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/run_pingpong_test.sh
            $<TARGET_FILE:corerat-router-tcp>
            $<TARGET_FILE:pong_node>
            $<TARGET_FILE:ping_node>
            100)
    set_tests_properties(test_pingpong PROPERTIES
        TIMEOUT 30
        LABELS  "integration")
endif()

