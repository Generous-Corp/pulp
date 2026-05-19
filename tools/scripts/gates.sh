#!/usr/bin/env bash
# gates.sh — fast pre-push safety net for the cheap CI gates.
#
# Naming: aligned with Shipyard's planned `shipyard gates` subcommand
# (see planning/2026-05-19-shipyard-preflight-upstream-proposal.md).
# Avoids collision with Shipyard's existing `preflight` namespace,
# which is exclusively about SSH backend reachability checks.
#
# Runs JUST the sub-second gates that `.githooks/pre-push` runs in
# `--mode=report`:
#   - skill-sync (catches missing SKILL.md updates for mapped paths)
#   - version-bump (catches feat:/fix: PRs without a chore: bump versions commit)
#   - compat-sync (when compat.json is touched, requires matching test coverage)
#   - deps-audit (catches DEPENDENCIES.md / NOTICE.md drift)
#
# Does NOT run:
#   - local diff-coverage (slow — builds the cov target, hits ring crate
#     compile failures on macOS, eats ~10 minutes). Use
#     `tools/scripts/local_diff_cover.sh` directly when you need it.
#   - build / test targets (the agent/developer should already be running
#     these in their dev loop).
#
# Why this script exists (Phase 0b PR #2374 lesson): the pre-push hook
# already runs all of these gates, but the bypass env vars are easy to
# misuse — `PULP_SKIP_PREPUSH=1` (nuclear) skips skill-sync along with
# diff-cover, even though skill-sync is fast and catches real problems.
# Running gates.sh BEFORE git push gives you the skill-sync /
# version-bump signal without needing to push (which then triggers a
# 15-30 min CI roundtrip if a gate fails). Surgical alternative to
# `PULP_SKIP_PREPUSH=1`.
#
# Usage:
#     ./tools/scripts/gates.sh                     # uses origin/main as base
#     ./tools/scripts/gates.sh main                # custom base
#     PULP_GATES_BASE=develop ./tools/scripts/gates.sh
#
# Exit codes:
#   0 — all gates pass
#   1 — one or more gates failed; output above shows which
#   2 — internal error (script couldn't find the gate tools)

set -u

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$ROOT" ]; then
    echo "gates.sh: not in a git working tree" >&2
    exit 2
fi
cd "$ROOT" || exit 2

PYTHON="${PYTHON:-python3}"
BASE="${1:-${PULP_GATES_BASE:-origin/main}}"

VBC="$ROOT/tools/scripts/version_bump_check.py"
SSC="$ROOT/tools/scripts/skill_sync_check.py"
CSC="$ROOT/tools/scripts/compat_sync_check.py"
CFG="$ROOT/tools/scripts/versioning.json"
DEPS_AUDIT="$ROOT/tools/deps/audit.py"

if [ ! -f "$VBC" ] || [ ! -f "$SSC" ] || [ ! -f "$CFG" ]; then
    echo "gates.sh: gate scripts not found (expected at tools/scripts/)" >&2
    exit 2
fi

# PR title detection for --require-bump-for-fix-feat. Mirrors how the CI
# workflow sets GITHUB_PR_TITLE. Local-only nuance: if the branch has
# NO commits ahead of base, the tip commit IS the base commit, and using
# its subject would falsely trip the gate when the base happens to be a
# fix:/feat: merge. So we only synthesize a title from the FIRST commit
# ON THIS BRANCH (which is what the PR title will most likely match).
# If no commits are ahead, leave the title empty — the gate is then a
# no-op for the local run, and the pre-push hook / CI will still enforce
# it from the real PR title once you actually open one.
if [ -z "${GITHUB_PR_TITLE:-}" ]; then
    first_branch_commit="$(git rev-list --reverse "$BASE..HEAD" 2>/dev/null | head -1)"
    if [ -n "$first_branch_commit" ]; then
        GITHUB_PR_TITLE="$(git log -1 --pretty=%s "$first_branch_commit" 2>/dev/null || echo "")"
    else
        GITHUB_PR_TITLE=""
    fi
    export GITHUB_PR_TITLE
fi

fail=0

echo "gates: base = $BASE" >&2

# ── 1. skill-sync ──────────────────────────────────────────────────────────
echo "" >&2
echo "▸ skill-sync check" >&2
if ! "$PYTHON" "$SSC" --base "$BASE" --config "$CFG" --mode=report; then
    fail=1
fi

# ── 2. version-bump ────────────────────────────────────────────────────────
echo "" >&2
echo "▸ version-bump check (fix:/feat: titles require a chore: bump versions commit)" >&2
if ! "$PYTHON" "$VBC" --base "$BASE" --config "$CFG" --mode=report \
        --require-bump-for-fix-feat; then
    fail=1
fi

# ── 3. compat-sync (optional — only if both files exist) ───────────────────
COMPAT_MAP="$ROOT/tools/scripts/compat_path_map.json"
if [ -f "$CSC" ] && [ -f "$COMPAT_MAP" ]; then
    echo "" >&2
    echo "▸ compat-sync check" >&2
    if ! "$PYTHON" "$CSC" --base "$BASE" --mode=report; then
        fail=1
    fi
fi

# ── 4. deps-audit ──────────────────────────────────────────────────────────
if [ -f "$DEPS_AUDIT" ]; then
    echo "" >&2
    echo "▸ deps-audit (attribution drift)" >&2
    if ! "$PYTHON" "$DEPS_AUDIT" --strict >/dev/null 2>&1; then
        echo "  deps-audit: attribution drift detected — run \`python3 tools/deps/audit.py --strict\` for details." >&2
        fail=1
    else
        echo "  deps-audit: ok" >&2
    fi
fi

# ── Summary ────────────────────────────────────────────────────────────────
echo "" >&2
if [ "$fail" -eq 0 ]; then
    echo "gates: ✓ all gates pass — safe to push." >&2
    echo "" >&2
    echo "  Diff-coverage NOT run (slow). When you want it:" >&2
    echo "    tools/scripts/local_diff_cover.sh [test_target ...]" >&2
    exit 0
else
    echo "gates: ✗ one or more gates failed (see above)." >&2
    echo "" >&2
    echo "  Fix the listed gate(s), then re-run gates.sh. After it goes" >&2
    echo "  green, the pre-push hook will pass and you avoid burning a" >&2
    echo "  15-30 min CI roundtrip on the same problem." >&2
    echo "" >&2
    echo "  If you genuinely need to push past a single gate (e.g., diff-cover" >&2
    echo "  build-cov hits the ring crate compile bug), use the SURGICAL bypass:" >&2
    echo "    PULP_DISABLE_PREPUSH_DIFF_COVER=1 git push    # diff-cover only" >&2
    echo "  NOT the nuclear bypass:" >&2
    echo "    PULP_SKIP_PREPUSH=1 git push                  # also skips fast checks (avoid)" >&2
    exit 1
fi
