#!/usr/bin/env bash
# run_coverage.sh — configure + build + test + HTML coverage report.
#
# Usage:
#   scripts/run_coverage.sh [--jobs N] [--tests REGEX]
#
# Produces:
#   build-coverage/coverage/index.html         — per-file drilldown
#   build-coverage/coverage/summary.txt        — top-level table
#
# Coverage is informational only. This script never fails on a
# coverage threshold; thresholds would be noise without a baseline
# and the #290 hardening initiative.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-coverage"
PROFRAW_DIR="${BUILD_DIR}/profraw"
REPORT_DIR="${BUILD_DIR}/coverage"
JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TESTS_REGEX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --tests) TESTS_REGEX="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

# Require Clang — llvm-cov reads Clang-specific .profdata format.
if ! command -v clang >/dev/null 2>&1; then
    echo "run_coverage.sh: clang not found on PATH" >&2
    exit 1
fi
if ! command -v llvm-profdata >/dev/null 2>&1; then
    echo "run_coverage.sh: llvm-profdata not found on PATH" >&2
    exit 1
fi
if ! command -v llvm-cov >/dev/null 2>&1; then
    echo "run_coverage.sh: llvm-cov not found on PATH" >&2
    exit 1
fi

echo "=== Configuring coverage build in ${BUILD_DIR} ==="
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPULP_ENABLE_COVERAGE=ON \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++

echo "=== Building ==="
cmake --build "${BUILD_DIR}" -j"${JOBS}"

echo "=== Running tests with LLVM_PROFILE_FILE ==="
mkdir -p "${PROFRAW_DIR}"
cd "${BUILD_DIR}"
export LLVM_PROFILE_FILE="${PROFRAW_DIR}/pulp-%p-%m.profraw"
if [[ -n "${TESTS_REGEX}" ]]; then
    ctest -R "${TESTS_REGEX}" --output-on-failure || true
else
    ctest --output-on-failure || true
fi

echo "=== Merging profiles ==="
# Avoid shell-glob on thousands of profraw files that may exhaust argv
# on some OSes; feed via find -print0 | xargs.
mkdir -p "${REPORT_DIR}"
PROFDATA="${REPORT_DIR}/pulp.profdata"
find "${PROFRAW_DIR}" -name '*.profraw' -print0 \
    | xargs -0 llvm-profdata merge -sparse -o "${PROFDATA}"

# Gather every test binary for llvm-cov's -object multi-arg form.
BINARIES=()
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}/test" -maxdepth 2 -type f -perm -u+x \
         ! -name '*.cmake' ! -name '*.txt' 2>/dev/null || true
)

if [[ ${#BINARIES[@]} -eq 0 ]]; then
    echo "run_coverage.sh: no test binaries found under ${BUILD_DIR}/test" >&2
    exit 1
fi

echo "=== llvm-cov report (top-level summary) ==="
llvm-cov report \
    "${BINARIES[@]}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex='_deps/|/external/|/test/' \
    | tee "${REPORT_DIR}/summary.txt"

echo "=== llvm-cov show (HTML drilldown) ==="
llvm-cov show \
    "${BINARIES[@]}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex='_deps/|/external/|/test/' \
    -format=html \
    -output-dir="${REPORT_DIR}"

echo ""
echo "HTML report: ${REPORT_DIR}/index.html"
echo "Summary:     ${REPORT_DIR}/summary.txt"
