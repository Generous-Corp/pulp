#!/usr/bin/env bash
# Negative control for the coverage timeout scaling (GEN-28).
#
# The arithmetic is asserted in tools/cmake/test_pulp_test_timeout.cmake. This
# proves the part arithmetic cannot: that a genuinely wedged process is STILL
# killed while scaling is active. Widening a budget is only safe if the budget
# is still enforced — otherwise the fix for a false timeout has quietly removed
# the only mechanism that catches a real hang.
#
# Uses a `NONE`-language project so it needs no compiler and runs in seconds.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE="$ROOT/tools/cmake"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

cat >"$tmp/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(pulp_timeout_fixture NONE)
include(CTest)
include($MODULE/PulpTestTimeout.cmake)

# Pretend to be a coverage tree so the scale is genuinely in effect; a control
# that ran with scaling off would prove nothing about this change.
set(PULP_COVERAGE_ENABLED TRUE)
pulp_scaled_test_timeout(wedged_budget 2)

# 2s authored * 4 (coverage) = 8s. The command sleeps far longer, so the only
# way this test can end is CTest killing it.
add_test(NAME wedged COMMAND \${CMAKE_COMMAND} -E sleep 600)
set_tests_properties(wedged PROPERTIES TIMEOUT \${wedged_budget})

# A fast test alongside it, so a run that fails for some unrelated reason
# (bad fixture, missing ctest) is distinguishable from the timeout under test.
add_test(NAME quick COMMAND \${CMAKE_COMMAND} -E echo ok)
set_tests_properties(quick PROPERTIES TIMEOUT \${wedged_budget})

message(STATUS "fixture scaled budget: \${wedged_budget}")
EOF

configure_log="$tmp/configure.log"
cmake -S "$tmp" -B "$tmp/build" >"$configure_log" 2>&1 \
    || { cat "$configure_log"; fail "fixture failed to configure"; }

grep -q 'fixture scaled budget: 8' "$configure_log" \
    || fail "scaling did not apply to the fixture ($(grep 'scaled budget' "$configure_log" || echo 'no line'))"
echo "ok: coverage scaling widened the fixture budget 2s -> 8s"

started="$(date +%s)"
set +e
ctest --test-dir "$tmp/build" --output-on-failure >"$tmp/ctest.log" 2>&1
rc=$?
set -e
elapsed=$(( $(date +%s) - started ))

[ "$rc" -ne 0 ] || fail "ctest passed — a wedged test was not caught"
grep -qi 'timeout' "$tmp/ctest.log" || fail "ctest did not report a timeout: $(tail -5 "$tmp/ctest.log")"
grep -qE 'quick .*Passed' "$tmp/ctest.log" \
    || fail "the control test did not pass, so this run proves nothing: $(tail -8 "$tmp/ctest.log")"

# The sleep is 600s. Finishing in well under that is the actual evidence that
# the scaled budget was enforced rather than merely configured.
[ "$elapsed" -lt 60 ] \
    || fail "wedged test ran ${elapsed}s — the scaled budget was not enforced"

echo "ok: wedged process still killed at the scaled budget (run took ${elapsed}s, sleep was 600s)"
echo "PASS: ctest enforces the scaled timeout"
