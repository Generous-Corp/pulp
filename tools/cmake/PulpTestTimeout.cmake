# PulpTestTimeout.cmake — scale per-test wall-clock budgets by build config.
#
# Every `TIMEOUT` a test suite declares is written for an ordinary optimized
# build. An instrumented build is a different machine. A coverage build is
# `-O0` with no inlining, plus a counter update on every region. A sanitizer
# build is the same `-O0` tree (the ASan and TSan lanes both configure
# `CMAKE_BUILD_TYPE=Debug`) carrying a strictly more expensive check on every
# memory access. Both lanes also run on shared hosts, so wall time there
# measures contention as much as it measures the test.
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

# Sized against the coverage scale rather than picked, because the sanitizer
# lanes share coverage's dominant cost. `sanitizers.yml` configures ASan and
# TSan with `CMAKE_BUILD_TYPE=Debug`, so those trees pay the same `-O0`
# no-inlining penalty the 4x above was measured on, and then pay a shadow
# memory check per access where coverage pays a counter increment. That makes
# 4 a floor, not an estimate. On top of it the sanitizer runtimes carry their
# own documented multiplier over an equivalent uninstrumented tree — roughly
# 2x for ASan, considerably more for TSan — and composing the two axes gives
# 8. The UBSan lane is the in-tree corroboration: it alone configures
# RelWithDebInfo, and its comment says it does so because the Debug runtime of
# the certification suites is "many times their production runtime".
#
# One multiplier serves all four sanitizer lanes. A lane that needs a
# different number pins PULP_TEST_TIMEOUT_SCALE rather than teaching this file
# about each sanitizer, and the ceiling below bounds the result either way:
# any budget at or above 450s is clamped, so the largest suites are unaffected
# by the choice between 4 and 8 and only the small and medium budgets — which
# is where the observed sanitizer timeouts sit — gain headroom.
set(PULP_TEST_TIMEOUT_SANITIZER_SCALE 8 CACHE STRING
    "Multiplier used when the build tree is sanitizer-instrumented.")

# A ceiling is what keeps a scaled budget honest. Without it a large TIMEOUT
# times a large scale becomes effectively unbounded, and a wedged test would
# hold a CI lane for the length of the job instead of failing.
#
# It must stay STRICTLY BELOW the lane's own job budget, and the first version
# of this file got that wrong: the ceiling was 7200s, exactly the JIT runner's
# `job_timeout=7200s`. A test clamped at the job budget can never time out
# first -- the job is killed at the same instant, and the run reports
# `cancelled` with no failing test named. That is the opposite of the point.
# The whole reason to keep budgets finite is so a hung test IDENTIFIES itself
# instead of taking the job down anonymously, and a ceiling equal to the job
# cap converts every such case into an unattributed cancellation.
#
# 3600s is half the current job budget: high enough that the largest budget
# routed through this helper (900s) still gets its full 4x coverage scale
# without clamping, and low enough that a test which blows through it fails as
# a test. The larger sanitizer scale clamps at 450s rather than 900s; that is
# the ceiling doing its job, not a budget being cut, since clamping never
# returns less than the authored value.
set(PULP_TEST_TIMEOUT_CEILING 3600 CACHE STRING
    "Hard upper bound in seconds on any scaled test TIMEOUT; must stay below the CI lane job budget")

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
    # Coverage and sanitizers are mutually exclusive in a real configure --
    # PulpInstrumentation.cmake refuses the combination with a FATAL_ERROR --
    # but this module is also included in script mode, where that guard never
    # runs. Taking the larger of the two applicable scales makes the answer
    # independent of the order these branches happen to be written in, and it
    # is the smallest value that is never shorter than either lane's own
    # budget. Multiplying them would instead compound two estimates of the
    # same `-O0` cost.
    set(_resolved 1)
    if(PULP_COVERAGE_ENABLED AND PULP_TEST_TIMEOUT_COVERAGE_SCALE GREATER _resolved)
        set(_resolved "${PULP_TEST_TIMEOUT_COVERAGE_SCALE}")
    endif()
    if(PULP_SANITIZER AND PULP_TEST_TIMEOUT_SANITIZER_SCALE GREATER _resolved)
        set(_resolved "${PULP_TEST_TIMEOUT_SANITIZER_SCALE}")
    endif()
    set(${out_var} "${_resolved}" PARENT_SCOPE)
endfunction()

# Scale `seconds` into `out_var`, clamped to PULP_TEST_TIMEOUT_CEILING.
#
# A non-numeric or non-positive input is passed through untouched: this helper
# exists to widen a real budget, not to invent one, and silently turning a
# malformed value into a number would hide the mistake.
#
# Reach, so nobody reads a scaled lane as a fully covered one. Only a budget
# that comes through `pulp_add_test_suite` or through an explicit call to this
# helper is scaled. A budget written as a literal on a raw
# `set_tests_properties(... TIMEOUT n)` is not, and those are the majority of
# the declarations in the test manifests. A test that declares no TIMEOUT at
# all is out of reach by construction: it is governed by the lane's own
# `ctest --timeout` default, which no per-test property can widen. Widening
# either of those is a separate edit at the site or in the workflow.
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
