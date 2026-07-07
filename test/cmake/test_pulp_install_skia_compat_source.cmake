#[[
Install-layout regression: Linux Skia raw_ptr compatibility source.

FindSkia.cmake can compile a tiny Pulp-owned compatibility source when the
prebuilt Linux Skia archive references Chromium BackupRefPtr / PartitionAlloc
support symbols but the standalone Skia bundle does not ship PartitionAlloc.
Installed SDK consumers need that source under <prefix>/src/pulp/canvas/.

Inputs (passed via -D):
  PULP_BUILD_DIR — path to a configured Pulp build directory.
]]

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR
        "test_pulp_install_skia_compat_source.cmake requires "
        "-DPULP_BUILD_DIR=<dir> pointing at a configured Pulp build.")
endif()
get_filename_component(PULP_BUILD_DIR "${PULP_BUILD_DIR}" ABSOLUTE)
if(NOT EXISTS "${PULP_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "PULP_BUILD_DIR=${PULP_BUILD_DIR} is not a configured CMake build "
        "directory (no CMakeCache.txt).")
endif()

set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-skia-compat-prefix")
file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            --install "${PULP_BUILD_DIR}"
            --prefix  "${_prefix}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE  _stderr)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "cmake --install failed (rc=${_rc}):\n"
        "----- stdout -----\n${_stdout}\n"
        "----- stderr -----\n${_stderr}\n----- end -----")
endif()

set(_compat_source
    "${_prefix}/src/pulp/canvas/skia_chromium_raw_ptr_compat.cpp")
if(NOT EXISTS "${_compat_source}")
    message(FATAL_ERROR
        "Installed Pulp SDK is missing the Linux Skia raw_ptr compatibility "
        "source expected by FindSkia.cmake:\n"
        "  ${_compat_source}")
endif()

file(GLOB_RECURSE _find_skia_candidates
    "${_prefix}/lib*/cmake/Pulp/FindSkia.cmake")
list(LENGTH _find_skia_candidates _find_skia_count)
if(_find_skia_count EQUAL 0)
    message(FATAL_ERROR "Installed FindSkia.cmake not found under ${_prefix}.")
endif()
list(GET _find_skia_candidates 0 _find_skia)
file(READ "${_find_skia}" _find_skia_text)
if(NOT _find_skia_text MATCHES "skia_chromium_raw_ptr_compat\\.cpp")
    message(FATAL_ERROR
        "Installed FindSkia.cmake does not know how to find the Linux Skia "
        "raw_ptr compatibility source.")
endif()

message(STATUS
    "Install layout: Linux Skia raw_ptr compatibility source is present "
    "and FindSkia.cmake references it.")
