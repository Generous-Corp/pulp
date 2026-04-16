#!/usr/bin/env bash
# run_ubsan.sh — UndefinedBehaviorSanitizer-only build + test run.
#
# run_asan.sh already enables UBSan alongside ASan (via
# -fsanitize=address,undefined), which is the usual case. This
# separate wrapper exists for two reasons (Codex P2 on #317):
# 1. ASan has higher overhead and can mask signed-overflow / null-
#    deref patterns that UBSan alone catches faster.
# 2. Some UBSan checks collide with ASan's shadow memory; running
#    UBSan alone gives cleaner reports for those classes.
#
# Uses the existing Sanitizers.cmake path (PULP_SANITIZER=undefined).
#
# Usage:
#   scripts/run_ubsan.sh [--jobs N] [--tests REGEX]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-ubsan"
JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TESTS_REGEX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --tests) TESTS_REGEX="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

echo "=== Configuring UBSan build in ${BUILD_DIR} ==="
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPULP_SANITIZER=undefined

echo "=== Building ==="
cmake --build "${BUILD_DIR}" -j"${JOBS}"

# halt_on_error=1 + print_stacktrace=1 → fail on first UB, with trace.
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:symbolize=1"

echo "=== Running tests under UBSan ==="
cd "${BUILD_DIR}"
if [[ -n "${TESTS_REGEX}" ]]; then
    ctest -R "${TESTS_REGEX}" --output-on-failure
else
    ctest --output-on-failure
fi
