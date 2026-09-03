#!/usr/bin/env bash
#
# clean_worktrees.sh — remove worktrees whose work is provably already in main.
#
# Pulp's workflow creates a worktree per slice and relies on the author to run
# `git worktree remove` when it lands. Nothing enforces that, so they accumulate:
# one M3 sweep found 411 registered worktrees, most landed weeks earlier, each
# holding a full source tree and often a build dir. They fill the shared volume,
# and the next build dies for space while reporting something else entirely.
#
# `git worktree prune` does NOT help: it only drops registrations whose directory
# is already gone. Every one of those 411 was still on disk. `pulp-worktree.sh
# gc` was written for this job but ships as a stub — it reports zero candidates
# even in apply mode, waiting for "the affirmative safety classifier". This is
# that classifier, and the reaper built on it.
#
# THE GATE — all five must hold, or the worktree is kept:
#
#   1. STRUCTURE — not the primary worktree, not bare, directory exists.
#   2. CLEAN — `git status --porcelain` is empty. Uncommitted content exists
#      here and nowhere else; it is never recoverable from a ref.
#   3. CONTAINED — HEAD is an ancestor of origin/main, so every commit is
#      already in the trunk and removal loses no history. A deleted upstream
#      branch is NOT this proof; only exact ancestry is.
#   4. IDLE — no live process has its cwd in the tree or names the tree on its
#      command line.
#   5. NOT ACTIVE — the lineage registry does not record the branch as `active`.
#      Lineage is a discovery aid and never deletion authority, so it is used
#      only as a veto, never as permission.
#
# Removal then runs `git worktree remove` WITHOUT --force, so git's own dirty
# check is an independent second gate on top of check 2.
#
# Conservative in one direction only: a worktree that cannot be proven finished
# is kept, and every retained worktree is REPORTED with its reason — especially
# the at-risk set, which is work that exists on this disk and nowhere else.
#
# Usage:
#   tools/scripts/clean_worktrees.sh                    # dry-run: classify + list
#   tools/scripts/clean_worktrees.sh --yes              # remove the safe set
#   tools/scripts/clean_worktrees.sh --repo path/to/co  # sweep another checkout
#   tools/scripts/clean_worktrees.sh --json             # machine-readable counts
#
# Exit: 0 ok, 2 usage or precondition failure. A worktree that refuses removal
# is reported and does not fail the run.
set -euo pipefail

APPLY=0
JSON=0
REPO_ARG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --yes|-y) APPLY=1 ;;
        --json) JSON=1 ;;
        --repo)
            shift
            REPO_ARG="${1:-}"
            [[ -n "${REPO_ARG}" ]] || { echo "clean_worktrees: --repo requires a value" >&2; exit 2; } ;;
        -h|--help) sed -n '2,46p' "$0"; exit 0 ;;
        *) echo "clean_worktrees: unknown argument '$1'" >&2; exit 2 ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REPO="${REPO_ARG:-${REPO_ROOT}}"
[[ -d "${REPO}" ]] || { echo "clean_worktrees: repo not found: ${REPO}" >&2; exit 2; }
git -C "${REPO}" rev-parse --git-dir >/dev/null 2>&1 || {
    echo "clean_worktrees: not a git repository: ${REPO}" >&2; exit 2; }

# The containment baseline. Without it nothing can be proven disposable, so
# refuse rather than guess: an unresolvable ref is unknown, never safe.
git -C "${REPO}" fetch -q origin main 2>/dev/null || true
if ! MAIN="$(git -C "${REPO}" rev-parse origin/main 2>/dev/null)"; then
    echo "clean_worktrees: cannot resolve origin/main in ${REPO}; nothing removed" >&2
    exit 2
fi

# Liveness, on two independent instruments because either alone has a blind
# spot: a process cwd catches a shell sitting in the tree, and a process argv
# catches a build whose cwd is elsewhere but which names the tree on its command
# line (cmake -B <wt>/build, cc -o <wt>/build/...).
#
# Do not use `pgrep -fl`: on Linux `-l` prints only the executable name even when
# `-f` matched the full argv, which can turn a live build into a false idle
# result. An unreadable process list is UNKNOWN, never evidence that removal is
# safe, so an empty read aborts the run.
if ! ACTIVE_ARGS="$(ps -e -ww -o args= 2>/dev/null)" || [[ -z "${ACTIVE_ARGS}" ]]; then
    echo "clean_worktrees: could not read process command lines; nothing removed" >&2
    exit 2
fi
# lsof is best-effort — absent on some hosts, and argv matching already covers
# builds. Its absence narrows detection; it does not invalidate the run.
LIVE_CWDS="$(lsof -a -d cwd -Fn 2>/dev/null | sed -n 's/^n//p' | sort -u || true)"

# One worktree has several spellings, and the two sides of this comparison do
# not agree on which to use: `git worktree list` reports the PHYSICAL path
# (/private/var/... on macOS) while a process launched through the logical path
# carries /var/... on its command line. Comparing one spelling against the other
# silently finds nothing — a live build would read as idle and its worktree
# would be removed out from under it, which is the exact failure this gate
# exists to prevent. So match against every spelling of the same directory.
path_variants() {
    local path="$1" physical
    physical="$(cd "${path}" 2>/dev/null && pwd -P || true)"
    {
        printf '%s\n' "${path}"
        [[ -n "${physical}" ]] && printf '%s\n' "${physical}"
    } | sed -e 'p' -e 's#^/private/#/#' | sort -u
}

# Match with a here-string, never `printf ... | grep -q`. Under `pipefail` a
# successful `grep -q` exits on its first match, `printf` then dies of SIGPIPE,
# and the pipeline reports failure — so a MATCH would read as NO MATCH and a
# busy worktree would be classified idle. The bug is silent in the safe
# direction only by luck, and it inverts exactly the answer this gate exists to
# give, so the pipe is not used here at all.
is_live() {
    local path="$1" variant
    while IFS= read -r variant; do
        [[ -n "${variant}" ]] || continue
        if [[ -n "${LIVE_CWDS}" ]] && grep -qF -- "${variant}" <<<"${LIVE_CWDS}"; then
            return 0
        fi
        if grep -qF -- "${variant}" <<<"${ACTIVE_ARGS}"; then
            return 0
        fi
    done < <(path_variants "${path}")
    return 1
}

# The primary worktree owns the common git dir; every linked worktree points at
# a subdirectory of it. Comparing against the common dir identifies the primary
# without depending on path conventions.
COMMON_DIR="$(git -C "${REPO}" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"

removable=(); branches=(); dirty=(); unmerged=(); busy=(); active=()

total=0
while IFS=$'\t' read -r path head branch; do
    [[ -n "${path}" && -d "${path}" ]] || continue
    total=$((total + 1))
    [[ -n "${COMMON_DIR}" && "${COMMON_DIR}" == "${path}/.git" ]] && continue
    [[ "$(git -C "${path}" rev-parse --is-bare-repository 2>/dev/null)" == "true" ]] && continue

    if [[ -n "$(git -C "${path}" status --porcelain 2>/dev/null | head -1)" ]]; then
        dirty+=("${path}"); continue
    fi
    if ! git -C "${REPO}" merge-base --is-ancestor "${head}" "${MAIN}" 2>/dev/null; then
        unmerged+=("${path}"); continue
    fi
    if [[ "${branch}" != "(detached)" ]] && \
       [[ "$(git -C "${REPO}" config --get "branch.${branch}.pulpworktreeStatus" 2>/dev/null)" == "active" ]]; then
        active+=("${path}"); continue
    fi
    if is_live "${path}"; then busy+=("${path}"); continue; fi
    removable+=("${path}"); branches+=("${branch}")
done < <(git -C "${REPO}" worktree list --porcelain | awk '
    /^worktree /{p=substr($0,10)}
    /^HEAD /{h=substr($0,6)}
    /^branch /{b=substr($0,8); sub(/^refs\/heads\//,"",b); print p"\t"h"\t"b; p=""; h=""}
    /^detached$/{print p"\t"h"\t(detached)"; p=""; h=""}')

if (( JSON )); then
    printf '{"repo":"%s","baseline":"%s","registered":%d,"removable":%d,"dirty":%d,"unmerged":%d,"busy":%d,"active":%d,"applied":%s}\n' \
        "${REPO}" "${MAIN}" "${total}" "${#removable[@]}" "${#dirty[@]}" "${#unmerged[@]}" \
        "${#busy[@]}" "${#active[@]}" "$( ((APPLY)) && echo true || echo false )"
    exit 0
fi

echo "clean_worktrees: ${REPO}"
echo "  baseline origin/main ${MAIN}"
echo "  registered ${total} | removable ${#removable[@]} | kept: dirty ${#dirty[@]}, unmerged ${#unmerged[@]}, busy ${#busy[@]}, active ${#active[@]}"
echo

# The at-risk set is reported FIRST. It holds content recoverable from no ref,
# and burying it under a removal log is how it stays invisible until a volume
# fills and someone deletes a tree in a hurry.
if (( ${#dirty[@]} + ${#unmerged[@]} > 0 )); then
    echo "AT RISK — kept; this content is in no ref:"
    for p in ${dirty[@]+"${dirty[@]}"}; do
        printf '  uncommitted %5s files  %s\n' \
            "$(git -C "${p}" status --porcelain 2>/dev/null | wc -l | tr -d ' ')" "${p}"
    done
    for p in ${unmerged[@]+"${unmerged[@]}"}; do
        printf '  unpushed    %5s commits %s\n' \
            "$(git -C "${p}" rev-list --count "${MAIN}"..HEAD 2>/dev/null || echo '?')" "${p}"
    done
    echo
fi
if (( ${#busy[@]} > 0 )); then
    echo "BUSY — kept; a live process is using them:"
    printf '  %s\n' "${busy[@]}"
    echo
fi
if (( ${#active[@]} > 0 )); then
    echo "ACTIVE — kept; lineage records them as active work:"
    printf '  %s\n' "${active[@]}"
    echo
fi

if (( ${#removable[@]} == 0 )); then
    echo "Nothing to remove."
    exit 0
fi

if (( ! APPLY )); then
    echo "REMOVABLE — clean, contained in origin/main, idle (dry-run; pass --yes):"
    printf '  %s\n' "${removable[@]}"
    exit 0
fi

echo "Removing ${#removable[@]}:"
removed=0
refused=0
for i in "${!removable[@]}"; do
    p="${removable[$i]}"
    b="${branches[$i]}"
    [[ -d "${p}" ]] || continue
    # Re-check at the moment of removal. The scan is a snapshot, and a long
    # sweep gives an agent time to start work in a tree classified idle.
    if [[ -n "$(git -C "${p}" status --porcelain 2>/dev/null | head -1)" ]] || is_live "${p}"; then
        echo "  skip (changed since scan)  ${p}"
        refused=$((refused + 1))
        continue
    fi
    # Record the disposition before removing: the lineage record is branch-local
    # and outlives the directory, so this is the last moment it can be written.
    if [[ "${b}" != "(detached)" && -x "${SCRIPT_DIR}/worktree_lineage.sh" ]]; then
        ( cd "${p}" && "${SCRIPT_DIR}/worktree_lineage.sh" mark --status merged \
            --note "clean_worktrees: HEAD contained in origin/main ${MAIN}" ) >/dev/null 2>&1 || true
    fi
    # No --force: git's own dirty check is an independent second gate.
    if git -C "${REPO}" worktree remove "${p}" 2>/dev/null; then
        echo "  removed  ${p}"
        removed=$((removed + 1))
    else
        echo "  REFUSED  ${p}"
        refused=$((refused + 1))
    fi
done
echo
echo "removed ${removed}, refused ${refused}"
