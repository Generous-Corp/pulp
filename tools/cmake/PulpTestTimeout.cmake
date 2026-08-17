# PulpTestTimeout.cmake — scale per-test wall-clock budgets by build config.
#
# Every `TIMEOUT` a test suite declares is written for an ordinary optimized
# build. A coverage build is a different machine: `-O0` with no inlining, plus
# an instrumentation counter update on every region. The coverage lane also
# runs on a shared host, so wall time there measures contention as much as it
# measures the test.
#
# The result is a false timeout. The `fdn reverb stays bounded and decaying for
# every parameter vector` case stayed CPU-active and was killed at its 900s
# property while a contended M5 ran 18,520 tests in 4,598s. `ctest --timeout`
# does NOT rescue this: a command-line timeout is only a default, and a per-test
# TIMEOUT property always wins.
#
# The fix scales the budget; it deliberately does NOT remove it. An unbounded
# test cannot distinguish "slow under instrumentation" from "wedged", which is
# the only question a timeout exists to answer. Every scaled budget therefore
# stays finite and is clamped to a hard ceiling.
#
# Tests: tools/cmake/test_pulp_test_timeout.cmake (arithmetic and clamping) and
# test/test_ctest_timeout_kills_wedged.sh (a genuinely wedged process is still
# killed while scaling is active).

include_guard(GLOBAL)

# Empty means "decide from the build config" — see pulp_resolve_test_timeout_scale.
# Set it explicitly to pin a multiplier (a slow bare-metal runner, a bisect).
set(PULP_TEST_TIMEOUT_SCALE "" CACHE STRING
    "Multiplier applied to every pulp_add_test_suite TIMEOUT. Empty = auto.")

# Chosen from the observed gap rather than a round number: the case that
# triggered this needed more than 900s and the same full run completed 18,520
# tests in 4,598s, so 4x turns a 900s budget into 3600s — comfortably above the
# real cost, still far below "never".
set(PULP_TEST_TIMEOUT_COVERAGE_SCALE 4 CACHE STRING
    "Multiplier used when the build tree is coverage-instrumented.")

# A ceiling is what keeps a scaled budget honest. Without it a large TIMEOUT
# times a large scale becomes effectively unbounded, and a wedged test would
# hold a CI lane for the length of the job instead of failing.
set(PULP_TEST_TIMEOUT_CEILING 7200 CACHE STRING
    "Hard upper bound, in seconds, on any scaled test TIMEOUT.")

# Resolve the multiplier for this build tree into `out_var`.
#
# Explicit configuration wins over inference, so a runner that is slow for some
# reason Pulp does not model can be pinned without teaching this file about it.
function(pulp_resolve_test_timeout_scale out_var)
    if(NOT "${PULP_TEST_TIMEOUT_SCALE}" STREQUAL "")
        if(NOT PULP_TEST_TIMEOUT_SCALE MATCHES "^[0-9]+$" OR PULP_TEST_TIMEOUT_SCALE LESS 1)
            message(FATAL_ERROR
                "PULP_TEST_TIMEOUT_SCALE must be an integer >= 1 "
                "(got '${PULP_TEST_TIMEOUT_SCALE}'). A scale below 1 would "
                "shorten every budget and manufacture timeouts.")
        endif()
        set(${out_var} "${PULP_TEST_TIMEOUT_SCALE}" PARENT_SCOPE)
        return()
    endif()
    if(PULP_COVERAGE_ENABLED)
        set(${out_var} "${PULP_TEST_TIMEOUT_COVERAGE_SCALE}" PARENT_SCOPE)
        return()
    endif()
    set(${out_var} 1 PARENT_SCOPE)
endfunction()

# Scale `seconds` into `out_var`, clamped to PULP_TEST_TIMEOUT_CEILING.
#
# A non-numeric or non-positive input is passed through untouched: this helper
# exists to widen a real budget, not to invent one, and silently turning a
# malformed value into a number would hide the mistake.
function(pulp_scaled_test_timeout out_var seconds)
    if(NOT "${seconds}" MATCHES "^[0-9]+$" OR "${seconds}" LESS 1)
        set(${out_var} "${seconds}" PARENT_SCOPE)
        return()
    endif()
    pulp_resolve_test_timeout_scale(_scale)
    math(EXPR _scaled "${seconds} * ${_scale}")
    if(_scaled GREATER PULP_TEST_TIMEOUT_CEILING)
        set(_scaled "${PULP_TEST_TIMEOUT_CEILING}")
    endif()
    # Clamping must never shorten a budget the author already chose. A suite
    # that legitimately declares more than the ceiling keeps its own value.
    if(_scaled LESS seconds)
        set(_scaled "${seconds}")
    endif()
    set(${out_var} "${_scaled}" PARENT_SCOPE)
endfunction()
