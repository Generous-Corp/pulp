# Arithmetic and clamping tests for PulpTestTimeout.cmake.
#
# Script mode (`cmake -P`) so the contract is provable without configuring
# Pulp: the whole point of the helper is that a coverage tree and a normal tree
# disagree, and building both to find out would cost more than the bug.
#
# The wedged-process half of the contract cannot be asserted here — that needs
# a real CTest run and lives in test/test_ctest_timeout_kills_wedged.sh.

cmake_minimum_required(VERSION 3.20)

set(_failures 0)

function(expect what expected actual)
    if("${expected}" STREQUAL "${actual}")
        message(STATUS "  ok   ${what}")
    else()
        message(STATUS "  FAIL ${what}: expected '${expected}', got '${actual}'")
        math(EXPR _f "${_failures} + 1")
        set(_failures "${_f}" PARENT_SCOPE)
    endif()
endfunction()

include(${CMAKE_CURRENT_LIST_DIR}/PulpTestTimeout.cmake)

# --- default build: budgets are untouched ------------------------------------
# The scaling must be invisible to an ordinary PR run, or it changes the
# meaning of every existing TIMEOUT in the tree.
set(PULP_COVERAGE_ENABLED FALSE)
set(PULP_TEST_TIMEOUT_SCALE "")
pulp_scaled_test_timeout(_out 900)
expect("uninstrumented build leaves a 900s budget alone" "900" "${_out}")

# --- coverage build: budget widens -------------------------------------------
# This is the GEN-28 case: the fdn stability sweep at TIMEOUT 900 was killed
# while still CPU-active on a contended coverage lane.
set(PULP_COVERAGE_ENABLED TRUE)
pulp_scaled_test_timeout(_out 900)
expect("coverage build scales 900s to 3600s" "3600" "${_out}")

# --- the budget stays finite --------------------------------------------------
# A scaled budget that exceeds the ceiling is clamped, not removed. Without
# this a large TIMEOUT times a large scale is effectively unbounded, and a
# wedged test would hold a CI lane for the whole job instead of failing.
pulp_scaled_test_timeout(_out 3000)
expect("scaled budget is clamped to the ceiling" "3600" "${_out}")

# The largest budget actually declared in the tree is 900s. It must still get
# its full 4x scale without clamping, or the ceiling is silently shortening the
# very budgets this file exists to widen.
pulp_scaled_test_timeout(_out 900)
expect("the largest declared budget is not clamped" "3600" "${_out}")

# The ceiling must stay STRICTLY below the CI lane's job budget. Equal is the
# bug this replaced: a test clamped at the job cap cannot time out first, so
# the JOB is killed instead and the run reports `cancelled` with no test named.
set(_lane_job_timeout 7200)
if(NOT PULP_TEST_TIMEOUT_CEILING LESS _lane_job_timeout)
    message(STATUS "  FAIL ceiling ${PULP_TEST_TIMEOUT_CEILING} is not below the lane job budget ${_lane_job_timeout}")
    math(EXPR _failures "${_failures} + 1")
else()
    message(STATUS "  ok   ceiling stays below the lane job budget, so a hung test names itself")
endif()

# --- clamping never shortens an authored budget -------------------------------
# A suite that deliberately declares more than the ceiling keeps its own value;
# the helper exists to widen budgets, never to cut one down.
pulp_scaled_test_timeout(_out 9000)
expect("a budget already above the ceiling is preserved" "9000" "${_out}")

# --- explicit configuration wins over inference -------------------------------
set(PULP_TEST_TIMEOUT_SCALE 2)
pulp_scaled_test_timeout(_out 900)
expect("an explicit scale overrides the coverage default" "1800" "${_out}")

set(PULP_COVERAGE_ENABLED FALSE)
pulp_scaled_test_timeout(_out 100)
expect("an explicit scale applies without coverage too" "200" "${_out}")

# --- malformed input is passed through, not invented into a number ------------
# Silently coercing a bad value would hide the authoring mistake.
set(PULP_TEST_TIMEOUT_SCALE "")
pulp_scaled_test_timeout(_out "notanumber")
expect("a non-numeric budget is passed through untouched" "notanumber" "${_out}")
pulp_scaled_test_timeout(_out 0)
expect("a zero budget is passed through untouched" "0" "${_out}")

# --- a scale below 1 is rejected ----------------------------------------------
# It would shorten every budget in the tree and manufacture timeouts, so it
# must fail configuration rather than quietly apply.
set(PULP_TEST_TIMEOUT_SCALE 0)
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DPULP_TEST_TIMEOUT_SCALE=0
        -P ${CMAKE_CURRENT_LIST_DIR}/test_pulp_test_timeout_reject.cmake
    RESULT_VARIABLE _reject_rc
    OUTPUT_QUIET ERROR_QUIET)
if(_reject_rc EQUAL 0)
    message(STATUS "  FAIL a scale of 0 was accepted")
    math(EXPR _failures "${_failures} + 1")
else()
    message(STATUS "  ok   a scale below 1 fails configuration")
endif()

if(_failures GREATER 0)
    message(FATAL_ERROR "PulpTestTimeout: ${_failures} assertion(s) failed")
endif()
message(STATUS "PASS: PulpTestTimeout.cmake")
