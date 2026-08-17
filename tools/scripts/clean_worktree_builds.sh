#!/usr/bin/env bash
#
# clean_worktree_builds.sh — reclaim `build/` directories from finished worktrees.
#
# Every agent session and every parallel slice gets its own worktree, and each
# worktree grows a 10-40 GB `build/`. Nothing reaps them: the documented cleanup
# ("git worktree remove … after landing") depends on remembering, and dozens of
# sessions end without a tidy closeout. The shared volume fills, and the next
# person to touch it gets a build that dies for space while reporting something
# else entirely. `clean_build_cov.sh` covers `build-cov` only, by design; this
# covers the far larger `build/`.
#
# It removes a worktree's `build/` — never the worktree, never source, never
# uncommitted work. The worst case is a rebuild.
#
# THE GATE — all five must hold, or the directory is kept:
#
#   1. STRUCTURE — the path is `<W>/build`, `<W>` is a directory `git worktree
#      list` reports for THIS repository, `<W>/.git` exists, and the path is a
#      real directory rather than a symlink.
#   2. HISTORY — the exact worktree HEAD is a strict ancestor of the current
#      default branch. A deleted remote branch is not completion evidence:
#      unique commits must never lose their only warm build by inference.
#   3. LINEAGE — the local continuity registry records this exact branch and
#      exact HEAD as `merged`, with the merged PR as immutable provenance.
#   4. IDLE — nothing under the build directory modified in the last
#      PULP_WORKTREE_BUILD_IDLE_HOURS hours (default 2).
#   5. QUIET — the shared build-directory exclusion lock is held by this
#      reaper, and no other live process names, holds open, or has its cwd in
#      the worktree.
#
# Each condition is individually imperfect; requiring all five is what makes the
# combination safe. It is conservative in one direction only: a directory that
# cannot be proven finished is kept.
#
# Usage:
#   tools/scripts/clean_worktree_builds.sh                 # dry-run: list + total reclaimable
#   tools/scripts/clean_worktree_builds.sh --yes           # delete
#   tools/scripts/clean_worktree_builds.sh --verbose       # also explain every skip
#   PULP_WORKTREE_BUILD_IDLE_HOURS=24 …                    # widen the idle window
#   PULP_WORKTREES_ROOT=/path/prefix …                     # only worktrees under this prefix
#
# Exit codes:
#   0 — ran (deleted, or listed what it would delete)
#   2 — bad argument or bad configuration
#   3 — could not reach origin, so branch state is unknown; nothing removed
#
# Scheduling this is deliberately NOT part of the script. Whether a machine runs
# a reaper on a timer, and how often, is an operator decision.
set -euo pipefail

APPLY=0
VERBOSE=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) APPLY=1 ;;
        --verbose|-v) VERBOSE=1 ;;
        -h|--help)
            sed -n '2,50p' "$0"; exit 0 ;;
        *) echo "clean_worktree_builds: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd -P)"
REPO_COMMON_DIR="$(git -C "${REPO_ROOT}" rev-parse --git-common-dir)"
case "${REPO_COMMON_DIR}" in
    /*) ;;
    *) REPO_COMMON_DIR="${REPO_ROOT}/${REPO_COMMON_DIR}" ;;
esac
REPO_COMMON_DIR="$(cd "${REPO_COMMON_DIR}" && pwd -P)"
IDLE_HOURS="${PULP_WORKTREE_BUILD_IDLE_HOURS:-2}"
ROOT_FILTER="${PULP_WORKTREES_ROOT:-}"
# Resolve the filter the same way git reports worktree paths (physically), or a
# filter written as /tmp/… matches nothing against a worktree git calls
# /private/tmp/… and the sweep silently covers zero directories.
if [ -n "${ROOT_FILTER}" ] && [ -d "${ROOT_FILTER}" ]; then
    ROOT_FILTER="$(cd "${ROOT_FILTER}" && pwd -P)"
fi

if ! [[ "${IDLE_HOURS}" =~ ^[0-9]+$ ]]; then
    echo "clean_worktree_builds: invalid PULP_WORKTREE_BUILD_IDLE_HOURS: '${IDLE_HOURS}'" >&2
    exit 2
fi

# Scratch under `mktemp -d`, never a fixed /tmp name. Several agents share /tmp
# on these machines and a fixed filename is silently replaced between the write
# and the read: a destructive loop that takes its target list from a clobbered
# file deletes paths some other process chose.
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/pulp-reap-XXXXXX")"
trap 'rm -rf -- "${SCRATCH}"' EXIT

# ── Structural invariant ───────────────────────────────────────────────────
# Re-checked immediately before every `rm -rf`, never only when the candidate
# list is built. A list is not evidence. During the manual sweep this script
# encodes, the target list was read from a fixed /tmp path that another agent
# had overwritten with unrelated data, and the only thing that prevented an
# unbounded delete was a per-path check at the moment of deletion. This is that
# check, and it is why it is a function called twice rather than a filter
# applied once.
#
# $1 = candidate path, $2 = file listing the worktree roots this run enumerated.
# Prints why it refuses; returns non-zero.
assert_reapable_path() {
    local dir="$1" roots_file="$2" parent parent_physical candidate_common
    case "${dir}" in
        # Absolute, and the last component is exactly `build`. In a case
        # pattern `*` spans `/`, so this accepts any depth.
        /*/build) ;;
        *) echo "REFUSING (not an absolute path ending in /build): ${dir}" >&2; return 1 ;;
    esac
    if [ -L "${dir}" ]; then
        echo "REFUSING (symlink, not a directory): ${dir}" >&2; return 1
    fi
    if [ ! -d "${dir}" ]; then
        echo "REFUSING (not a directory): ${dir}" >&2; return 1
    fi
    parent="${dir%/build}"
    parent_physical="$(cd "${parent}" 2>/dev/null && pwd -P || true)"
    if [ "${parent_physical}" != "${parent}" ]; then
        echo "REFUSING (worktree root has a symlink/replacement component): ${dir}" >&2
        return 1
    fi
    if [ ! -e "${parent}/.git" ]; then
        echo "REFUSING (parent is not a git worktree): ${dir}" >&2; return 1
    fi
    if [ ! -f "${roots_file}" ] || ! grep -qxF "${parent}" "${roots_file}"; then
        echo "REFUSING (parent is not an enumerated worktree of this repo): ${dir}" >&2
        return 1
    fi
    if ! git -C "${REPO_ROOT}" worktree list --porcelain \
            | awk -v wanted="worktree ${parent}" \
                '$0 == wanted { found=1 } END { exit !found }'; then
        echo "REFUSING (parent is no longer a registered worktree): ${dir}" >&2
        return 1
    fi
    candidate_common="$(git -C "${parent}" rev-parse --git-common-dir 2>/dev/null || true)"
    case "${candidate_common}" in
        /*) ;;
        '') echo "REFUSING (candidate Git authority unavailable): ${dir}" >&2; return 1 ;;
        *) candidate_common="${parent}/${candidate_common}" ;;
    esac
    candidate_common="$(cd "${candidate_common}" 2>/dev/null && pwd -P || true)"
    if [ "${candidate_common}" != "${REPO_COMMON_DIR}" ]; then
        echo "REFUSING (candidate belongs to a different Git repository): ${dir}" >&2
        return 1
    fi
    return 0
}

# macOS resolves /tmp, /var and /etc through /private, so the same directory has
# two spellings: git reports the physical one, while a command line usually
# carries whichever the caller typed. A process gate that tests only one of them
# misses a live build — and worktrees under /tmp are common here, so this is
# precisely the case that would reap a directory being written to right now.
path_aliases() {
    local p="$1"
    printf '%s\n' "${p}"
    case "${p}" in
        /private/tmp/*|/private/var/*|/private/etc/*) printf '%s\n' "${p#/private}" ;;
        /tmp/*|/var/*|/etc/*) printf '%s\n' "/private${p}" ;;
    esac
}

# Return 0 when active, 1 when proven quiet, 2 when activity cannot be read.
# Command lines are only the cheap first signal; lsof covers cwd and open-file
# activity whose argv is commonly just `ninja`, `make`, or `bash`.
a_live_process_is_using() {
    local wt_path="$1" spelling
    [ -f "${PS_SNAPSHOT:-}" ] && [ -f "${LSOF_SNAPSHOT:-}" ] || return 2
    while IFS= read -r spelling; do
        [ -z "${spelling}" ] && continue
        if grep -qF "${spelling}" "${PS_SNAPSHOT}"; then
            return 0
        fi
    done < <(path_aliases "${wt_path}")
    while IFS= read -r spelling; do
        [ -z "${spelling}" ] && continue
        if awk -v root="${spelling}" '
                /^n/ { path=substr($0, 2); if (path == root || index(path, root "/") == 1) found=1 }
                END { exit !found }
            ' "${LSOF_SNAPSHOT}"; then
            return 0
        fi
    done < <(path_aliases "${wt_path}")
    return 1
}

capture_process_snapshot() {
    local target="$1" raw="${1}.raw" lsof_raw="${LSOF_SNAPSHOT}.raw"
    local lsof_errors="${LSOF_SNAPSHOT}.errors" subshell_pid="${BASHPID:-$$}"
    local lock_pid="${BUILD_LOCK_PID:-0}" shell_pid
    shell_pid="$(sh -c 'echo "$PPID"')"
    if ps -e -ww -o pid= -o args= > "${raw}" 2>/dev/null; then
        :
    elif ps -Ao pid= -o args= > "${raw}" 2>/dev/null; then
        :
    else
        return 1
    fi
    # Remove this script by exact PID. Filtering on the script name would also
    # hide a genuine build whose command happened to include that filename.
    awk -v self="$$" -v child="${subshell_pid}" -v shell="${shell_pid}" -v lock="${lock_pid}" \
        '$1 != self && $1 != child && $1 != shell && $1 != lock { $1=""; sub(/^ +/, ""); print }' \
        "${raw}" > "${target}"
    rm -f "${raw}"
    [ -s "${target}" ] || return 1
    command -v lsof >/dev/null 2>&1 || return 1
    if ! (cd -P "${REPO_ROOT}" && lsof -n -P -Fn) \
            > "${lsof_raw}" 2> "${lsof_errors}"; then
        return 1
    fi
    awk -v self="$$" -v child="${subshell_pid}" -v shell="${shell_pid}" -v lock="${lock_pid}" '
        /^p/ { skip=(substr($0, 2) == self || substr($0, 2) == child || substr($0, 2) == shell || substr($0, 2) == lock) }
        !skip { print }
    ' "${lsof_raw}" > "${LSOF_SNAPSHOT}"
    [ -s "${LSOF_SNAPSHOT}" ] && [ ! -s "${lsof_errors}" ]
}

acquire_build_lock() {
    local build="$1" ready="${SCRATCH}/build-lock-ready-$$-${BASHPID:-0}"
    rm -f "${ready}"
    python3 - "${REPO_ROOT}/tools/ci" "${build}" "${ready}" <<'PY' &
import pathlib
import sys
import time

sys.path.insert(0, sys.argv[1])
import build_dir_lock

with build_dir_lock.exclusive_build_dir(pathlib.Path(sys.argv[2])):
    pathlib.Path(sys.argv[3]).write_text("locked\n")
    while True:
        time.sleep(60)
PY
    BUILD_LOCK_PID=$!
    local attempt
    for ((attempt=0; attempt<200; attempt++)); do
        [ -f "${ready}" ] && return 0
        kill -0 "${BUILD_LOCK_PID}" 2>/dev/null || break
        sleep 0.01
    done
    kill "${BUILD_LOCK_PID}" 2>/dev/null || true
    wait "${BUILD_LOCK_PID}" 2>/dev/null || true
    BUILD_LOCK_PID=""
    return 1
}

release_build_lock() {
    [ -n "${BUILD_LOCK_PID:-}" ] || return 0
    kill "${BUILD_LOCK_PID}" 2>/dev/null || true
    wait "${BUILD_LOCK_PID}" 2>/dev/null || true
    BUILD_LOCK_PID=""
}

activity_is_quiet_for() {
    if a_live_process_is_using "$1"; then
        return 1
    else
        active_rc=$?
        [ "${active_rc}" -eq 1 ]
    fi
}

# Return 0 when idle, 1 when a fresh path exists, 2 when traversal is
# incomplete. The full tree matters: object files normally live far below
# build/CMakeFiles/*/src rather than within two levels of build/.
build_is_idle() {
    local build="$1" found errors
    found="$(mktemp "${SCRATCH}/find-out-XXXXXX")"
    errors="$(mktemp "${SCRATCH}/find-err-XXXXXX")"
    if ! find "${build}" -newer "${STAMP}" -print -quit \
            > "${found}" 2> "${errors}"; then
        return 2
    fi
    [ ! -s "${errors}" ] || return 2
    [ ! -s "${found}" ] || return 1
    return 0
}

path_identity() {
    case "$(uname -s 2>/dev/null || true)" in
        Darwin) stat -f '%d:%i' "$1" 2>/dev/null ;;
        *) stat -c '%d:%i' "$1" 2>/dev/null ;;
    esac
}

# Called only after `cd -P` pins the directory. Every mutable Git/filesystem
# fact is re-read through that pinned cwd, so path replacement cannot redirect
# the eventual relative rename.
pinned_worktree_authority_is_safe() {
    local wt="$1" expected_identity="$2" roots_file="$3"
    local expected_head="$4" expected_branch="$5" expected_tip="$6" candidate_common
    [ "$(pwd -P)" = "${wt}" ] || return 1
    [ "$(path_identity . 2>/dev/null || true)" = "${expected_identity}" ] || return 1
    [ -e .git ] || return 1
    grep -qxF "${wt}" "${roots_file}" || return 1
    git -C "${REPO_ROOT}" worktree list --porcelain \
        | awk -v wanted="worktree ${wt}" \
            '$0 == wanted { found=1 } END { exit !found }' || return 1
    candidate_common="$(git rev-parse --git-common-dir 2>/dev/null || true)"
    case "${candidate_common}" in
        /*) ;;
        '') return 1 ;;
        *) candidate_common="$(pwd -P)/${candidate_common}" ;;
    esac
    candidate_common="$(cd "${candidate_common}" 2>/dev/null && pwd -P || true)"
    [ "${candidate_common}" = "${REPO_COMMON_DIR}" ] || return 1
    current_worktree_identity_proves_merge . "${expected_head}" "${expected_branch}" \
        "${expected_tip}"
}

pinned_worktree_is_safe() {
    [ -d build ] && [ ! -L build ] || return 1
    pinned_worktree_authority_is_safe "$@"
}

restore_quarantine() {
    local quarantine="$1" expected_identity="$2"
    [ ! -e build ] || return 1
    [ -d "${quarantine}" ] && [ ! -L "${quarantine}" ] || return 1
    [ "$(path_identity "${quarantine}" 2>/dev/null || true)" = \
        "${expected_identity}" ] || return 1
    mv "${quarantine}" build
}

# Recursively remove through an opened, identity-verified directory descriptor.
# Pathname replacement after verification cannot redirect recursive deletion:
# only the exact original build inode is traversed. The final pathname removal
# is non-recursive rmdir, so a substituted non-empty directory is preserved.
remove_verified_tree() {
    local quarantine="$1" expected_identity="$2"
    python3 - "${quarantine}" "${expected_identity}" <<'PY'
import os
import stat
import sys

name = sys.argv[1]
expected = tuple(int(part) for part in sys.argv[2].split(":"))
parent_fd = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
root_fd = -1

def identity(st):
    return (st.st_dev, st.st_ino)

def remove_contents(directory_fd):
    for entry in list(os.scandir(directory_fd)):
        entry_stat = os.stat(entry.name, dir_fd=directory_fd, follow_symlinks=False)
        if stat.S_ISDIR(entry_stat.st_mode):
            child_fd = os.open(
                entry.name,
                os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=directory_fd,
            )
            try:
                if identity(os.fstat(child_fd)) != identity(entry_stat):
                    raise OSError("directory entry identity changed")
                remove_contents(child_fd)
            finally:
                os.close(child_fd)
            os.rmdir(entry.name, dir_fd=directory_fd)
        else:
            os.unlink(entry.name, dir_fd=directory_fd)

try:
    root_fd = os.open(
        name,
        os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0),
        dir_fd=parent_fd,
    )
    if identity(os.fstat(root_fd)) != expected:
        raise OSError("quarantine identity changed before descriptor open")
    remove_contents(root_fd)
    # This check can race only with the following non-recursive rmdir. A
    # substituted directory containing anything cannot be recursively erased.
    if identity(os.stat(name, dir_fd=parent_fd, follow_symlinks=False)) != expected:
        raise OSError("quarantine pathname no longer names the opened directory")
    os.rmdir(name, dir_fd=parent_fd)
finally:
    if root_fd >= 0:
        os.close(root_fd)
    os.close(parent_fd)
PY
}

reap_pinned_build() (
    local wt="$1" expected_identity="$2" roots_file="$3"
    local expected_head="$4" expected_branch="$5" expected_tip="$6"
    local quarantine="$7" pinned_now build_identity
    BUILD_LOCK_PID=""
    trap release_build_lock EXIT
    acquire_build_lock "${wt}/build" || return 11
    cd -P "${wt}" || return 12
    pinned_worktree_is_safe "${wt}" "${expected_identity}" "${roots_file}" \
        "${expected_head}" "${expected_branch}" "${expected_tip}" || return 13
    build_is_idle build || return 14
    capture_process_snapshot "${PS_SNAPSHOT}" || return 15
    activity_is_quiet_for "${wt}" || return 16
    [ ! -e "${quarantine}" ] || return 17
    build_identity="$(path_identity build 2>/dev/null || true)"
    [ -n "${build_identity}" ] || return 18
    mv "build" "${quarantine}" || return 18
    [ "$(path_identity "${quarantine}" 2>/dev/null || true)" = \
        "${build_identity}" ] || return 19

    # The rename closes entry by the ordinary build path. Re-snapshot after
    # that synchronization point; any pre-existing process now exposes an
    # fd/cwd under the quarantine and forces restoration.
    if ! capture_process_snapshot "${PS_SNAPSHOT}"; then
        restore_quarantine "${quarantine}" "${build_identity}" 2>/dev/null || true
        cd -P "${REPO_ROOT}" || true
        return 1
    fi
    pinned_now="$(pwd -P 2>/dev/null || true)"
    if [ -z "${pinned_now}" ] || ! activity_is_quiet_for "${wt}" || \
       { [ "${pinned_now}" != "${wt}" ] && ! activity_is_quiet_for "${pinned_now}"; }; then
        restore_quarantine "${quarantine}" "${build_identity}" 2>/dev/null || true
        cd -P "${REPO_ROOT}" || true
        return 1
    fi
    # A short-lived compiler can write after the first freshness scan and exit
    # before the post-rename process snapshot is inspected. Re-read the full
    # quarantined tree after that snapshot; fresh or unreadable state restores
    # build/ instead of authorizing deletion.
    if ! build_is_idle "${quarantine}"; then
        restore_quarantine "${quarantine}" "${build_identity}" 2>/dev/null || true
        cd -P "${REPO_ROOT}" || true
        return 1
    fi
    # Re-read every authority after quarantine and require the directory entry
    # to remain the exact original build inode. A moved/replaced entry is
    # preserved in place; it is never deleted or moved over a new build/.
    if ! pinned_worktree_authority_is_safe "${wt}" "${expected_identity}" \
            "${roots_file}" "${expected_head}" "${expected_branch}" \
            "${expected_tip}" || \
       [ -e build ] || [ ! -d "${quarantine}" ] || [ -L "${quarantine}" ] || \
       [ "$(path_identity "${quarantine}" 2>/dev/null || true)" != \
            "${build_identity}" ]; then
        restore_quarantine "${quarantine}" "${build_identity}" 2>/dev/null || true
        cd -P "${REPO_ROOT}" || true
        return 1
    fi
    if ! remove_verified_tree "${quarantine}" "${build_identity}"; then
        # Descriptor traversal may already have removed part of the old cache.
        # Never publish a partially processed tree back at the ordinary build/
        # path; leave any survivor quarantined for explicit inspection.
        cd -P "${REPO_ROOT}" || true
        return 1
    fi
    cd -P "${REPO_ROOT}" || return 1
)

merged_pr_proves_exact_head() {
    local pr="$1" head="$2" expected_tip="$3" owner repo number row
    local state merged_at pr_head base merge_sha cache
    [[ "${pr}" =~ ^https://github\.com/([^/]+)/([^/]+)/pull/([0-9]+)$ ]] || return 1
    owner="${BASH_REMATCH[1]}"; repo="${BASH_REMATCH[2]}"; number="${BASH_REMATCH[3]}"
    cache="${SCRATCH}/merged-pr-${head}"
    if [ -f "${cache}" ] && grep -qxF "${pr}"$'\t'"${head}"$'\t'"${expected_tip}" "${cache}"; then
        return 0
    fi
    command -v gh >/dev/null 2>&1 || return 1
    row="$(gh api "repos/${owner}/${repo}/pulls/${number}" \
        --jq '[.state, (.merged_at // ""), .head.sha, .base.ref, (.merge_commit_sha // "")] | @tsv' \
        2>/dev/null || true)"
    IFS=$'\t' read -r state merged_at pr_head base merge_sha <<< "${row}"
    [ "${state}" = closed ] && [ -n "${merged_at}" ] || return 1
    [ "${pr_head}" = "${head}" ] && [ "${base}" = "${DEFAULT_BRANCH}" ] || return 1
    [ -n "${merge_sha}" ] || return 1
    git -C "${REPO_ROOT}" merge-base --is-ancestor "${merge_sha}" \
        "${expected_tip}" 2>/dev/null || return 1
    printf '%s\t%s\t%s\n' "${pr}" "${head}" "${expected_tip}" > "${cache}"
}

# Read the repository's shared lineage registry, never candidate-controlled
# source, and require exact-head ancestry or a live merged-PR proof. The latter
# is necessary for squash merges, whose source head is intentionally not an
# ancestor of main even though its merged commit is.
lineage_proves_exact_merge() {
    local branch="$1" head="$2" expected_tip="$3" status durable_sha pr
    [ -n "${branch}" ] || return 1
    status="$(git -C "${REPO_ROOT}" config --local --get \
        "branch.${branch}.pulpWorktreeStatus" 2>/dev/null || true)"
    durable_sha="$(git -C "${REPO_ROOT}" config --local --get \
        "branch.${branch}.pulpWorktreeDurableSha" 2>/dev/null || true)"
    pr="$(git -C "${REPO_ROOT}" config --local --get \
        "branch.${branch}.pulpWorktreePr" 2>/dev/null || true)"
    [ "${status}" = "merged" ] || return 1
    [ -n "${head}" ] && [ "${durable_sha}" = "${head}" ] || return 1
    [[ "${pr}" =~ ^https://github\.com/[^/]+/[^/]+/pull/[0-9]+$ ]] || return 1
    if git -C "${REPO_ROOT}" merge-base --is-ancestor "${head}" \
            "${expected_tip}" 2>/dev/null; then
        return 0
    fi
    merged_pr_proves_exact_head "${pr}" "${head}" "${expected_tip}"
}

current_worktree_identity_proves_merge() {
    local wt="$1" expected_head="$2" expected_branch="$3" expected_tip="$4"
    local current_head current_branch current_tip
    current_head="$(git -C "${wt}" rev-parse --verify HEAD 2>/dev/null || true)"
    current_branch="$(git -C "${wt}" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    [ "${current_head}" = "${expected_head}" ] || return 1
    [ "${current_branch}" = "${expected_branch}" ] || return 1
    current_tip="$(git -C "${REPO_ROOT}" rev-parse --verify --quiet \
        "refs/remotes/origin/${DEFAULT_BRANCH}" 2>/dev/null || true)"
    [ -n "${expected_tip}" ] && [ "${current_tip}" = "${expected_tip}" ] || return 1
    [ "${current_head}" != "${expected_tip}" ] || return 1
    lineage_proves_exact_merge "${current_branch}" "${current_head}" "${expected_tip}"
}

# Let tests exercise the invariant and the path handling without enumerating or
# deleting anything.
if [ "${PULP_REAP_LIB_ONLY:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

DEFAULT_BRANCH="$(git -C "${REPO_ROOT}" symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null || true)"
DEFAULT_BRANCH="${DEFAULT_BRANCH#origin/}"
DEFAULT_BRANCH="${DEFAULT_BRANCH:-main}"
# Destructive-adjacent decisions require a current authority. An unavailable
# origin is a stop, not permission to use stale ancestry.
if [ "$(git -C "${REPO_ROOT}" rev-parse --is-shallow-repository 2>/dev/null || true)" = "true" ]; then
    if ! git -C "${REPO_ROOT}" fetch --no-tags --quiet --unshallow origin 2>/dev/null; then
        echo "clean_worktree_builds: full history is unavailable. Nothing removed." >&2
        exit 3
    fi
fi
if ! git -C "${REPO_ROOT}" fetch --no-tags --quiet origin "${DEFAULT_BRANCH}" 2>/dev/null; then
    echo "clean_worktree_builds: could not refresh origin/${DEFAULT_BRANCH};" >&2
    echo "clean_worktree_builds: exact merged history is unknown. Nothing removed." >&2
    exit 3
fi
DEFAULT_TIP="$(git -C "${REPO_ROOT}" rev-parse --verify --quiet \
    "refs/remotes/origin/${DEFAULT_BRANCH}" 2>/dev/null || true)"
[ -n "${DEFAULT_TIP}" ] || {
    echo "clean_worktree_builds: origin/${DEFAULT_BRANCH} has no exact tip. Nothing removed." >&2
    exit 3
}

# ── One process snapshot for the whole run ─────────────────────────────────
# A build in flight names the directory it is building on its command line, and
# so does a shell or an agent working inside the worktree. One `ps` gives every
# command line on the machine; the per-worktree test is then a string compare.
PS_SNAPSHOT="${SCRATCH}/ps"
LSOF_SNAPSHOT="${SCRATCH}/lsof"
if ! capture_process_snapshot "${PS_SNAPSHOT}"; then
    # No process list means the "nothing is using it" condition cannot be
    # evaluated at all, and an unreadable gate silently passes everything.
    # Same fail-closed rule as an unreachable origin.
    echo "clean_worktree_builds: could not read process argv/cwd/open-file state, so whether a" >&2
    echo "clean_worktree_builds: build is running is unknown. Nothing removed." >&2
    exit 3
fi

# ── One idle cutoff for the whole run ──────────────────────────────────────
# A stamp file plus POSIX `find -newer` is portable. Relative-time syntax is
# not: `touch -d "-2 hours"` is GNU, `date -v-2H` is BSD, and `-newermt` accepts
# different formats on each.
STAMP="${SCRATCH}/idle-cutoff"
if ! touch -d "-${IDLE_HOURS} hours" "${STAMP}" 2>/dev/null; then
    touch -t "$(date -v-"${IDLE_HOURS}"H +%Y%m%d%H%M)" "${STAMP}"
fi

# ── Enumerate worktrees ────────────────────────────────────────────────────
# `git worktree list`, not a filesystem scan: it is the registry git itself
# keeps, it covers every layout these worktrees live in (Code/agent-worktrees,
# Code/pulp-*, .claude/worktrees, /tmp), and it means a stray directory named
# `build` somewhere in a source tree can never become a candidate. It also
# yields each worktree's branch and HEAD, so the branch gate costs no extra git
# calls.
WORKTREE_ROOTS="${SCRATCH}/worktree-roots"
WORKTREE_TABLE="${SCRATCH}/worktree-table"   # path <TAB> head <TAB> branch-or-empty
git -C "${REPO_ROOT}" worktree list --porcelain \
    | awk '
        /^worktree / { if (path != "") print path "\t" head "\t" branch;
                       path = substr($0, 10); head = ""; branch = ""; next }
        /^HEAD /     { head = substr($0, 6); next }
        /^branch /   { branch = substr($0, 8); sub(/^refs\/heads\//, "", branch); next }
        END          { if (path != "") print path "\t" head "\t" branch }
    ' > "${WORKTREE_TABLE}"
cut -f1 "${WORKTREE_TABLE}" > "${WORKTREE_ROOTS}"

# The first entry `git worktree list` prints is the main worktree. That is where
# a human works, so its `build/` is an interactive rebuild cost rather than
# reclaimable scratch — as is the checkout this script is running from.
MAIN_WORKTREE="$(head -1 "${WORKTREE_ROOTS}" 2>/dev/null || true)"

# State the scope before the numbers. PULP_WORKTREES_ROOT is shared with
# clean_build_cov.sh and is commonly already exported in a shell profile on
# these machines, which silently narrows the sweep: a run reporting "46
# worktree build dirs" on a host that has 100 is not wrong, but it is
# uninterpretable unless the filter is on screen next to the total.
echo "clean_worktree_builds: $(wc -l < "${WORKTREE_ROOTS}" | tr -d ' ') registered worktree(s);" \
     "idle window ${IDLE_HOURS}h; default branch ${DEFAULT_BRANCH}"
if [ -n "${ROOT_FILTER}" ]; then
    echo "clean_worktree_builds: LIMITED to worktrees under ${ROOT_FILTER} (PULP_WORKTREES_ROOT)"
fi

total_kb=0
found=0
reapable=0
deleted=0
kept=0

note_skip() {
    kept=$((kept + 1))
    if [ "${VERBOSE}" -eq 1 ]; then
        echo "  keeping ($1)	$2"
    fi
}

while IFS=$'\t' read -r wt head branch; do
    [ -z "${wt}" ] && continue
    build="${wt}/build"
    [ -d "${build}" ] || continue
    if [ -n "${ROOT_FILTER}" ]; then
        case "${wt}" in "${ROOT_FILTER}"/*|"${ROOT_FILTER}") ;; *) continue ;; esac
    fi
    found=$((found + 1))
    wt_identity="$(path_identity "${wt}" 2>/dev/null || true)"
    if [ -z "${wt_identity}" ]; then
        note_skip "worktree filesystem identity is unreadable" "${build}"
        continue
    fi

    if [ "${wt}" = "${MAIN_WORKTREE}" ] || [ "${wt}" = "${REPO_ROOT}" ]; then
        note_skip "primary checkout" "${build}"
        continue
    fi

    # 2. HISTORY.
    if [ -n "${branch}" ] && [ "${branch}" = "${DEFAULT_BRANCH}" ]; then
        note_skip "on ${DEFAULT_BRANCH}" "${build}"
        continue
    fi
    if [ -n "${DEFAULT_TIP}" ] && [ "${head}" = "${DEFAULT_TIP}" ]; then
        # Sitting exactly at the default tip carries no work of its own and is
        # indistinguishable from a worktree created minutes ago. Ancestry would
        # call it "merged"; treat it as unfinished instead.
        note_skip "at the ${DEFAULT_BRANCH} tip, no commits of its own" "${build}"
        continue
    fi
    if [ -z "${head}" ]; then
        note_skip "unique or unproven history" "${build}"
        continue
    fi
    if ! git -C "${wt}" merge-base --is-ancestor "${head}" "${DEFAULT_TIP}" \
            2>/dev/null && \
       ! lineage_proves_exact_merge "${branch}" "${head}" "${DEFAULT_TIP}"; then
        note_skip "unique or unproven history" "${build}"
        continue
    fi

    # 3. LINEAGE. An ancestor relationship alone includes abandoned branches
    # and freshly-created worktrees. Require explicit exact-head closeout.
    if ! lineage_proves_exact_merge "${branch}" "${head}" "${DEFAULT_TIP}"; then
        note_skip "lineage does not prove this exact head merged" "${build}"
        continue
    fi

    # 4. IDLE. Scan the whole tree; normal object files are nested deeply.
    if build_is_idle "${build}"; then
        :
    else
        idle_rc=$?
        if [ "${idle_rc}" -eq 2 ]; then
            note_skip "build freshness is unreadable" "${build}"
            continue
        fi
        note_skip "modified within ${IDLE_HOURS}h" "${build}"
        continue
    fi

    # 5. QUIET.
    if a_live_process_is_using "${wt}"; then
        note_skip "a live process is working in it" "${build}"
        continue
    else
        active_rc=$?
        if [ "${active_rc}" -eq 2 ]; then
            note_skip "active-use state is unreadable" "${build}"
            continue
        fi
    fi

    # 1. STRUCTURE.
    if ! assert_reapable_path "${build}" "${WORKTREE_ROOTS}"; then
        note_skip "failed the structural check" "${build}"
        continue
    fi

    reapable=$((reapable + 1))
    size_kb="$(du -sk "${build}" 2>/dev/null | awk '{print $1}')"; size_kb="${size_kb:-0}"
    human="$(du -sh "${build}" 2>/dev/null | awk '{print $1}')"
    if [ "${APPLY}" -eq 1 ]; then
        quarantine=".pulp-reap-build-$$"
        if reap_pinned_build "${wt}" "${wt_identity}" "${WORKTREE_ROOTS}" \
                "${head}" "${branch}" "${DEFAULT_TIP}" "${quarantine}"; then
            total_kb=$((total_kb + size_kb))
            echo "  removed ${human}	${build}	(merged, lineage-proven)"
            deleted=$((deleted + 1))
        else
            reap_rc=$?
            echo "  FAILED to remove	${build}"
            [ "${VERBOSE}" -eq 0 ] || echo "    final safety gate ${reap_rc} refused deletion"
            kept=$((kept + 1))
        fi
    else
        total_kb=$((total_kb + size_kb))
        echo "  would remove ${human}	${build}	(merged, lineage-proven)"
    fi
done < "${WORKTREE_TABLE}"

total_gb="$(awk -v kb="${total_kb}" 'BEGIN{printf "%.1f", kb/1024/1024}')"
echo
if [ "${APPLY}" -eq 1 ]; then
    echo "clean_worktree_builds: removed ${deleted} build dir(s), ~${total_gb} GB reclaimed; kept ${kept} of ${found}."
else
    echo "clean_worktree_builds: ${found} worktree build dir(s); ${reapable} reapable, ~${total_gb} GB reclaimable; kept ${kept}."
    if [ "${VERBOSE}" -eq 0 ] && [ "${kept}" -gt 0 ]; then
        echo "clean_worktree_builds: re-run with --verbose to see why each was kept."
    fi
    echo "clean_worktree_builds: re-run with --yes to delete."
fi
