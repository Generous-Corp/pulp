#!/usr/bin/env bash
# run_tsan.sh — ThreadSanitizer build + test run.
#
# Uses the pre-existing Sanitizers.cmake (PULP_SANITIZER=thread) path.
# TSan can be noisy with lock-free code; TSAN_OPTIONS below suppresses
# known-safe patterns. Expect to iterate on suppressions as the test
# surface grows.
#
# Usage:
#   scripts/run_tsan.sh [--jobs N] [--tests REGEX]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-tsan"
JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TESTS_REGEX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --tests) TESTS_REGEX="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

echo "=== Configuring TSan build in ${BUILD_DIR} ==="
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPULP_SANITIZER=thread

echo "=== Building ==="
cmake --build "${BUILD_DIR}" -j"${JOBS}"

# halt_on_error=1 → fail on first real race.
# history_size=7 → deeper history for diagnosis (default 4).
# Optional suppressions file for documented lock-free patterns.
SUPPRESSIONS="${REPO_ROOT}/test/tsan.supp"
TSAN_OPT="halt_on_error=1:history_size=7"
if [[ -f "${SUPPRESSIONS}" ]]; then
    TSAN_OPT="${TSAN_OPT}:suppressions=${SUPPRESSIONS}"
fi
export TSAN_OPTIONS="${TSAN_OPT}"

echo "=== Running tests under TSan ==="
cd "${BUILD_DIR}"
if [[ -n "${TESTS_REGEX}" ]]; then
    ctest -R "${TESTS_REGEX}" --output-on-failure
else
    ctest --output-on-failure
fi
