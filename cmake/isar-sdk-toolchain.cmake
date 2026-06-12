# cmake/isar-sdk-toolchain.cmake
#
# CMake toolchain for building CoreRaT against the RaTOS ISAR SDK.
#
# The RaTOS SDK is a Debian Trixie amd64 sysroot containing libevl, RACK,
# SeRTial, and all CoreRaT dependencies.  The SDK ships its own gcc-14 with
# a sysroot-wrapper (gcc-sysroot-wrapper.sh) that auto-injects
# --sysroot=<sdk> into every compiler invocation.
#
# Usage:
#   export EVL_SDK_DIR=.evl-cache/sdk
#   cmake --preset evl-cross
#
# The CommRaT scripts/evl-dev.sh script handles SDK download and relocation.

if(DEFINED ENV{EVL_SDK_DIR})
    if(IS_ABSOLUTE "$ENV{EVL_SDK_DIR}")
        set(_EVL_SDK "$ENV{EVL_SDK_DIR}" CACHE PATH "RaTOS ISAR SDK root" FORCE)
    else()
        set(_EVL_SDK "${CMAKE_SOURCE_DIR}/$ENV{EVL_SDK_DIR}" CACHE PATH "RaTOS ISAR SDK root" FORCE)
    endif()
elseif(NOT DEFINED CACHE{_EVL_SDK})
    message(FATAL_ERROR
        "EVL_SDK_DIR environment variable is not set.\n"
        "Run 'scripts/evl-dev.sh --cross' to download the SDK to .evl-cache/sdk,\n"
        "or export EVL_SDK_DIR=/path/to/sdk before invoking cmake directly.")
endif()

set(CMAKE_C_COMPILER   "${_EVL_SDK}/usr/bin/x86_64-linux-gnu-gcc" CACHE FILEPATH "C compiler"   FORCE)
set(CMAKE_CXX_COMPILER "${_EVL_SDK}/usr/bin/x86_64-linux-gnu-g++" CACHE FILEPATH "C++ compiler" FORCE)

set(CMAKE_FIND_ROOT_PATH "${_EVL_SDK}")

list(PREPEND CMAKE_PREFIX_PATH "${_EVL_SDK}/usr")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
