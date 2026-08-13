cmake_minimum_required(VERSION 3.25)
project(CoreRaT VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# ============================================================================
# Platform backend selection
# ============================================================================

set(CORERAT_PLATFORM "STD" CACHE STRING "Platform backend (STD or EVL)")
set_property(CACHE CORERAT_PLATFORM PROPERTY STRINGS STD EVL)

if(NOT CORERAT_PLATFORM MATCHES "^(STD|EVL)$")
    message(FATAL_ERROR "CORERAT_PLATFORM must be STD or EVL, got: ${CORERAT_PLATFORM}")
endif()

# ============================================================================
# IPC backend selection (AUTO = follow platform)
# ============================================================================

set(CORERAT_IPC "AUTO" CACHE STRING "IPC backend (TIMS, EVL, AUTO)")
set_property(CACHE CORERAT_IPC PROPERTY STRINGS TIMS EVL AUTO)

if(CORERAT_IPC STREQUAL "AUTO")
    if(CORERAT_PLATFORM STREQUAL "EVL")
        set(CORERAT_IPC "EVL")
    else()
        set(CORERAT_IPC "TIMS")
    endif()
endif()

message(STATUS "CoreRaT platform: ${CORERAT_PLATFORM}")
message(STATUS "CoreRaT IPC:      ${CORERAT_IPC}")

# ============================================================================
# Dependencies
# ============================================================================

find_package(SeRTial   REQUIRED)
find_package(Threads   REQUIRED)

if(CORERAT_PLATFORM STREQUAL "EVL")
    find_library(EVL_LIBRARY NAMES evl REQUIRED
        DOC "libevl runtime library (Xenomai 4 / libevl)")
    find_path(EVL_INCLUDE_DIR NAMES evl/thread.h REQUIRED
        DOC "libevl include directory")
    add_library(EVL::evl UNKNOWN IMPORTED)
    set_target_properties(EVL::evl PROPERTIES
        IMPORTED_LOCATION       "${EVL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${EVL_INCLUDE_DIR}")
endif()

# No external IPC dependency — TIMS uses direct POSIX sockets; EVL uses libevl (already found above)

# ============================================================================
# Header-only core library
# ============================================================================

add_library(corerat INTERFACE)
add_library(CoreRaT::corerat ALIAS corerat)

target_include_directories(corerat INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(corerat INTERFACE
    SeRTial::sertial
    Threads::Threads
    rt
)

target_compile_definitions(corerat INTERFACE
    CORERAT_PLATFORM_${CORERAT_PLATFORM}
)

# -march=native is intentionally NOT set here: corerat is an installed INTERFACE
# library whose compile flags propagate to every downstream consumer.  A native-
# tuned binary cannot be distributed — it will SIGILL on machines without the
# same ISA extensions.  Callers that want -march=native for local developer builds
# should add it to their own target or pass -DCMAKE_CXX_FLAGS=-march=native.
target_compile_options(corerat INTERFACE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>
)

if(CORERAT_PLATFORM STREQUAL "EVL")
    target_link_libraries(corerat INTERFACE EVL::evl)
endif()

# ============================================================================
# TiMS IPC backend (compiled TU — only when CORERAT_IPC_TIMS)
# ============================================================================

option(CORERAT_BUILD_TESTS   "Build CoreRaT tests"   ON)
option(CORERAT_BUILD_ROUTERS "Build CoreRaT routers" ON)

if(CORERAT_IPC STREQUAL "TIMS")
    add_library(corerat_tims STATIC src/tims_backend.cpp)
    add_library(CoreRaT::corerat_tims ALIAS corerat_tims)

    target_compile_definitions(corerat_tims PUBLIC CORERAT_IPC_TIMS)
    target_link_libraries(corerat_tims PUBLIC corerat)

    install(TARGETS corerat_tims
        EXPORT CoreRaTTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
elseif(CORERAT_IPC STREQUAL "EVL")
    # EVL backend is header-only — no compiled TU.
    # corerat_evl is an INTERFACE target that:
    #   1. Pulls in corerat (platform + messaging headers)
    #   2. Defines CORERAT_IPC_EVL so mailbox.hpp selects EvlMailbox
    #   3. Links libevl
    # Downstream consumers (CommRaT etc.) link CoreRaT::corerat_evl instead of
    # CoreRaT::corerat to get the EVL IPC backend.
    add_library(corerat_evl INTERFACE)
    add_library(CoreRaT::corerat_evl ALIAS corerat_evl)

    target_compile_definitions(corerat_evl INTERFACE CORERAT_IPC_EVL)
    target_link_libraries(corerat_evl INTERFACE corerat)

    install(TARGETS corerat_evl
        EXPORT CoreRaTTargets
    )
endif()

# ============================================================================
# Router binaries
# ============================================================================

# Optional ISA baseline for router binaries — defaults to empty (compiler default).
# Set to e.g. -march=x86-64-v3 for portable release packages (AVX2, no AVX-512).
# Never set this to -march=native for distributed binaries.
set(CORERAT_ROUTER_MARCH_FLAGS "" CACHE STRING
    "Extra -march flags applied only to router executables (e.g. -march=x86-64-v3)")

if(CORERAT_BUILD_ROUTERS)
    # TCP router — always available (no extra dependencies)
    add_executable(corerat-router-tcp src/router/tcp_router.cpp)
    target_link_libraries(corerat-router-tcp PRIVATE corerat)
    install(TARGETS corerat-router-tcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # EVL router — full TCP router with xbuf bridge (no EVL headers needed here;
    # it probes /dev/evl/xbuf/* at runtime using plain open(2)).
    # Built on all platforms so it can be deployed to EVL hosts from the cross-
    # compile preset.
    add_executable(corerat-router-evl src/router/evl_router.cpp)
    target_link_libraries(corerat-router-evl PRIVATE corerat)
    install(TARGETS corerat-router-evl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    if(CORERAT_ROUTER_MARCH_FLAGS)
        target_compile_options(corerat-router-tcp PRIVATE ${CORERAT_ROUTER_MARCH_FLAGS})
        target_compile_options(corerat-router-evl  PRIVATE ${CORERAT_ROUTER_MARCH_FLAGS})
    endif()
endif()

# ============================================================================
# CTest integration
# ============================================================================

if(CORERAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# ============================================================================
# Install
# ============================================================================

install(TARGETS corerat
    EXPORT CoreRaTTargets
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT CoreRaTTargets
    FILE      CoreRaTTargets.cmake
    NAMESPACE CoreRaT::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreRaT
)

include(CMakePackageConfigHelpers)

# (tests registered above via add_subdirectory)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreRaTConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CoreRaTConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreRaT
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CoreRaTConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/CoreRaTConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/CoreRaTConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreRaT
)

# ============================================================================
# CPack — produces corerat-std_<version>_amd64.deb for STD-platform CI
# Only active when CORERAT_PLATFORM=STD (the default preset).
# ============================================================================

if(CORERAT_PLATFORM STREQUAL "STD")
    set(CPACK_PACKAGE_NAME            "corerat-std")
    set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "CoreRaT runtime — STD platform (TiMS IPC)")
    set(CPACK_PACKAGE_CONTACT         "CoreRaT maintainers")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    # Runtime deps: libstdc++/libc are always present on ubuntu-latest.
    # SeRTial and reflectcpp are header-only and install into /usr/local —
    # no Debian package dependency can express that, so we leave it empty.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS  "")
    set(CPACK_GENERATOR               "DEB")
    set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}_amd64")
    include(CPack)
endif()
