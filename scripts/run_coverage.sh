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
# coverage threshold; threshold enforcement belongs to the diff-cover
# gate.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-coverage"
PROFRAW_DIR="${BUILD_DIR}/profraw"
REPORT_DIR="${BUILD_DIR}/coverage"
JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
TESTS_REGEX=""
EXTRA_CMAKE_ARGS=()
EXTRA_CTEST_ARGS=()

# Canonical source-filter regex used by llvm-cov and by the
# LCOV→Cobertura converter. Matches paths we explicitly DO NOT want in
# the coverage report:
#   _deps/               — FetchContent build trees (incl. Catch2)
#   external/            — vendored dependencies
#   test/                — test code itself (we measure coverage OF code under test)
#   catch2/Catch2/       — Catch2 headers/src anywhere; papers over the
#                          `build-coverage/_deps/catch2-build/src/src/` mapping
#                          that produces "No such file or directory" stderr spam
#                          on every llvm-cov show invocation (issue #569).
#   build-coverage/      — this build tree itself.
#   build/               — any other build tree.
#   examples/            — example projects, tracked separately if at all.
#   fetchcontent-src/    — the FetchContent source cache under
#                          Library/Caches/Pulp/; CMake places downloaded
#                          deps there and llvm-cov reports them with
#                          absolute paths that don't hit the _deps/
#                          pattern above; the direct llvm-cov export
#                          pipeline otherwise surfaces tens of thousands
#                          of FetchContent lines as first-party coverage.
#
# Each alternative uses `(^|/)` as the leading anchor so relative and
# absolute paths are both matched. Keep in sync with codecov.yml's
# `ignore:` list.
COVERAGE_IGNORE_REGEX='(^|/)(_deps|external|test|[Cc]atch2|build|build-coverage|examples|fetchcontent-src|sandbox-e2e)/'

while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) JOBS="$2"; shift 2 ;;
        --tests) TESTS_REGEX="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 2 ;;
    esac
done

if [[ -n "${PULP_COVERAGE_CMAKE_ARGS:-}" ]]; then
    # Coverage-only toggles (for example -DPULP_BUILD_PYTHON=ON on the
    # Linux lane) belong in the workflow, not hard-coded here. Split on
    # shell words so simple space-delimited CMake args pass through.
    read -r -a EXTRA_CMAKE_ARGS <<< "${PULP_COVERAGE_CMAKE_ARGS}"
fi
if [[ -n "${PULP_COVERAGE_CTEST_ARGS:-}" ]]; then
    # Optional coverage-only CTest arguments. Keep policy in the workflow and
    # let this script provide the shared execution/reporting mechanics.
    # NOTE: this is whitespace-tokenized only (no shell quoting) — fine for
    # simple flags like `-LE validation`, but a value needing an embedded space
    # (e.g. -LE 'validation|slow gpu') will split wrong. If that's ever needed,
    # switch this to a NUL/newline-delimited or JSON transport.
    read -r -a EXTRA_CTEST_ARGS <<< "${PULP_COVERAGE_CTEST_ARGS}"
fi

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

cmake_args=(
    -S "${REPO_ROOT}"
    -B "${BUILD_DIR}"
    -DCMAKE_BUILD_TYPE=Debug
    -DPULP_ENABLE_COVERAGE=ON
    -DCMAKE_C_COMPILER="${CLANG_C}"
    -DCMAKE_CXX_COMPILER="${CLANG_CXX}"
)
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
    cmake_args+=("${EXTRA_CMAKE_ARGS[@]}")
fi
cmake "${cmake_args[@]}"

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
find "${PROFRAW_DIR}" -name '*.profraw' -type f -delete
cd "${BUILD_DIR}"
# Use one merge-enabled profile per instrumented binary. The full suite runs
# thousands of one-test Catch2 processes; per-PID profiles are noisy, can hit
# shell argv limits during cleanup, and are vulnerable to PID reuse over a long
# coverage run.
export LLVM_PROFILE_FILE="${PROFRAW_DIR}/pulp-%m.profraw"

# Regression guard for #317: track the test-suite outcome without aborting the
# coverage report. A broken test run should still upload its partial coverage
# (so reviewers can see what did exercise), but the script must exit non-zero so
# CI flags the failure; silently swallowing test failures hid real regressions.
CTEST_RC=0
# Run the suite in parallel with a per-test wall-clock cap. Serial ctest over
# the full ~13.5k-case suite is the dominant cost of a coverage run and pushed
# even the macOS leg past the workflow's internal budget (a killed run drops the
# Cobertura report and reddens main). `-j` matches the parallelism the primary
# build.yml/build-macos.yml lanes already use; source-based instrumentation is
# parallel-safe because each test process writes its own merge-enabled profile
# via LLVM_PROFILE_FILE's %m pattern. Cap at 8 like the primary lanes so a
# high-core self-hosted runner doesn't oversubscribe memory with instrumented
# Debug test processes. `--timeout` bounds a single wedged test so it can't eat
# the whole budget; it is generous (instrumented Debug tests run slower than the
# Release lanes' 120s), and a genuinely hung test failing here still emits a
# partial report rather than getting SIGKILLed with the report dropped.
CTEST_JOBS="${JOBS}"
if [[ "${CTEST_JOBS}" -gt 8 ]]; then CTEST_JOBS=8; fi
CTEST_PER_TEST_TIMEOUT="${PULP_COVERAGE_CTEST_TIMEOUT:-600}"
if [[ -n "${TESTS_REGEX}" ]]; then
    ctest -R "${TESTS_REGEX}" "${EXTRA_CTEST_ARGS[@]}" --output-on-failure --repeat until-pass:2 -j"${CTEST_JOBS}" --timeout "${CTEST_PER_TEST_TIMEOUT}" || CTEST_RC=$?
else
    ctest "${EXTRA_CTEST_ARGS[@]}" --output-on-failure --repeat until-pass:2 -j"${CTEST_JOBS}" --timeout "${CTEST_PER_TEST_TIMEOUT}" || CTEST_RC=$?
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

# Gather binaries for llvm-cov's -object multi-arg form.
#
# #566 follow-up: historically we only passed test executables, which meant
# llvm-cov only saw translation units LINKED INTO a test binary. Production
# code in first-party libraries that no test transitively depends on was
# invisible to llvm-cov -> gcovr -> Codecov, tracking only ~0.6% of the
# codebase and silently false-negative-ing the diff-cover gate on any PR
# that touched code outside the test-linked set.
#
# Expose the full first-party surface by also passing every first-party static
# archive plus any non-test executables (tools/cli/pulp, standalone hosts,
# inspect tools). LLVM docs confirm llvm-cov -object accepts .a archives
# directly:
#   https://llvm.org/docs/CommandGuide/llvm-cov.html
#
# External SDK archives (libausdk.a, libvst3-sdk.a) are excluded because
# they're built from third-party sources not instrumented by our flags.
BINARIES=()
TEST_BINARIES=()

# Test executables — these drive the actual coverage hits. Keep them before
# archive-only coverage maps so files linked into both a static archive and a
# test binary keep the executed test binary's counters. Putting archives first
# can report a zero-hit archive map for code that CTest did exercise.
# On Windows/MSYS,
# CTest can run `.exe` files whose Unix executable bit is not visible to
# `find -perm -u+x`; include `.exe` explicitly so their coverage maps reach
# llvm-cov.
while IFS= read -r f; do
    TEST_BINARIES+=("$f")
    BINARIES+=("-object" "$f")
done < <(
    find "${BUILD_DIR}/test" -maxdepth 2 -type f \
         \( -perm -u+x -o -name '*.exe' \) \
         ! -name '*.cmake' ! -name '*.txt' 2>/dev/null || true
)

# Coverage helper tests that live outside test/ still need their own
# binaries in the llvm-cov object set. The embedded Python bindings
# smoke target is built under bindings/python/, not build/test/, so
# without this pass its profile data never contributes to report/show.
while IFS= read -r f; do
    TEST_BINARIES+=("$f")
    BINARIES+=("-object" "$f")
done < <(
    find "${BUILD_DIR}" -type f \
         \( -perm -u+x -o -name '*.exe' \) \
         -name 'pulp-test-*' \
         ! -path "${BUILD_DIR}/test/*" \
         ! -path '*/_deps/*' \
         ! -path '*/external/*' \
         ! -name '*.cmake' ! -name '*.txt' ! -name '*.o' \
         2>/dev/null || true
)

# First-party static libraries — expose every instrumented TU regardless
# of whether a test links it. Pattern is `libpulp-*.a` on macOS/Linux
# and `pulp-*.lib` on Windows (clang-cl's MSVC-style driver emits `.lib`
# archives, not `.a`). Without the `.lib` branch the Windows coverage
# matrix leg silently skips the full-surface expansion, so the Windows
# Codecov upload has only test-linked TUs — same narrow view this fix
# was meant to close everywhere. Deliberately does not pick up
# libausdk.a / libvst3-sdk.a (external SDKs, not our code); the
# `pulp-*.lib` prefix is narrow enough to skip third-party .lib files
# that also appear under the build tree.
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}" -type f \
         \( -name 'libpulp-*.a' -o -name 'pulp-*.lib' \) \
         ! -path "${BUILD_DIR}/test/*" \
         2>/dev/null || true
)

if [[ ${#TEST_BINARIES[@]} -eq 0 ]]; then
    echo "run_coverage.sh: no test binaries found under ${BUILD_DIR}/test" >&2
    exit 1
fi

# First-party non-test executables — CLI, standalone host, inspector.
# Test binaries under build/test/ are already included above; exclude
# anything under /_deps/ or /external/ to avoid third-party objects.
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}/tools" "${BUILD_DIR}/inspect" \
         -maxdepth 3 -type f \
         \( -perm -u+x -o -name '*.exe' \) \
         ! -name '*.cmake' ! -name '*.txt' ! -name '*.o' \
         2>/dev/null || true
)

# Loadable first-party modules that execute instrumented code under test.
# Prefer the embedded Python bindings executable above for bindings.cpp:
# passing both the executable and the pybind11 module gives llvm-cov two
# coverage maps for the same PyInit_pulp function and can collapse the
# file to 0% with "mismatched data" warnings.
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}/bindings" -type f \
         \( -name 'pulp*.so' -o -name 'pulp*.pyd' -o -name 'pulp*.dylib' \) \
         ! -path "${BUILD_DIR}/bindings/python/*" \
         2>/dev/null || true
)

# ── Pre-flight: drop objects llvm-cov can't load ───────────────────────────
# `llvm-cov report` refuses the entire run if any single -object produces
# "malformed coverage data" (e.g. a Linux static archive with a bogus
# coverage-mapping header — seen on CI with `libpulp-ship.a`). Probe each
# binary individually with a minimal `llvm-cov report` against the merged
# profdata; drop any that fail, log a warning. One bad archive should not
# blackhole the whole pipeline.
echo "=== Pre-flight: probing ${#BINARIES[@]} -object entries ==="
PROBED=()
SKIPPED=()
prev=""
for tok in "${BINARIES[@]}"; do
    if [[ "${prev}" == "-object" ]]; then
        if llvm-cov report -object="${tok}" -instr-profile="${PROFDATA}" \
                >/dev/null 2>&1; then
            PROBED+=("-object" "${tok}")
        else
            SKIPPED+=("${tok}")
            echo "  ✗ skipping (malformed coverage data): ${tok}" >&2
        fi
    fi
    prev="${tok}"
done
BINARIES=("${PROBED[@]}")
if [[ ${#SKIPPED[@]} -gt 0 ]]; then
    echo "=== Pre-flight: dropped ${#SKIPPED[@]} bad object(s); using ${#PROBED[@]} entries ==="
fi
if [[ ${#BINARIES[@]} -eq 0 ]]; then
    echo "run_coverage.sh: pre-flight left zero loadable -object entries" >&2
    exit 1
fi

# A full build probes 1000+ `-object` entries. Passing them inline overflows
# the Windows command-line length limit (CreateProcess ~32 KB) — llvm-cov dies
# with "Argument list too long", filters out every source file, and no
# Cobertura XML is produced (the os-windows coverage leg's failure mode). Feed
# the object list through an LLVM response file (`@file`, expanded by every LLVM
# tool's CommandLine parser) so the OS arg-length limit never applies — on any
# platform. One token per line; the probed paths never contain spaces.
OBJ_RSP="${REPORT_DIR}/llvm-cov-objects.rsp"
# Final existence filter while materializing the response file. An -object can
# vanish between the pre-flight probe above and this point — the observed
# Windows case is a late-cleaned test-fixture executable (e.g.
# `test/pulp-cli-run-fixture.exe`) that is still present when the probe loads it
# but gone by report time. llvm-cov hard-fails the ENTIRE run on a single
# missing -object ("failed to load coverage: '…': no such file or directory"),
# so one disappearing fixture would blackhole the whole os-windows leg. Drop any
# that are no longer present instead.
: > "${OBJ_RSP}"
KEPT_OBJECTS=0
VANISHED_OBJECTS=0
prev=""
for tok in "${BINARIES[@]}"; do
    if [[ "${prev}" == "-object" ]]; then
        if [[ -f "${tok}" ]]; then
            # Quote the path: LLVM response files are whitespace-tokenized but
            # honor double quotes, so a runner workdir containing a space
            # (e.g. /Volumes/CI Builds/...) stays a single -object argument
            # instead of splitting into bogus tokens. Harmless for the common
            # space-free GitHub/self-hosted paths.
            printf -- '-object\n"%s"\n' "${tok}" >> "${OBJ_RSP}"
            KEPT_OBJECTS=$((KEPT_OBJECTS + 1))
        else
            VANISHED_OBJECTS=$((VANISHED_OBJECTS + 1))
            echo "  ✗ dropping object that vanished after pre-flight: ${tok}" >&2
        fi
    fi
    prev="${tok}"
done
if [[ "${VANISHED_OBJECTS}" -gt 0 ]]; then
    echo "=== Dropped ${VANISHED_OBJECTS} object(s) that vanished after the pre-flight probe ==="
fi
if [[ "${KEPT_OBJECTS}" -eq 0 ]]; then
    echo "run_coverage.sh: existence filter left zero -object entries" >&2
    exit 1
fi
# Mass-drop guard. A healthy run drops ~0-1 objects (a late-cleaned Windows
# fixture). Losing a large fraction means object discovery or the build tree is
# broken — and llvm-cov would otherwise happily export near-empty coverage from
# the survivors and upload it as if it were valid (silently tanking the number).
# Refuse instead. On the best-effort os-windows leg this just fails the leg
# non-fatally; on Linux/macOS it correctly red-flags a broken run.
TOTAL_OBJECTS=$((KEPT_OBJECTS + VANISHED_OBJECTS))
if [[ "${VANISHED_OBJECTS}" -gt 25 \
      && $((VANISHED_OBJECTS * 100)) -gt $((TOTAL_OBJECTS * 5)) ]]; then
    echo "run_coverage.sh: ${VANISHED_OBJECTS}/${TOTAL_OBJECTS} -object entries vanished after the pre-flight probe (>5%) — refusing to upload coverage from the survivors; object discovery or the build tree is likely broken." >&2
    exit 1
fi
echo "=== Wrote ${KEPT_OBJECTS} -object tokens to a response file (avoids Windows ARG_MAX) ==="

echo "=== llvm-cov report (top-level summary) ==="
llvm-cov report \
    "@${OBJ_RSP}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex="${COVERAGE_IGNORE_REGEX}" \
    | tee "${REPORT_DIR}/summary.txt"

echo "=== llvm-cov show (HTML drilldown) ==="
llvm-cov show \
    "@${OBJ_RSP}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex="${COVERAGE_IGNORE_REGEX}" \
    -format=html \
    -output-dir="${REPORT_DIR}"

# Emit Cobertura XML for Codecov + diff-cover via
#   llvm-cov export --format=lcov  →  lcov_cobertura.py  →  Cobertura XML
#
# Why not gcovr? We tried gcovr 8.6 in LLVM mode (--llvm-cov-binary
# per test binary). When we expanded the `-object` set from just the
# test executables to also include every `libpulp-*.a` archive and
# non-test executable, `llvm-cov report` correctly reported ~110k
# tracked lines across 577 source files, but gcovr emitted a Cobertura
# XML with 150 lines across 4 files — silently dropping ~99.9% of the
# coverage data. The direct `llvm-cov export --format=lcov` pipeline
# produces complete output (same 577 files, 106k lines after
# fetchcontent-src filtering) and the lcov_cobertura.py converter is a
# single-file script we cache alongside this build script.
#
# `llvm-cov -object` accepts static `.a` archives directly. LLVM docs:
#   https://llvm.org/docs/CommandGuide/llvm-cov.html
#
# The converter script lives at tools/scripts/lcov_cobertura.py and is
# a vendored copy of https://github.com/eriwen/lcov-to-cobertura-xml
# (Apache 2.0, MIT-compatible).
COBERTURA_XML="${BUILD_DIR}/coverage.cobertura.xml"
RAW_LCOV_FILE="${REPORT_DIR}/coverage.raw.lcov"
LCOV_FILE="${REPORT_DIR}/coverage.lcov"
LCOV_COBERTURA="${REPO_ROOT}/tools/scripts/lcov_cobertura.py"

if [[ ! -f "${LCOV_COBERTURA}" ]]; then
    echo "run_coverage.sh: missing ${LCOV_COBERTURA}" >&2
    echo "    (expected the vendored lcov→cobertura converter)" >&2
    exit 1
fi

echo "=== llvm-cov export → LCOV ==="
llvm-cov export --format=lcov \
    "@${OBJ_RSP}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex="${COVERAGE_IGNORE_REGEX}" \
    > "${RAW_LCOV_FILE}"

# Issue #1058: `llvm-cov export --format=lcov` is not gcov-aware and does NOT
# honor `LCOV_EXCL_START` / `LCOV_EXCL_STOP` markers in source. Without the
# `lcov --remove` pass below, those markers are silently documentation-only
# and excluded ranges still appear as Missing in diff-cover output. Run the
# raw .lcov through `lcov --remove` so its gcov parser strips the excluded
# ranges before lcov_cobertura.py converts to Cobertura XML. Falls back to
# a straight copy + warning when `lcov` isn't installed (CI / Linux dev
# machines often lack it) so the existing pipeline keeps working with the
# prior behavior.
echo "=== LCOV → LCOV (honor LCOV_EXCL markers via lcov --filter region) ==="
if command -v lcov >/dev/null 2>&1; then
    # The dummy `--remove` pattern matches no real source path, so the
    # remove step is a no-op file-wise; what we actually want is the
    # source-aware re-read triggered by `--filter region`, which scans
    # the SF: source files for LCOV_EXCL_START/STOP and drops the
    # excluded line ranges. `--ignore-errors unused` keeps the dummy
    # pattern from being a fatal error in lcov 2.x. If the lcov binary
    # is too old to recognize `--filter region` (lcov 1.x), fall back
    # to a straight copy and print a hint.
    if ! lcov --remove "${RAW_LCOV_FILE}" '/__pulp_unmatched__/*' \
              --output-file "${LCOV_FILE}" \
              --filter region \
              --ignore-errors unused \
              --rc branch_coverage=1 \
              >/dev/null 2>&1; then
        cp "${RAW_LCOV_FILE}" "${LCOV_FILE}"
        echo "run_coverage.sh: WARN: lcov --filter region failed; falling back to raw .lcov" >&2
        echo "run_coverage.sh:       (LCOV_EXCL markers will not be honored — needs lcov >= 2.0)" >&2
    fi
else
    cp "${RAW_LCOV_FILE}" "${LCOV_FILE}"
    echo "run_coverage.sh: note: lcov not installed — LCOV_EXCL markers won't be honored." >&2
    echo "run_coverage.sh: install with: brew install lcov  /  sudo apt install lcov" >&2
fi

echo "=== LCOV → Cobertura XML ==="
python3 "${LCOV_COBERTURA}" "${LCOV_FILE}" \
    --output "${COBERTURA_XML}" \
    --base-dir "${REPO_ROOT}"
echo "Cobertura:  ${COBERTURA_XML}"

echo ""
echo "HTML report: ${REPORT_DIR}/index.html"
echo "Summary:     ${REPORT_DIR}/summary.txt"

# Propagate test-suite failure to callers/CI.
if [[ "${CTEST_RC}" -ne 0 ]]; then
    echo ""
    echo "=== FAIL: ctest exited non-zero (${CTEST_RC}). Coverage report above is based on partial profile data from tests that did run. ==="
    exit "${CTEST_RC}"
fi
