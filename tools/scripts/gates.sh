#!/usr/bin/env bash
# gates.sh — fast pre-push safety net for the cheap CI gates.
#
# Naming: aligned with Shipyard's planned `shipyard gates` subcommand.
# Avoids collision with Shipyard's existing `preflight` namespace,
# which is exclusively about SSH backend reachability checks.
#
# Runs JUST the sub-second gates that `.githooks/pre-push` runs in
# `--mode=report`:
#   - skill-sync (catches missing SKILL.md updates for mapped paths)
#   - version-bump (catches feat:/fix: PRs without a chore: bump versions commit)
#   - compat-sync (mapped compat paths require matrix/docs/tests or a skip trailer)
#   - compat-aggregate (compat.json stays byte-identical to compat/ parts)
#   - node-ABI (Processor/PluginSlot virtual methods are append-only)
#   - hotspot-size (known refactor hotspots must not exceed frozen LOC baselines)
#   - deps-audit (catches DEPENDENCIES.md / NOTICE.md drift)
#   - codecov-config (codecov.yml flags/components mirror the live core/* tree
#     with no double-counts, and its ignore list mirrors diff_cover_excludes)
#
# Does NOT run:
#   - local diff-coverage (slow — builds the cov target, hits ring crate
#     compile failures on macOS, eats ~10 minutes). Use
#     `tools/scripts/local_diff_cover.sh` directly when you need it.
#   - build / test targets (the agent/developer should already be running
#     these in their dev loop).
#
# Why this script exists: the pre-push hook already runs all of these
# gates, but the bypass env vars are easy to
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
COMPAT_AGG="$ROOT/tools/scripts/compat_aggregate.py"
NAG="$ROOT/tools/scripts/node_abi_gate.py"
HSG="$ROOT/tools/scripts/hotspot_size_guard.py"
HSG_CFG="$ROOT/tools/scripts/hotspot_size_guard.json"
CFG="$ROOT/tools/scripts/versioning.json"
DEPS_AUDIT="$ROOT/tools/deps/audit.py"
IMPORT_PROV="$ROOT/tools/scripts/check_import_provenance.py"
CODECOV_CFG_TEST="$ROOT/tools/scripts/test_codecov_config.py"
CODECOV_COMP_TEST="$ROOT/tools/scripts/test_codecov_components.py"
TERMS_LINT="$ROOT/tools/scripts/processing_model_terms_lint.py"
SINGLE_BACKEND_GUARD="$ROOT/tools/scripts/single_backend_guard.py"

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

# ── 0. host-vitals (ADVISORY) ──────────────────────────────────────────────
# A pushing developer/agent on a self-hosted CI host is often the SAME machine
# that runs the required macos gate. If that host is shedding memory (jetsam) or
# at critical pressure, piling a foreground build/ship on top risks an unclean
# reboot that kills the in-flight CI job. This banner surfaces that BEFORE the
# push so you can ship via GitHub-native auto-merge (survives a restart) instead
# of a foreground watch. Advisory only — it never changes the exit code.
HOST_VITALS="$ROOT/tools/scripts/host_vitals.sh"
if [ -x "$HOST_VITALS" ]; then
    echo "" >&2
    echo "▸ host-vitals (advisory)" >&2
    vitals_out="$("$HOST_VITALS" 2>/dev/null)"; vitals_code=$?
    echo "  $vitals_out" >&2
    if [ "$vitals_code" -ge 20 ]; then
        echo "  ⚠︎ host is CRITICAL — prefer 'shipyard pr' / GitHub auto-merge over a" >&2
        echo "    foreground watch, and shed idle load (RepoPrompt/Figma/MCP) before builds." >&2
    fi
fi

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
    if ! "$PYTHON" "$CSC" --base "$BASE" --mode=report --enforce; then
        fail=1
    fi
fi

# ── 4. compat aggregate/part consistency ───────────────────────────────────
if [ -f "$COMPAT_AGG" ] && [ -f "$ROOT/compat.json" ] && [ -d "$ROOT/compat" ]; then
    echo "" >&2
    echo "▸ compat aggregate check" >&2
    if ! "$PYTHON" "$COMPAT_AGG" check; then
        fail=1
    fi
fi

# ── 5. node ABI virtual-order gate ─────────────────────────────────────────
if [ -f "$NAG" ]; then
    echo "" >&2
    echo "▸ node-ABI virtual-order check" >&2
    if ! "$PYTHON" "$NAG" --base "$BASE" --mode=report; then
        fail=1
    fi
fi

# ── 6. hotspot-size guard ──────────────────────────────────────────────────
if [ -f "$HSG" ] && [ -f "$HSG_CFG" ]; then
    echo "" >&2
    echo "▸ hotspot-size guard" >&2
    if ! "$PYTHON" "$HSG" --base "$BASE" --config "$HSG_CFG" --mode=report \
            --require-ceiling-reduction; then
        fail=1
    fi
fi

# ── 7. deps-audit ──────────────────────────────────────────────────────────
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

# ── 8. codecov-config drift ────────────────────────────────────────────────
# Global invariant — runs unconditionally (not diff-scoped), because the
# usual way codecov.yml goes stale is a NEW core/<sub>/ subsystem landing
# with no codecov.yml edit at all (graph/scene drifted onto main exactly
# this way). The contract tests assert codecov.yml's flags + components
# mirror the live core/* tree with no double-counts, and that its `ignore:`
# list mirrors coverage_config.json's diff_cover_excludes. Sub-second.
# Needs PyYAML; skip cleanly if the local interpreter lacks it (CI's
# codecov-config-validation job is the authoritative gate).
if [ -f "$CODECOV_CFG_TEST" ] && [ -f "$CODECOV_COMP_TEST" ]; then
    echo "" >&2
    echo "▸ codecov-config drift (flags/components mirror core/*; ignore mirrors diff_cover_excludes)" >&2
    if ! "$PYTHON" -c "import yaml" >/dev/null 2>&1; then
        echo "  codecov-config: skipped (PyYAML not installed locally; CI enforces it)." >&2
    elif ! "$PYTHON" "$CODECOV_CFG_TEST" >/dev/null 2>&1 \
            || ! "$PYTHON" "$CODECOV_COMP_TEST" >/dev/null 2>&1; then
        echo "  codecov-config: drift detected — codecov.yml is stale. Run:" >&2
        echo "    python3 tools/scripts/test_codecov_config.py" >&2
        echo "    python3 tools/scripts/test_codecov_components.py" >&2
        fail=1
    else
        echo "  codecov-config: ok" >&2
    fi
fi

# ── 9. SignalGraph single-backend governance ──────────────────────────────
# Global structural invariants (not diff-scoped): one routing backend
# (GraphRuntimeExecutor), no second authoring surface, no unsanctioned
# generated-DSP ABI entrypoint, and the differential parity safety-net stays
# wired into the build. Plus the reserved-terminology lint. Sub-second; these
# protect the convergence work from silently regrowing a second runtime.
if [ -f "$TERMS_LINT" ] && [ -f "$SINGLE_BACKEND_GUARD" ]; then
    echo "" >&2
    echo "▸ SignalGraph governance (single-backend + reserved terminology)" >&2
    if ! "$PYTHON" "$TERMS_LINT" >/dev/null; then
        echo "  terminology lint: reserved-phrase violation — run \`python3 tools/scripts/processing_model_terms_lint.py\` for details." >&2
        fail=1
    fi
    if ! "$PYTHON" "$SINGLE_BACKEND_GUARD"; then
        fail=1
    fi
fi

# ── 10. import-provenance (opt-in) ─────────────────────────────────────────
# Audits that any emitted/migrated project carries a well-formed clean-room
# provenance marker. No-op for normal Pulp-repo pushes; set
# PULP_IMPORT_PROVENANCE_DIRS (space-separated project dirs) on a PR that lands
# a migrated project to enforce it here.
if [ -f "$IMPORT_PROV" ] && [ -n "${PULP_IMPORT_PROVENANCE_DIRS:-}" ]; then
    echo "" >&2
    echo "▸ import-provenance check" >&2
    # shellcheck disable=SC2086
    if ! "$PYTHON" "$IMPORT_PROV" $PULP_IMPORT_PROVENANCE_DIRS; then
        fail=1
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
