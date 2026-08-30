#[[
Linux min-OS architecture projection test. The x64 aggregate floor is known,
but the ARM64 aggregate is not; Pulp must never reuse a cached x64 ceiling for
an ARM64 build.
]]

cmake_minimum_required(VERSION 3.24)

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PULP_MIN_OS_JSON "${_repo_root}/tools/deps/min_os.json")
set(PULP_HOST_UNAME "Linux")

set(PULP_LINUX_ARCH "x86_64")
include("${_repo_root}/tools/cmake/PulpMinOs.cmake")
if(NOT PULP_LINUX_GLIBC_FLOOR STREQUAL "2.34")
    message(FATAL_ERROR
        "linux-x64 floor regression: expected 2.34, got '${PULP_LINUX_GLIBC_FLOOR}'")
endif()

set(PULP_LINUX_ARCH "aarch64")
include("${_repo_root}/tools/cmake/PulpMinOs.cmake")
if(DEFINED PULP_LINUX_GLIBC_FLOOR AND NOT PULP_LINUX_GLIBC_FLOOR STREQUAL "")
    message(FATAL_ERROR
        "linux-arm64 incorrectly inherited x64 floor '${PULP_LINUX_GLIBC_FLOOR}'")
endif()

message(STATUS "Linux min-OS architecture projection: x64 known, ARM64 unknown. OK.")
