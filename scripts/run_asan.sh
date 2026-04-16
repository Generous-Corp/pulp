#!/usr/bin/env bash
# run_asan.sh — ASan + UBSan build + test run.
#
# Uses the pre-existing Sanitizers.cmake (PULP_SANITIZER=address) path.
# Exits non-zero if any sanitizer report fires during the test suite.
#
# Usage:
#   scripts/run_asan.sh [--jobs N] [--tests REGEX]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-asan"
JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TESTS_REGEX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --tests) TESTS_REGEX="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

echo "=== Configuring ASan build in ${BUILD_DIR} ==="
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPULP_SANITIZER=address

echo "=== Building ==="
cmake --build "${BUILD_DIR}" -j"${JOBS}"

# Disable ASLR-sensitive features that conflict with ASan on macOS.
# halt_on_error=1 turns first sanitizer report into non-zero exit.
# abort_on_error=0 so we get a full stack trace, not a SIGABRT dump.
export ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:symbolize=1:detect_leaks=0"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"

echo "=== Running tests under ASan + UBSan ==="
cd "${BUILD_DIR}"
if [[ -n "${TESTS_REGEX}" ]]; then
    ctest -R "${TESTS_REGEX}" --output-on-failure
else
    ctest --output-on-failure
fi
