#!/usr/bin/env bash
#
# clean_build_cov.sh — prune orphaned coverage build directories.
#
# local_diff_cover.sh (and shipyard's local validation, which runs it) creates a
# `build-cov/` directory per worktree at <worktree>/build-cov and never cleans
# it up. Across many worktrees these accumulate into hundreds of GB of
# regenerable coverage objects and fill the disk — which then fails the next
# coverage build with "No space left on device" (looks like a code failure, is
# not). This script reclaims them.
#
# Safe by construction:
#   - Only ever removes directories literally named `build-cov` / `build-coverage`
#     (coverage scratch; never a source tree or the primary `build/`).
#   - Idle-gated: skips a directory whose worktree has an active build process
#     (cmake / ctest / ninja / a clang compile) so an in-flight coverage run is
#     never corrupted.
#   - Dry-run by default; pass --yes to actually delete.
#
# Usage:
#   tools/scripts/clean_build_cov.sh                 # dry-run: list + total reclaimable
#   tools/scripts/clean_build_cov.sh --yes           # delete (idle-gated)
#   PULP_WORKTREES_ROOT=/path/to/code clean_build_cov.sh --yes
#
# Root: defaults to the parent directory of this repo, so sibling worktrees
# (the common `git worktree add ../pulp-<topic>` layout) are all scanned. Set
# PULP_WORKTREES_ROOT to override.
set -euo pipefail

APPLY=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) APPLY=1 ;;
        -h|--help)
            sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "clean_build_cov: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOT="${PULP_WORKTREES_ROOT:-$(dirname "${REPO_ROOT}")}"

if [[ ! -d "${ROOT}" ]]; then
    echo "clean_build_cov: scan root not found: ${ROOT}" >&2
    exit 2
fi

# Worktree roots with an active build — we skip their coverage dirs so a live
# coverage run isn't corrupted mid-flight. A build process's command line
# carries the absolute path of the dir it builds (e.g. -B <wt>/build-cov,
# --test-dir <wt>/build-cov, or an -o <wt>/build-cov/... object path), so we
# match coverage dirs against the active command lines directly.
active_cmolines="$(pgrep -fl 'cmake|ctest|ninja|clang|cc1|c\+\+' 2>/dev/null || true)"

is_active() {
    # $1 = absolute coverage dir. Active if any build command line mentions it.
    local dir="$1"
    [[ -n "${active_cmolines}" ]] && grep -qF "${dir}" <<<"${active_cmolines}"
}

dir_size_kb() { du -sk "$1" 2>/dev/null | awk '{print $1}'; }

total_kb=0
deleted=0
skipped=0
found=0

# -prune so we don't descend into a build-cov looking for nested ones.
while IFS= read -r dir; do
    [[ -z "${dir}" ]] && continue
    found=$((found + 1))
    size_kb="$(dir_size_kb "${dir}")"; size_kb="${size_kb:-0}"
    human="$(du -sh "${dir}" 2>/dev/null | awk '{print $1}')"
    if is_active "${dir}"; then
        echo "  SKIP (active build) ${human}	${dir}"
        skipped=$((skipped + 1))
        continue
    fi
    total_kb=$((total_kb + size_kb))
    if [[ "${APPLY}" -eq 1 ]]; then
        rm -rf "${dir}" && { echo "  removed ${human}	${dir}"; deleted=$((deleted + 1)); }
    else
        echo "  would remove ${human}	${dir}"
    fi
done < <(find "${ROOT}" -maxdepth 2 \( -name build-cov -o -name build-coverage \) -type d -prune 2>/dev/null)

total_gb="$(awk -v kb="${total_kb}" 'BEGIN{printf "%.1f", kb/1024/1024}')"
echo
if [[ "${APPLY}" -eq 1 ]]; then
    echo "clean_build_cov: removed ${deleted} dir(s), ~${total_gb} GB reclaimed; skipped ${skipped} active."
else
    echo "clean_build_cov: ${found} coverage dir(s) under ${ROOT}; ~${total_gb} GB reclaimable (${skipped} active, skipped)."
    echo "clean_build_cov: re-run with --yes to delete."
fi
