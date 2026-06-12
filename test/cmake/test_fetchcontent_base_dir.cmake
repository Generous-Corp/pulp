#[[
Smoke test for the Windows FetchContent base-dir guard in
tools/cmake/PulpFetchContent.cmake.

The Windows WebGPU prebuilt path can exceed MSBuild's MAX_PATH limit when
FetchContent leaves nested subbuilds under a deep Actions workspace. The guard
keeps transient FetchContent build trees in a short per-user cache directory
while still respecting explicit operator overrides.
]]

cmake_minimum_required(VERSION 3.20)

set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
get_filename_component(_repo_root "${_repo_root}" ABSOLUTE)

# Simulate the Windows branch without requiring a Windows host.
set(WIN32 TRUE)

set(ENV{PULP_FETCHCONTENT_BASE_DIR} "")
set(ENV{LOCALAPPDATA} "C:/Users/test/AppData/Local")
unset(FETCHCONTENT_BASE_DIR CACHE)
unset(FETCHCONTENT_BASE_DIR)

include("${_repo_root}/tools/cmake/PulpFetchContent.cmake")

set(_expected_default "C:/Users/test/AppData/Local/Pulp/fc")
if(NOT FETCHCONTENT_BASE_DIR STREQUAL "${_expected_default}")
    message(FATAL_ERROR
        "Expected default FetchContent base '${_expected_default}', got "
        "'${FETCHCONTENT_BASE_DIR}'.")
endif()

unset(FETCHCONTENT_BASE_DIR CACHE)
unset(FETCHCONTENT_BASE_DIR)
set(ENV{PULP_FETCHCONTENT_BASE_DIR} "D:/pulp/fc")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL "D:/pulp/fc")
    message(FATAL_ERROR
        "Expected PULP_FETCHCONTENT_BASE_DIR override, got "
        "'${FETCHCONTENT_BASE_DIR}'.")
endif()

set(FETCHCONTENT_BASE_DIR "E:/operator/fc" CACHE PATH "" FORCE)
set(ENV{PULP_FETCHCONTENT_BASE_DIR} "D:/ignored/fc")
pulp_configure_fetchcontent_base_dir()
if(NOT FETCHCONTENT_BASE_DIR STREQUAL "E:/operator/fc")
    message(FATAL_ERROR
        "Expected existing FETCHCONTENT_BASE_DIR to win, got "
        "'${FETCHCONTENT_BASE_DIR}'.")
endif()

message(STATUS "FetchContent Windows base-dir guard: all cases pass.")
