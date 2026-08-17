#!/usr/bin/env bash
# local_diff_cover.sh — local mirror of the CI diff-cover gate.
#
# Catches the same `Diff coverage required` failure CI catches, but
# locally before push. Saves the ~20-min CI roundtrip on coverage-only
# failures.
#
# Single source of truth for threshold + filters lives in
# tools/scripts/coverage_config.json. Both this script and
# .github/workflows/coverage.yml read from there, so editing the JSON
# in one place keeps CI + local + the pre-push hook in lockstep.
#
# Required deps (lazy-prompts if missing):
#   pip install --user 'diff-cover>=9' gcovr jq-not-needed-on-macos
#   (jq is only needed if your shell can't parse JSON via python3,
#    which it always can — see read_config_value below.)
#
# Usage:
#   tools/scripts/local_diff_cover.sh                      # whole tree (slow, matches CI)
#   tools/scripts/local_diff_cover.sh pulp-test-state      # targeted (fast)
#   PULP_DIFF_COVER_CTEST_REGEX='State|WidgetBridge' tools/scripts/local_diff_cover.sh pulp-test-state
#   PULP_SKIP_DIFF_COVER=1 tools/scripts/local_diff_cover.sh   # bypass
#   PULP_DIFF_COVER_HTML_REPORT=/path/report.html tools/scripts/local_diff_cover.sh
#   PULP_DIFF_COVER_MIN_FREE_GIB=5 tools/scripts/local_diff_cover.sh  # 0 disables
#
# Exit codes:
#   0 — diff coverage at or above threshold (or skipped)
#   1 — diff coverage below threshold, or a hard error during the run
#   2 — missing required dependency (clear remediation message)
#   3 — not enough free disk space to run the coverage build (nothing built)
#
# Design (mirrors CI):
#   1. Read threshold + filters from coverage_config.json.
#   2. If PULP_SKIP_DIFF_COVER=1 → exit 0 with a clear message.
#   2b. Refuse to start when the coverage build volume is nearly full, before
#      any configure or compile — a full disk otherwise surfaces minutes later
#      as a coverage gate failure that never mentions disk.
#   3. Configure build-cov/ separately from build/ to avoid churning
#      the user's main CMake cache (Coverage requires Clang +
#      PULP_ENABLE_COVERAGE=ON which conflicts with the default debug
#      build's settings).
#   4. Build either user-supplied targets, or `all` if none given.
#   5. Run the test binaries (ctest) under LLVM_PROFILE_FILE.
#   6. Convert llvm-cov export → Cobertura XML using the same
#      lcov_cobertura.py path scripts/run_coverage.sh uses.
#   7. Run diff-cover with --fail-under from the JSON.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONFIG_JSON="${REPO_ROOT}/tools/scripts/coverage_config.json"
BUILD_DIR="${REPO_ROOT}/build-cov"
BUILD_COV_LOCK="${BUILD_DIR}.lock"

# shellcheck source=../../scripts/coverage_ctest_policy.sh
source "${REPO_ROOT}/scripts/coverage_ctest_policy.sh"

# Diff coverage is a local pre-push mirror of the required PR gate, not the
# full/nightly test lane. Source the same CTest policy as run_coverage.sh so
# local and SSH hooks cannot spend tens of minutes in an unrelated slow
# platform smoke or validator. Full/nightly/main CI invoke CTest separately.
DIFF_COVER_CTEST_LABEL_EXCLUDE="${PULP_DIFF_COVER_CTEST_LABEL_EXCLUDE:-${PULP_COVERAGE_CTEST_LABEL_EXCLUDE}}"
DIFF_COVER_CTEST_NAME_EXCLUDE="${PULP_DIFF_COVER_CTEST_NAME_EXCLUDE:-${PULP_COVERAGE_CTEST_NAME_EXCLUDE}}"
DIFF_COVER_TEST_JOBS="${PULP_DIFF_COVER_TEST_JOBS:-${PULP_COVERAGE_TEST_JOBS:-8}}"
DIFF_COVER_PER_TEST_TIMEOUT="${PULP_DIFF_COVER_CTEST_TIMEOUT:-${PULP_COVERAGE_CTEST_TIMEOUT:-600}}"
if ! [[ "${DIFF_COVER_TEST_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[local_diff_cover] invalid PULP_DIFF_COVER_TEST_JOBS: '${DIFF_COVER_TEST_JOBS}'" >&2
    return 2 2>/dev/null || exit 2
fi
if [[ "${DIFF_COVER_TEST_JOBS}" -gt 8 ]]; then DIFF_COVER_TEST_JOBS=8; fi

run_coverage_ctest() {
    local build_dir="$1"
    local test_regex="${2:-}"
    # Match scripts/run_coverage.sh and the primary CI lanes: enough parallelism
    # to avoid launching nearly 19k discovered Catch2 cases serially, without
    # oversubscribing memory on M1/M3 or SSH/self-hosted builders.
    local args=(
        --test-dir "${build_dir}"
        --output-on-failure
        --label-exclude "${DIFF_COVER_CTEST_LABEL_EXCLUDE}"
        --exclude-regex "${DIFF_COVER_CTEST_NAME_EXCLUDE}"
        --parallel "${DIFF_COVER_TEST_JOBS}"
        --timeout "${DIFF_COVER_PER_TEST_TIMEOUT}"
    )
    if [ -n "${test_regex}" ]; then
        echo "[local_diff_cover] limiting ctest to regex: ${test_regex}" >&2
        args+=(-R "${test_regex}")
    fi
    ctest "${args[@]}"
}

# The report lives beside the coverage data it describes, under this
# worktree's build-cov. $TMPDIR is per-USER, not per-worktree, so a fixed
# filename under it is a shared mailbox: concurrent runs from other worktrees
# (several agent sessions, plus shipyard's local validation, routinely run at
# once) overwrite each other, and a report read as evidence for the wrong
# branch is a correctness failure, not clutter. build-cov is per-worktree,
# single-writer while the lock below is held, and already reclaimed by
# clean_build_cov.sh.
HTML_REPORT="${PULP_DIFF_COVER_HTML_REPORT:-${BUILD_DIR}/coverage/diff-cover.html}"

# Scrub any inherited git environment. When this script runs from a git hook
# (e.g. pre-push) or another git-invoked context, GIT_DIR / GIT_WORK_TREE are
# set — and a set GIT_DIR OVERRIDES `git -C <dir>` discovery, so the test
# binaries ctest runs below (which shell out to `git -C <tempdir> …`) would
# operate on THIS worktree's repo instead of their throwaway temp repos,
# corrupting it (stray "initial" commits, throwaway branches, core.bare flips).
# This script discovers the repo from cwd, so unsetting these is also correct
# for its own `git`/diff-cover calls. Root-caused: a full-suite ctest under the
# pre-push hook mutated a live worktree's .git.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
      GIT_COMMON_DIR GIT_PREFIX GIT_NAMESPACE GIT_QUARANTINE_PATH

# ── build-cov mutual exclusion ─────────────────────────────────────────────
# Everything below shares one build-cov directory: a CMake cache, an object
# tree, and a .profraw set that is deleted and re-accumulated per run. Two runs
# racing in the same worktree — a hand-run racing shipyard's local validation,
# which invokes this script too — interleave all three and report a coverage
# number that measures neither run's change set. That reads as a real coverage
# failure and sends someone debugging code that is fine.
#
# mkdir is the atomic primitive because flock(1) is not on stock macOS. The
# lockdir records its owner's pid so a run killed before its EXIT trap can fire
# (SIGKILL, a reboot) leaves a lock later runs can prove is dead and reclaim,
# rather than wedging every future run.
release_build_cov_lock() {
    rm -rf "${BUILD_COV_LOCK}" >/dev/null 2>&1 || true
}

acquire_build_cov_lock() {
    local announced=0 owner recheck
    mkdir -p "$(dirname "${BUILD_COV_LOCK}")"
    while ! mkdir "${BUILD_COV_LOCK}" 2>/dev/null; do
        owner="$(cat "${BUILD_COV_LOCK}/pid" 2>/dev/null || true)"
        if [ -n "${owner}" ] && ! kill -0 "${owner}" 2>/dev/null; then
            # Reclaim only after re-reading: if another waiter won the lock in
            # between, the pid changes and we must not delete its live lock.
            sleep 1
            recheck="$(cat "${BUILD_COV_LOCK}/pid" 2>/dev/null || true)"
            if [ "${recheck}" = "${owner}" ] && ! kill -0 "${owner}" 2>/dev/null; then
                echo "[local_diff_cover] reclaiming stale lock left by dead pid ${owner}" >&2
                rm -rf "${BUILD_COV_LOCK}"
            fi
            continue
        fi
        if [ "${announced}" -eq 0 ]; then
            echo "[local_diff_cover] waiting for another diff-coverage run to finish${owner:+ (pid ${owner})}…" >&2
            echo "[local_diff_cover] lock: ${BUILD_COV_LOCK}" >&2
            announced=1
        fi
        sleep 2
    done
    # Arm the release before recording the owner: writing the pid can itself
    # fail (a full disk is the usual way, since build-cov accumulates), and
    # under `set -e` that exits — with the trap already armed, it releases the
    # lock instead of stranding a pid-less one no later run could reclaim.
    trap release_build_cov_lock EXIT
    echo "$$" > "${BUILD_COV_LOCK}/pid"
}

# ── Free-disk precondition ─────────────────────────────────────────────────
# A coverage build writes tens of GB into build-cov. When the volume is
# already full, the configure below dies with its output redirected to
# /dev/null and the pre-push hook reports `gate failure(s) above; blocking
# push` — after ~20 minutes, and without the word "disk" anywhere in the
# output. That has cost real sessions ~25 minutes each: the stated reason is
# the coverage gate, so the natural next move is to go debug the diff.
# Measuring free space first turns that into a one-line answer.
diff_cover_disk_stats() {
    # Print "<available-KiB>\t<mount-point>" for the filesystem holding $1.
    # Walks up to the nearest EXISTING ancestor, because build-cov does not
    # exist yet on a first run. `df -Pk` is the POSIX form: a header line then
    # exactly one unwrapped record whose 4th field is available 1024-blocks.
    # The mount point is everything from field 6 on, so a path with a space in
    # it survives.
    local dir="$1"
    while [ -n "${dir}" ] && [ ! -d "${dir}" ] && [ "${dir}" != "/" ]; do
        dir="$(dirname "${dir}")"
    done
    df -Pk "${dir}" 2>/dev/null | awk '
        NR == 2 {
            mount = $6
            for (i = 7; i <= NF; i++) mount = mount " " $i
            printf "%s\t%s\n", $4, mount
        }'
}

require_free_disk() {
    # $1 = directory the coverage build writes into, $2 = required whole GiB.
    # Returns 0 when there is room, when the requirement is 0 (disabled), or
    # when free space cannot be measured — an instrument that cannot read must
    # never block a push. Returns 3 when the volume is too full to proceed.
    local dir="$1" min_gib="$2" stats avail_kib mount min_kib avail_gib
    [ "${min_gib}" -le 0 ] && return 0
    # `|| true` because the script runs under `set -e -o pipefail`: a df that
    # exits non-zero would otherwise kill the whole run right here with a bare
    # exit 1 and no message — the same unexplained failure this check exists to
    # replace. An unreadable instrument degrades to "skip", never to "block".
    stats="$(diff_cover_disk_stats "${dir}" || true)"
    avail_kib="${stats%%$'\t'*}"
    mount="${stats#*$'\t'}"
    if ! [[ "${avail_kib}" =~ ^[0-9]+$ ]]; then
        echo "[local_diff_cover] note: could not measure free disk space for ${dir}; skipping the disk precondition" >&2
        return 0
    fi
    min_kib=$(( min_gib * 1024 * 1024 ))
    [ "${avail_kib}" -ge "${min_kib}" ] && return 0

    avail_gib="$(awk -v kb="${avail_kib}" 'BEGIN{printf "%.1f", kb/1024/1024}')"
    # Loud, in the same register as the pre-push hook's build banner, because
    # this is read in a wall of gate output by someone who is one step away
    # from going and debugging the wrong thing.
    echo "" >&2
    echo "┌──────────────────────────────────────────────────────────────────────┐" >&2
    echo "│ local_diff_cover: NOT ENOUGH FREE DISK SPACE. Nothing was built.     │" >&2
    echo "│ This is a DISK problem, NOT a coverage or code problem.              │" >&2
    echo "└──────────────────────────────────────────────────────────────────────┘" >&2
    echo "[local_diff_cover] free disk space below the diff-coverage precondition:" >&2
    echo "[local_diff_cover]   coverage build dir : ${dir}" >&2
    echo "[local_diff_cover]   volume             : ${mount}" >&2
    echo "[local_diff_cover]   free disk space    : ${avail_gib} GiB" >&2
    echo "[local_diff_cover]   required           : ${min_gib} GiB" >&2
    echo "" >&2
    echo "[local_diff_cover] Reclaim stale coverage build dirs across worktrees:" >&2
    echo "[local_diff_cover]     tools/scripts/clean_build_cov.sh          # dry-run: what it would free" >&2
    echo "[local_diff_cover]     tools/scripts/clean_build_cov.sh --yes    # delete (idle-gated)" >&2
    echo "" >&2
    echo "[local_diff_cover] To lower or disable this precondition for one run:" >&2
    echo "[local_diff_cover]     PULP_DIFF_COVER_MIN_FREE_GIB=5 …   (0 disables the check)" >&2
    echo "[local_diff_cover] The default lives in coverage_config.json (min_free_disk_gib)." >&2
    return 3
}

# Let tests source the helpers above without running a coverage build.
if [ "${PULP_DIFF_COVER_LIB_ONLY:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

# ── PULP_SKIP_DIFF_COVER bypass ─────────────────────────────────────────────
# Honor PULP_SKIP_DIFF_COVER=1 before doing ANY other work (no config
# read, no dep check) so a workflow-only or doc-only PR can bypass even
# from a checkout that's missing diff-cover entirely.
if [ "${PULP_SKIP_DIFF_COVER:-0}" = "1" ]; then
    echo "[local_diff_cover] skipped via PULP_SKIP_DIFF_COVER=1" >&2
    exit 0
fi

# ── Read configuration via python3 (no jq dep needed) ──────────────────────
read_config_value() {
    # Read a top-level scalar key from coverage_config.json.
    # python3 is already a build prerequisite for Pulp, so this avoids
    # adding jq as a hard dep on every developer machine.
    local key="$1"
    python3 -c "
import json, sys
with open('${CONFIG_JSON}') as f:
    cfg = json.load(f)
v = cfg.get('${key}')
if v is None:
    sys.exit(1)
print(v)
"
}

if [ ! -f "${CONFIG_JSON}" ]; then
    echo "[local_diff_cover] missing config: ${CONFIG_JSON}" >&2
    exit 1
fi

THRESHOLD="$(read_config_value diff_coverage_fail_under)"
COMPARE_BRANCH="$(read_config_value compare_branch)"

if ! [[ "${THRESHOLD}" =~ ^[0-9]+$ ]]; then
    echo "[local_diff_cover] invalid diff_coverage_fail_under in ${CONFIG_JSON}: '${THRESHOLD}'" >&2
    exit 1
fi

# Disk precondition, ahead of the dependency preflight, the fetch, the lock and
# every line of build below — the whole point is to answer in milliseconds
# rather than after a 20-minute configure + compile.
MIN_FREE_GIB="${PULP_DIFF_COVER_MIN_FREE_GIB:-$(read_config_value min_free_disk_gib || echo "")}"
if ! [[ "${MIN_FREE_GIB}" =~ ^[0-9]+$ ]]; then
    echo "[local_diff_cover] invalid free-disk requirement '${MIN_FREE_GIB}': want whole GiB, from" >&2
    echo "[local_diff_cover] PULP_DIFF_COVER_MIN_FREE_GIB or min_free_disk_gib in ${CONFIG_JSON}" >&2
    exit 1
fi
require_free_disk "${BUILD_DIR}" "${MIN_FREE_GIB}" || exit 3

# Per-file exclusions from the same source-of-truth (kept in lockstep
# with .github/workflows/coverage.yml). diff-cover's `--exclude` flag
# uses argparse `nargs='+'` and matches via fnmatch against (a)
# basename and (b) absolute path. TWO subtleties matter for callers:
#   1. With repeated `--exclude=foo --exclude=bar`, argparse keeps
#      only the LAST entry (default action; not 'append'). So we
#      must pass ALL exclusions in a SINGLE `--exclude val1 val2 ...`
#      flag.
#   2. A literal relative path like `tools/cli/cmd_loop.cpp` matches
#      NEITHER the basename (no slash to strip) NOR the absolute path
#      (which has the repo prefix). Patterns must be a basename
#      (`cmd_loop.cpp`) or a glob (`**/cmd_loop.cpp`) — that's the
#      contract documented in coverage_config.json's _comment.
DIFF_COVER_EXCLUDE_ARGS=()
if command -v jq >/dev/null 2>&1; then
    EXCLUDE_LIST=()
    while IFS= read -r excl; do
        [ -n "${excl}" ] && EXCLUDE_LIST+=("${excl}")
    done < <(jq -r '.diff_cover_excludes // [] | .[]' "${CONFIG_JSON}")
    if [ ${#EXCLUDE_LIST[@]} -gt 0 ]; then
        DIFF_COVER_EXCLUDE_ARGS=("--exclude" "${EXCLUDE_LIST[@]}")
    fi
fi

# ── Dependency preflight ────────────────────────────────────────────────────
# Xcode installs llvm-cov/llvm-profdata behind xcrun on many macOS shells.
if command -v xcrun >/dev/null 2>&1 && xcrun -f llvm-cov >/dev/null 2>&1; then
    LLVM_TOOL_DIR="$(dirname "$(xcrun -f llvm-cov)")"
    case ":${PATH}:" in
        *":${LLVM_TOOL_DIR}:"*) ;;
        *) export PATH="${LLVM_TOOL_DIR}:${PATH}" ;;
    esac
fi

missing=()
for tool in clang llvm-profdata llvm-cov cmake ctest python3 git; do
    command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
done

# diff-cover is a Python module — `python3 -m diff_cover` works whether
# it's installed via pip or pipx.
if ! python3 -c "import diff_cover" 2>/dev/null; then
    missing+=("python3-module:diff_cover")
fi

if [ "${#missing[@]}" -gt 0 ]; then
    echo "[local_diff_cover] missing required deps:" >&2
    for m in "${missing[@]}"; do
        echo "  - ${m}" >&2
    done
    echo "" >&2
    echo "Install:" >&2
    echo "  pip install --user 'diff-cover>=9'" >&2
    echo "  # clang/llvm-cov/llvm-profdata: ship with Xcode (macOS) or apt install clang llvm (Linux)" >&2
    exit 2
fi

# ── Ensure compare branch is fetched ────────────────────────────────────────
# diff-cover needs origin/main reachable for the merge-base. Fetch
# silently — the user might be offline; degrade to whatever's local.
if [[ "${COMPARE_BRANCH}" == origin/* ]]; then
    remote_branch="${COMPARE_BRANCH#origin/}"
    git fetch --no-tags --quiet origin "${remote_branch}" 2>/dev/null || \
        echo "[local_diff_cover] WARN: could not fetch ${COMPARE_BRANCH}; using local copy" >&2
fi

# ── Build coverage ──────────────────────────────────────────────────────────
# build-cov/ lives separately from build/ so we don't trash the
# developer's main CMake cache (which is non-coverage).
#
# Taken after the dependency preflight so a missing-dep exit 2 never makes a
# concurrent run wait on a lock this one was never going to use.
acquire_build_cov_lock

echo "=== Configuring coverage build in ${BUILD_DIR} ==="

# Pick the right Clang driver per platform — clang-cl on Windows accepts
# MSVC-style flags from bundled deps; plain clang fails on /W3 etc.
if [ "${OS:-}" = "Windows_NT" ] || [ -n "${MSYSTEM:-}" ]; then
    CLANG_C=clang-cl
    CLANG_CXX=clang-cl
elif [ "$(uname -s)" = "Darwin" ] && command -v xcrun >/dev/null 2>&1 \
    && xcrun -f clang >/dev/null 2>&1 && xcrun -f clang++ >/dev/null 2>&1; then
    CLANG_C="$(xcrun -f clang)"
    CLANG_CXX="$(xcrun -f clang++)"
else
    CLANG_C=clang
    CLANG_CXX=clang++
fi
CI_PYTHON="$(python3 "${REPO_ROOT}/tools/ci/find_python311.py")"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPULP_ENABLE_COVERAGE=ON \
    -DPULP_ENABLE_GPU=OFF \
    -DPULP_BUILD_EXAMPLES=OFF \
    -DCMAKE_C_COMPILER="${CLANG_C}" \
    -DCMAKE_CXX_COMPILER="${CLANG_CXX}" \
    -DPython3_EXECUTABLE="${CI_PYTHON}" >/dev/null

# Stale-cache guard (issue #570 in scripts/run_coverage.sh) — same hazard
# applies here.
if ! grep -q '^PULP_ENABLE_COVERAGE:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null; then
    echo "[local_diff_cover] PULP_ENABLE_COVERAGE=ON did not stick — remove ${BUILD_DIR} and retry" >&2
    exit 1
fi

JOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# ── Importer CLI coverage auto-inclusion ────────────────────────────────────
# The framework-importer SDK CLI helpers (tools/cli/import_*.cpp,
# importer_install.cpp, tool_registry.cpp, cmd_import.cpp) are in the measured
# diff-coverage surface (NOT excluded in coverage_config.json), but they are
# exercised by pure-in-process Catch2 tests whose binaries COMPILE those .cpp
# files directly (parse_manifest / compute_write_plan / scan / detection /
# sha256 / version-window / terms-store). Those test cases use lowercase
# Catch2 titles ("import emit …", "tool registry …", "sha256 …", "install
# from …") that no obvious `-R Import` regex matches, so a targeted
# `local_diff_cover.sh` invocation (the path the pre-push hook and
# `pulp coverage diff` use) silently skipped them — the diff-cover gate then
# saw 0% patch coverage on every importer PR and was admin-bypassed even
# though the code IS tested. Wire the canonical importer test targets + their
# ctest case regex in here so any diff touching the importer CLI source builds
# and runs them, and the gate measures the importer code legitimately.
#
# The five targets each link the relevant import helper TU directly:
#   pulp-test-cli-import            ← import_detect.cpp, import_spi.cpp
#   pulp-test-cli-import-emit       ← import_emit.cpp, import_emit_scan.cpp, import_detect.cpp
#   pulp-test-cli-import-terms      ← import_terms.cpp
#   pulp-test-cli-tool-registry     ← tool_registry.cpp, importer_install.cpp, import_spi.cpp
#   pulp-test-cli-importer-install  ← importer_install.cpp, tool_registry.cpp, import_spi.cpp
# (cmd_import.cpp / import_run.cpp are dispatcher/orchestrator TUs only
# reached via a CLI shell-out, so they are NOT attributable to these
# in-process tests; their lines stay measured-but-shell-out-covered.)
IMPORTER_COVERAGE_TARGETS=(
    pulp-test-cli-import
    pulp-test-cli-import-emit
    pulp-test-cli-import-terms
    pulp-test-cli-tool-registry
    pulp-test-cli-importer-install
)
# Catch2 case titles for the targets above. These titles are lowercase, so the
# regex is lowercase too — the extra ctest pass below runs them regardless of
# the caller-supplied PULP_DIFF_COVER_CTEST_REGEX (which an `-R Import`-style
# uppercase value would never have matched — the original 0%-patch root cause).
IMPORTER_COVERAGE_CTEST_REGEX='import |tool registry |tool lookup |tool install |tool uninstall |tool command |sha256 |install from |install refuses |uninstall removes |pulp tool |pulp add '
# Source paths whose coverage these targets attribute. A diff touching any of
# them auto-includes the importer targets in a targeted build.
IMPORTER_COVERAGE_PATHS_REGEX='^tools/cli/(import_|importer_install\.cpp|tool_registry\.cpp|cmd_import\.cpp)'

# Match the SAME change set diff-cover measures: the merge-base diff PLUS
# staged and unstaged working-tree changes. `diff-cover` reports over
# "<base>...HEAD, staged and unstaged changes", so a working-tree-only edit to
# an importer file (the common pre-push case) must still trip auto-inclusion.
# Union three name-only diffs: committed (merge-base), staged, and unstaged.
importer_diff_touched=0
changed_for_importer="$(
    {
        git diff --name-only "${COMPARE_BRANCH}...HEAD" 2>/dev/null
        git diff --name-only "${COMPARE_BRANCH}" 2>/dev/null
        git diff --name-only --cached "${COMPARE_BRANCH}" 2>/dev/null
    } | sort -u
)"
if echo "${changed_for_importer}" | grep -qE "${IMPORTER_COVERAGE_PATHS_REGEX}"; then
    importer_diff_touched=1
fi

# ── Hosted plugin-slot RT-safety coverage auto-inclusion ────────────────────
# core/host/src/plugin_slot_*.cpp RT-safety (the prepare()-reserve / no-alloc
# contract) is exercised by tests that LOAD a real plugin fixture
# (PulpGain.clap / .vst3). This fast local build sets PULP_BUILD_EXAMPLES=OFF
# for speed, so those fixtures aren't built and the tests skip — making a
# plugin-slot diff read ~0% patch even though CI's full coverage build
# (run_coverage.sh leaves PULP_BUILD_EXAMPLES at its default ON) measures them.
# When a slot file changes, reconfigure build-cov with examples ON (GPU stays
# OFF; the GPU-only examples self-skip on PULP_ENABLE_GPU. Design-import stays
# ON — PULP_BUILD_TESTS=ON requires it — but in this targeted run we build only
# the PulpGain fixtures + host test, never the design-import examples) so the
# fixture coverage is attributed.
HOSTED_SLOT_PATHS_REGEX='^core/host/src/plugin_slot_'
HOSTED_SLOT_COVERAGE_TARGETS=(PulpGain_CLAP pulp-test-host)
HOSTED_SLOT_COVERAGE_CTEST_REGEX='allocation-free after prepare'
hosted_slot_diff_touched=0
if echo "${changed_for_importer}" | grep -qE "${HOSTED_SLOT_PATHS_REGEX}"; then
    hosted_slot_diff_touched=1
fi

if [ "$#" -gt 0 ]; then
    BUILD_TARGETS=("$@")
    # Targeted build that touches importer CLI source: ensure the importer
    # test binaries are built so their in-process coverage is attributable.
    if [ "${importer_diff_touched}" -eq 1 ]; then
        for t in "${IMPORTER_COVERAGE_TARGETS[@]}"; do
            case " ${BUILD_TARGETS[*]} " in
                *" ${t} "*) ;;
                *) BUILD_TARGETS+=("${t}") ;;
            esac
        done
        echo "[local_diff_cover] importer CLI source changed — added importer test targets to the build set" >&2
    fi
    echo "=== Building targets: ${BUILD_TARGETS[*]} ==="
    cmake --build "${BUILD_DIR}" -j"${JOBS}" --target "${BUILD_TARGETS[@]}"
else
    echo "=== Building all targets (slow) ==="
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
fi

# Plugin-slot diff: the main build above ran with PULP_BUILD_EXAMPLES=OFF (no
# plugin fixtures), so the slot no-alloc tests would skip and the slot lines
# read uncovered. Reconfigure with examples ON and build ONLY the PulpGain
# fixtures + host test (never the GPU/design-import examples, which the no-GPU
# build can't link) so the fixture-gated tests run and their coverage counts.
# Done as a post-pass so the main build stays examples-OFF/safe for both the
# targeted and the no-args (pre-push) paths, and only fires for slot diffs.
if [ "${hosted_slot_diff_touched}" -eq 1 ]; then
    echo "[local_diff_cover] plugin-slot source changed — building PulpGain fixtures for hosted-slot coverage" >&2
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DPULP_BUILD_EXAMPLES=ON >/dev/null
    cmake --build "${BUILD_DIR}" -j"${JOBS}" --target "${HOSTED_SLOT_COVERAGE_TARGETS[@]}"
fi

# ── Run tests with profile output ───────────────────────────────────────────
PROFRAW_DIR="${BUILD_DIR}/profraw"
mkdir -p "${PROFRAW_DIR}"
find "${PROFRAW_DIR}" -name '*.profraw' -type f -delete
# Use LLVM's `%Nm` merge pool, sized to the bounded CTest concurrency above.
# Plain `%m` is a one-file pool; parallel exits corrupted that sole file on
# Linux. A pool preserves online merging without the unbounded file count and
# PID-reuse risk of `%p`.
export LLVM_PROFILE_FILE="${PROFRAW_DIR}/pulp-%${DIFF_COVER_TEST_JOBS}m.profraw"

echo "=== Running tests ==="
run_coverage_ctest "${BUILD_DIR}" "${PULP_DIFF_COVER_CTEST_REGEX:-}" || \
    echo "[local_diff_cover] WARN: ctest exited non-zero — generating partial report" >&2

# Second ctest pass for the importer CLI cases. ctest's `-R` takes a single
# regex, so when the run above was narrowed by PULP_DIFF_COVER_CTEST_REGEX the
# lowercase importer titles would be filtered out — leaving the importer .cpp
# coverage maps present (the binaries link them) but unexecuted, i.e. 0%
# patch coverage. Run the importer cases case-insensitively into the SAME
# profraw dir so their hits merge with the rest before export. Only fires
# when the diff touched importer CLI source AND the run above was narrowed;
# an un-narrowed full run already covers these.
if [ "${importer_diff_touched}" -eq 1 ] && [ -n "${PULP_DIFF_COVER_CTEST_REGEX:-}" ]; then
    echo "[local_diff_cover] running importer CLI ctest cases so their in-process coverage is attributed" >&2
    run_coverage_ctest "${BUILD_DIR}" "${IMPORTER_COVERAGE_CTEST_REGEX}" \
        || echo "[local_diff_cover] WARN: importer ctest pass exited non-zero — continuing with partial report" >&2
fi

# Second ctest pass for the hosted-slot no-alloc cases, for the same reason as
# the importer pass: when narrowed by PULP_DIFF_COVER_CTEST_REGEX their titles
# would be filtered out, leaving the slot coverage maps present but unexecuted.
if [ "${hosted_slot_diff_touched}" -eq 1 ] && [ -n "${PULP_DIFF_COVER_CTEST_REGEX:-}" ]; then
    echo "[local_diff_cover] running hosted-slot no-alloc ctest cases for coverage attribution" >&2
    run_coverage_ctest "${BUILD_DIR}" "${HOSTED_SLOT_COVERAGE_CTEST_REGEX}" \
        || echo "[local_diff_cover] WARN: hosted-slot ctest pass exited non-zero — continuing with partial report" >&2
fi

# ── Merge profiles ──────────────────────────────────────────────────────────
PROFDATA="${BUILD_DIR}/coverage/pulp.profdata"
mkdir -p "${BUILD_DIR}/coverage"
echo "=== Merging profiles ==="
MERGE_LOG="${BUILD_DIR}/coverage/llvm-profdata-merge.log"
PROFILE_SHARDS=$(find "${PROFRAW_DIR}" -name '*.profraw' -type f | wc -l | tr -d ' ')
if [[ "${PROFILE_SHARDS}" -eq 0 ]]; then
    echo "[local_diff_cover] no raw profile shards were produced" >&2
    exit 1
fi
if ! find "${PROFRAW_DIR}" -name '*.profraw' -type f -print0 \
    | xargs -0 llvm-profdata merge -sparse --failure-mode=all \
        -o "${PROFDATA}" 2>"${MERGE_LOG}"; then
    cat "${MERGE_LOG}" >&2
    exit 1
fi
cat "${MERGE_LOG}" >&2
INVALID_PROFILE_SHARDS=$(grep -Ec '^(warning|error): .*\.profraw:' "${MERGE_LOG}" || true)
if [[ "${INVALID_PROFILE_SHARDS}" -gt 25 \
      && $((INVALID_PROFILE_SHARDS * 100)) -gt $((PROFILE_SHARDS * 5)) ]]; then
    echo "[local_diff_cover] ${INVALID_PROFILE_SHARDS}/${PROFILE_SHARDS} raw profile shards were invalid (>5%) — refusing to publish incomplete coverage" >&2
    exit 1
fi
echo "=== Merged ${PROFILE_SHARDS} raw profile shard(s); ignored ${INVALID_PROFILE_SHARDS} invalid shard(s) ==="

# ── Gather binaries for llvm-cov -object ────────────────────────────────────
# Mirror scripts/run_coverage.sh's binary discovery so we cover the same
# surface CI does. Without the non-test executable / loadable-module
# passes below, llvm-cov sees only test binaries — coverage data from
# CLI shell-out tests (cmd_coverage.cpp, cmd_loop.cpp, etc.) never
# propagates, and any first-party file exercised end-to-end through
# pulp-cli / pulp-standalone / pulp-inspect is silently dropped from
# the diff-cover gate. See issue #919.
BINARIES=()

# 1. Test executables — primary coverage drivers. Keep them before
#    archive-only coverage maps so files linked into both a static
#    archive and a test binary keep the executed test binary's counters.
#
# 2. First-party static archives below still expose every instrumented TU
#    when no test transitively links it.
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}/test" -maxdepth 2 -type f -perm -u+x \
        ! -name '*.cmake' ! -name '*.txt' 2>/dev/null || true
)

# 2. First-party static archives — expose every instrumented TU even
#    when no test transitively links it. `pulp-*.lib` covers Windows
#    where clang-cl emits MSVC-style archives.
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}" -type f \
        \( -name 'libpulp-*.a' -o -name 'pulp-*.lib' \) \
        2>/dev/null || true
)

# 3. First-party non-test executables — CLI, standalone host, inspector.
#    These are the targets shell-out tests actually invoke; without them
#    cmd_coverage.cpp et al. never accumulate coverage.
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}/tools" "${BUILD_DIR}/inspect" \
        -maxdepth 3 -type f -perm -u+x \
        ! -name '*.cmake' ! -name '*.txt' ! -name '*.o' \
        2>/dev/null || true
)

# 4. Loadable first-party modules under bindings/ that execute
#    instrumented code under test (Python smoke target etc.).
while IFS= read -r f; do BINARIES+=("-object" "$f"); done < <(
    find "${BUILD_DIR}/bindings" -type f \
        \( -name 'pulp*.so' -o -name 'pulp*.pyd' -o -name 'pulp*.dylib' \) \
        ! -path "${BUILD_DIR}/bindings/python/*" \
        2>/dev/null || true
)

if [ "${#BINARIES[@]}" -eq 0 ]; then
    echo "[local_diff_cover] no binaries found under ${BUILD_DIR}" >&2
    exit 1
fi

# ── Pre-flight probe to drop unloadable archives (#566 pattern) ────────────
PROBED=()
prev=""
for tok in "${BINARIES[@]}"; do
    if [[ "${prev}" == "-object" ]]; then
        if llvm-cov report -object="${tok}" -instr-profile="${PROFDATA}" \
                >/dev/null 2>&1; then
            PROBED+=("-object" "${tok}")
        fi
    fi
    prev="${tok}"
done
BINARIES=("${PROBED[@]}")
if [ "${#BINARIES[@]}" -eq 0 ]; then
    echo "[local_diff_cover] pre-flight left zero loadable -object entries" >&2
    exit 1
fi

# ── Generate Cobertura XML via lcov_cobertura.py ───────────────────────────
# Same pipeline scripts/run_coverage.sh uses — `llvm-cov export --format=lcov`
# then the vendored lcov_cobertura.py converter.
#
# Issue #1058: `llvm-cov export --format=lcov` is not gcov-aware and does NOT
# honor `LCOV_EXCL_START` / `LCOV_EXCL_STOP` markers in source. Without the
# `lcov --remove` pass below, those markers are silently documentation-only
# and excluded ranges still appear as Missing in diff-cover output. We pipe
# through `lcov --remove <raw> '*'` (which exercises lcov's gcov parser
# WITHOUT actually removing any files via the wildcard) so excluded ranges
# get stripped before the Cobertura conversion. Falls back to a straight
# copy + warning when `lcov` isn't installed (CI / Linux dev machines often
# lack it) so the existing pipeline keeps working with the prior behavior.
COVERAGE_IGNORE_REGEX='(^|/)(_deps|external|test|[Cc]atch2|build|build-cov|build-coverage|examples|fetchcontent-src|sandbox-e2e)/'
RAW_LCOV_FILE="${BUILD_DIR}/coverage/coverage.raw.lcov"
LCOV_FILE="${BUILD_DIR}/coverage/coverage.lcov"
COBERTURA_XML="${BUILD_DIR}/coverage.cobertura.xml"
LCOV_COBERTURA="${REPO_ROOT}/tools/scripts/lcov_cobertura.py"

if [ ! -f "${LCOV_COBERTURA}" ]; then
    echo "[local_diff_cover] missing converter: ${LCOV_COBERTURA}" >&2
    exit 1
fi

echo "=== llvm-cov export → LCOV ==="
llvm-cov export --format=lcov \
    "${BINARIES[@]}" \
    -instr-profile="${PROFDATA}" \
    -ignore-filename-regex="${COVERAGE_IGNORE_REGEX}" \
    > "${RAW_LCOV_FILE}"

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
        echo "[local_diff_cover] WARN: lcov --filter region failed; falling back to raw .lcov" >&2
        echo "[local_diff_cover]       (LCOV_EXCL markers will not be honored — needs lcov >= 2.0)" >&2
    fi
else
    cp "${RAW_LCOV_FILE}" "${LCOV_FILE}"
    echo "[local_diff_cover] note: lcov not installed — LCOV_EXCL markers won't be honored." >&2
    echo "[local_diff_cover] install with: brew install lcov  /  sudo apt install lcov" >&2
fi

echo "=== LCOV → Cobertura XML ==="
python3 "${LCOV_COBERTURA}" "${LCOV_FILE}" \
    --output "${COBERTURA_XML}" \
    --base-dir "${REPO_ROOT}"

# ── Run diff-cover ──────────────────────────────────────────────────────────
if [ ${#DIFF_COVER_EXCLUDE_ARGS[@]} -gt 0 ]; then
    echo "=== diff-cover (--compare-branch=${COMPARE_BRANCH} --fail-under=${THRESHOLD} excludes=${#DIFF_COVER_EXCLUDE_ARGS[@]}) ==="
else
    echo "=== diff-cover (--compare-branch=${COMPARE_BRANCH} --fail-under=${THRESHOLD}) ==="
fi
mkdir -p "$(dirname "${HTML_REPORT}")"
set +e
python3 -m diff_cover.diff_cover_tool \
    "${COBERTURA_XML}" \
    --compare-branch="${COMPARE_BRANCH}" \
    --fail-under="${THRESHOLD}" \
    "${DIFF_COVER_EXCLUDE_ARGS[@]}" \
    --html-report="${HTML_REPORT}"
rc=$?
set -e

if [ "${rc}" -eq 0 ]; then
    echo ""
    echo "[local_diff_cover] OK — diff coverage at or above ${THRESHOLD}%."
    echo "[local_diff_cover] HTML report: ${HTML_REPORT}"
    exit 0
fi

echo ""
echo "[local_diff_cover] FAIL — diff coverage below ${THRESHOLD}%."
echo "[local_diff_cover] HTML report: ${HTML_REPORT}"
echo "[local_diff_cover] To bypass for this push: PULP_SKIP_DIFF_COVER=1"
exit 1
