# SPDX-License-Identifier: MIT
#
# pulp-internal task #35 — CMake smoke for the Debug-SDK perf-killer guard
# in PulpConfig.cmake.
#
# Runs three scenarios against a tempdir-fabricated SDK install:
#
#   1. sdk_build_type.txt == "Debug", PULP_ALLOW_DEBUG_SDK unset.
#      Expect: FATAL_ERROR from PulpConfig.cmake.
#   2. sdk_build_type.txt == "Debug", PULP_ALLOW_DEBUG_SDK=ON.
#      Expect: WARNING, configure succeeds.
#   3. sdk_build_type.txt == "Release". Expect: silent, configure succeeds.
#
# The fixture SDK only contains the files the guard reads
# (sdk_build_type.txt + a minimal PulpConfig.cmake that includes the same
# guard block); we do NOT need a full SDK install to validate the
# guard's branch logic. This keeps the test cheap and CI-portable.

cmake_minimum_required(VERSION 3.20)

# Args plumbed via `cmake -P`:
#   PULP_SRC_DIR        — repo root (so we can pull the cmake-in file).
#   FIXTURE_DIR         — tempdir to stage the fake SDK under.
#   SCENARIO            — "debug-no-override" | "debug-with-override" | "release"
#   EXPECTED_OUTCOME    — "fail" | "warn" | "silent"

if(NOT PULP_SRC_DIR OR NOT FIXTURE_DIR OR NOT SCENARIO OR NOT EXPECTED_OUTCOME)
    message(FATAL_ERROR
        "test_debug_sdk_guard.cmake requires:\n"
        "  -DPULP_SRC_DIR=...   -DFIXTURE_DIR=...   -DSCENARIO=...   -DEXPECTED_OUTCOME=...")
endif()

# B3 (2026-05): the Debug-SDK guard block was extracted from
# PulpConfig.cmake.in into PulpSdkGuards.cmake. The fixture follows
# the move — the regex below still locates the same guard text in
# the new home. If a future refactor unifies these again, point
# `_template` back at PulpConfig.cmake.in.
set(_template "${PULP_SRC_DIR}/tools/cmake/PulpSdkGuards.cmake")
if(NOT EXISTS "${_template}")
    message(FATAL_ERROR "PulpSdkGuards.cmake not found at ${_template}")
endif()

# Fabricate a minimal SDK layout under FIXTURE_DIR/sdk-install/.
set(_sdk_dir "${FIXTURE_DIR}/sdk-install")
file(MAKE_DIRECTORY "${_sdk_dir}/lib/cmake/Pulp")
file(WRITE "${_sdk_dir}/version.txt" "0.0.0-fixture\n")
if(SCENARIO STREQUAL "debug-no-override" OR SCENARIO STREQUAL "debug-with-override")
    file(WRITE "${_sdk_dir}/sdk_build_type.txt" "Debug\n")
elseif(SCENARIO STREQUAL "debug-lowercase")
    # Codex P1 — CMake accepts CMAKE_BUILD_TYPE case-insensitively
    # (so `-DCMAKE_BUILD_TYPE=debug` is a real Debug build), and the
    # marker preserves the user's spelling. Pin that the guard
    # normalizes to lowercase before comparing — otherwise it
    # silently bypasses on a real Debug install.
    file(WRITE "${_sdk_dir}/sdk_build_type.txt" "debug\n")
elseif(SCENARIO STREQUAL "debug-mixed-case")
    # Same gotcha, with the camelCase spelling CMake also accepts.
    file(WRITE "${_sdk_dir}/sdk_build_type.txt" "DeBuG\n")
elseif(SCENARIO STREQUAL "release")
    file(WRITE "${_sdk_dir}/sdk_build_type.txt" "Release\n")
else()
    message(FATAL_ERROR "Unknown scenario: ${SCENARIO}")
endif()

# Extract just the guard block from the real PulpConfig.cmake.in so we
# don't accidentally execute parts of the template that depend on
# @PACKAGE_INIT@ or other configure-time substitutions.
file(READ "${_template}" _template_content)
string(REGEX MATCH
       "# Debug-SDK perf-killer guard.*unset\\(_pulp_sdk_build_type_marker\\)"
       _guard_block "${_template_content}")
if(NOT _guard_block)
    message(FATAL_ERROR
        "Could not locate the Debug-SDK guard block in ${_template}. "
        "Did the comment header change?")
endif()

# Wrap the guard with the variables it depends on (PULP_SDK_DIR + the
# user's PULP_ALLOW_DEBUG_SDK opt-in).
set(_guard_cmake "${FIXTURE_DIR}/run_guard.cmake")
file(WRITE "${_guard_cmake}"
"set(PULP_SDK_DIR \"${_sdk_dir}\")\n"
"${_guard_block}\n"
"message(STATUS \"guard completed without FATAL_ERROR\")\n")

# Invoke that snippet via `cmake -P`, capturing stdout/stderr.
set(_allow_arg "")
if(SCENARIO STREQUAL "debug-with-override")
    set(_allow_arg "-DPULP_ALLOW_DEBUG_SDK=ON")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_allow_arg} -P "${_guard_cmake}"
    OUTPUT_VARIABLE _guard_stdout
    ERROR_VARIABLE  _guard_stderr
    RESULT_VARIABLE _guard_result)

set(_combined "${_guard_stdout}\n${_guard_stderr}")

if(EXPECTED_OUTCOME STREQUAL "fail")
    if(_guard_result EQUAL 0)
        message(FATAL_ERROR
            "Expected guard to FATAL_ERROR but it exited 0.\n"
            "stdout: ${_guard_stdout}\nstderr: ${_guard_stderr}")
    endif()
    if(NOT _combined MATCHES "was built with[ \t\r\n]+CMAKE_BUILD_TYPE=Debug")
        message(FATAL_ERROR
            "Guard failed (exit ${_guard_result}) but the message did "
            "not mention the Debug build type.\n"
            "stdout: ${_guard_stdout}\nstderr: ${_guard_stderr}")
    endif()
    message(STATUS "OK: scenario '${SCENARIO}' produced the expected FATAL_ERROR.")
elseif(EXPECTED_OUTCOME STREQUAL "warn")
    if(NOT _guard_result EQUAL 0)
        message(FATAL_ERROR
            "Expected guard to WARN-but-proceed but it exited ${_guard_result}.\n"
            "stdout: ${_guard_stdout}\nstderr: ${_guard_stderr}")
    endif()
    if(NOT _combined MATCHES "CMake Warning" AND NOT _combined MATCHES "PULP_ALLOW_DEBUG_SDK=ON")
        message(FATAL_ERROR
            "Expected the override path to log a warning, but no warning "
            "marker found.\nstdout: ${_guard_stdout}\nstderr: ${_guard_stderr}")
    endif()
    message(STATUS "OK: scenario '${SCENARIO}' warned and proceeded.")
elseif(EXPECTED_OUTCOME STREQUAL "silent")
    if(NOT _guard_result EQUAL 0)
        message(FATAL_ERROR
            "Expected guard to be silent (exit 0) for Release SDK but exited ${_guard_result}.\n"
            "stdout: ${_guard_stdout}\nstderr: ${_guard_stderr}")
    endif()
    # Match the guard's specific phrasing rather than the literal token
    # "FATAL_ERROR" (which appears as plain text in the success-path
    # status message printed at the bottom of run_guard.cmake).
    if(_combined MATCHES "was built with[ \t\r\n]+CMAKE_BUILD_TYPE=Debug"
       OR _combined MATCHES "CMake Warning")
        message(FATAL_ERROR
            "Expected guard to stay silent on Release SDK but it emitted "
            "an error/warning.\nstdout: ${_guard_stdout}\nstderr: ${_guard_stderr}")
    endif()
    message(STATUS "OK: scenario '${SCENARIO}' silent on Release SDK.")
else()
    message(FATAL_ERROR "Unknown EXPECTED_OUTCOME: ${EXPECTED_OUTCOME}")
endif()
