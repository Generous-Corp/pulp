#!/usr/bin/env bash
# run_coverage.sh — configure + build + test + HTML coverage report.
#
# Usage:
#   scripts/run_coverage.sh [--jobs N] [--tests REGEX]
#
# Produces:
#   build-coverage/coverage/index.html          — per-file drilldown
#   build-coverage/coverage/summary.txt         — top-level table
#   build-coverage/coverage.cobertura.xml       — Cobertura XML (for
#                                                 Codecov + diff-cover);
#                                                 skipped if gcovr not on
#                                                 PATH (local-only).
#
# Coverage is informational only. This script never fails on a
# coverage threshold; thresholds come via the Phase 2/3 diff-cover
# gate (see planning/coverage-tooling-decision-2026-04-21.md).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-coverage"
PROFRAW_DIR="${BUILD_DIR}/profraw"
REPORT_DIR="${BUILD_DIR}/coverage"
JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TESTS_REGEX=""

# Canonical source-filter regex used by both llvm-cov and gcovr.
# Matches paths we explicitly DO NOT want in the coverage report:
#   _deps/          — FetchContent build trees (incl. Catch2)
#   external/       — vendored dependencies
#   test/           — test code itself (we measure coverage OF code under test)
#   catch2/Catch2/  — Catch2 headers/src anywhere; papers over the
#                     `build-coverage/_deps/catch2-build/src/src/` mapping
#                     that produces "No such file or directory" stderr spam
#                     on every llvm-cov show invocation (issue #569).
#   build-coverage/ — this build tree itself.
#   build/          — any other build tree.
#
# Each alternative uses `(^|/)` as the leading anchor so relative and
# absolute paths are both matched. Keep in sync with `gcovr --exclude`
# flags below and with planning/coverage-tooling-decision-2026-04-21.md §4.
COVERAGE_IGNORE_REGEX='(^|/)(_deps|external|test|[Cc]atch2|build|build-coverage)/'

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

# On Windows we need `clang-cl`, not plain `clang`. Some bundled deps
# (mbedtls, notably) pass MSVC-style flags like /W3 and /utf-8 when
# they detect a Windows host — plain `clang.exe` in GCC-driver mode
# rejects those as "no such file or directory", breaking the build.
# `clang-cl` is Clang's MSVC-compatible driver: it accepts MSVC flags
# AND the GCC-style `-fprofile-instr-generate -fcoverage-mapping` that
# PulpInstrumentation.cmake emits for source-based coverage.
if [ "${OS:-}" = "Windows_NT" ] || [ -n "${MSYSTEM:-}" ]; then
    CLANG_C=clang-cl
    CLANG_CXX=clang-cl
else
    CLANG_C=clang
    CLANG_CXX=clang++
fi

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPULP_ENABLE_COVERAGE=ON \
    -DCMAKE_C_COMPILER="${CLANG_C}" \
    -DCMAKE_CXX_COMPILER="${CLANG_CXX}"

# Issue #570: if build-coverage/CMakeCache.txt was previously populated
# with PULP_ENABLE_COVERAGE:BOOL=OFF (e.g. from a non-coverage run
# that reused the same directory), the cached value wins on
# reconfigure even though we pass -DPULP_ENABLE_COVERAGE=ON. The
# instrumentation flags never reach flags.make, no profraw files are
# emitted, and llvm-profdata merge later dies with "could not read
# profile data!" — a symptom that looks like a test/instrumentation
# bug, not a stale-cache bug. Assert the expected value stuck and
# error loudly with the fix if not.
CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
if ! grep -q '^PULP_ENABLE_COVERAGE:BOOL=ON$' "${CACHE_FILE}" 2>/dev/null; then
    echo "" >&2
    echo "ERROR: PULP_ENABLE_COVERAGE=ON did not stick in the CMake cache." >&2
    echo "  ${CACHE_FILE}" >&2
    echo "  still holds the previous (non-coverage) value, so no" >&2
    echo "  instrumentation flags will reach the compiler and no" >&2
    echo "  profraw files will be emitted. This is issue #570." >&2
    echo "" >&2
    echo "  Fix: remove the stale cache and re-run this script:" >&2
    echo "      rm -rf ${BUILD_DIR}" >&2
    echo "      ${BASH_SOURCE[0]}" >&2
    echo "" >&2
    exit 3
fi

echo "=== Building ==="
cmake --build "${BUILD_DIR}" -j"${JOBS}"

echo "=== Running tests with LLVM_PROFILE_FILE ==="
mkdir -p "${PROFRAW_DIR}"
cd "${BUILD_DIR}"
export LLVM_PROFILE_FILE="${PROFRAW_DIR}/pulp-%p-%m.profraw"

# #317 Codex P2: track test-suite outcome without aborting the
# coverage report. A broken test run should still upload its
# partial coverage (so reviewers can see what DID exercise), but
# the script MUST exit non-zero so CI flags the failure —
# silently swallowing test failures hid real regressions.
CTEST_RC=0
if [[ -n "${TESTS_REGEX}" ]]; then
    ctest -R "${TESTS_REGEX}" --output-on-failure || CTEST_RC=$?
else
    ctest --output-on-failure || CTEST_RC=$?
fi
if [[ "${CTEST_RC}" -ne 0 ]]; then
    echo "=== ctest failed with exit ${CTEST_RC} — coverage report WILL be generated from partial profile data, then the script will exit with that code. ==="
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
    -ignore-filename-regex="${COVERAGE_IGNORE_REGEX}" \
    | tee "${REPORT_DIR}/summary.txt"

echo "=== llvm-cov show (HTML drilldown) ==="
llvm-cov show \
    "${BINARIES[@]}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex="${COVERAGE_IGNORE_REGEX}" \
    -format=html \
    -output-dir="${REPORT_DIR}"

# Issue #569 Phase 1 PR 1: also emit Cobertura XML alongside the HTML
# report. Codecov + diff-cover both consume Cobertura; llvm-cov does
# NOT produce Cobertura natively, so gcovr bridges llvm-cov JSON →
# Cobertura XML. If gcovr isn't installed (local developer machine
# without the optional dep), skip with a hint rather than fail — the
# CI workflow explicitly installs gcovr, so CI coverage always has XML.
COBERTURA_XML="${BUILD_DIR}/coverage.cobertura.xml"
if command -v gcovr >/dev/null 2>&1; then
    echo "=== gcovr (Cobertura XML for Codecov + diff-cover) ==="
    # gcovr consumes the raw profdata + instrumented binaries and
    # produces Cobertura XML. Pass the same filter set as llvm-cov
    # (filters are anchored at repo root; excludes are regexes).
    GCOVR_LLVM_BINS=()
    for obj in "${BINARIES[@]}"; do
        if [[ "${obj}" != "-object" ]]; then
            GCOVR_LLVM_BINS+=("--llvm-cov-binary" "${obj}")
        fi
    done
    # Pulp #605: positive `--filter core/` and `--filter tools/cli/` were
    # silently filtering out ALL coverage data on CI (gcovr's unanchored-regex
    # semantics vs the absolute paths llvm-cov reports didn't line up),
    # producing a well-formed but empty Cobertura XML that Codecov then
    # rejected as "Unusable report." Dropping the positive filters — we rely
    # on the `--exclude` set below, which mirrors codecov.yml's `ignore:`
    # list. Anything not excluded is coverage-bearing by definition.
    gcovr \
        --root "${REPO_ROOT}" \
        --llvm-profdata-executable llvm-profdata \
        "${GCOVR_LLVM_BINS[@]}" \
        --exclude '.*/external/' \
        --exclude '.*/test/' \
        --exclude '.*/_deps/' \
        --exclude '.*/[Cc]atch2/' \
        --exclude '.*/build-coverage/' \
        --exclude '.*/build/' \
        --exclude '.*/examples/' \
        --cobertura "${COBERTURA_XML}" \
        "${PROFRAW_DIR}" \
        || echo "gcovr exited non-zero — Cobertura XML may be partial (non-fatal)."
    echo "Cobertura:  ${COBERTURA_XML}"
else
    echo "=== gcovr not found on PATH — skipping Cobertura XML ==="
    echo "    (Codecov + diff-cover need this in CI; install: pip install gcovr)"
fi

echo ""
echo "HTML report: ${REPORT_DIR}/index.html"
echo "Summary:     ${REPORT_DIR}/summary.txt"

# Propagate test-suite failure to callers/CI.
if [[ "${CTEST_RC}" -ne 0 ]]; then
    echo ""
    echo "=== FAIL: ctest exited non-zero (${CTEST_RC}). Coverage report above is based on partial profile data from tests that did run. ==="
    exit "${CTEST_RC}"
fi
