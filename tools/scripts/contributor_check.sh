#!/usr/bin/env bash
# Contributor pre-handoff check — everything worth running on a plain Mac.
#
# Deliberately does NOT need Shipyard, Tart, a VM, self-hosted runners, or
# write access. Those are maintainer infrastructure; a contribution should not
# be gated on owning them.
#
# The design rule here is that a check you cannot run is REPORTED, not fatal.
# An outside contributor with a broken Homebrew Python should still be able to
# hand off good work — they just need to say what they could not verify. A
# script that hard-fails on a missing optional tool teaches people to bypass it,
# which is worse than not having it.
#
#   tools/scripts/contributor_check.sh              # diff vs origin/main
#   tools/scripts/contributor_check.sh <target>...  # build only these targets
#   BASE=origin/develop tools/scripts/contributor_check.sh
#
# Exit 0 — ready to hand off (warnings may still be present, read them).
# Exit 1 — something a maintainer would send back. Fix before handing off.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 1
BASE="${BASE:-origin/main}"
TARGETS=("$@")

fail=0
warn=0
declare -a NOTES=()  # lines for the contributor's "what I could not do"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; fail=1; }
note() { printf '  \033[33mwarn\033[0m  %s\n' "$*"; warn=1; }
skip() { printf '  \033[33mSKIP\033[0m  %s\n' "$*"; NOTES+=("$*"); }

say "Contributor check — base: $BASE"

if ! git rev-parse --verify "$BASE" >/dev/null 2>&1; then
    echo "  cannot resolve $BASE — run: git fetch origin main" >&2
    exit 1
fi
MERGE_BASE="$(git merge-base "$BASE" HEAD)"
# bash 3.2 (macOS default) has no `mapfile` — read into an array portably.
CHANGED=()
while IFS= read -r _line; do
    [ -n "$_line" ] && CHANGED+=("$_line")
done < <(git diff --name-only "$MERGE_BASE"..HEAD)
if [ "${#CHANGED[@]:-0}" -eq 0 ]; then
    echo "  no changes vs $BASE — nothing to check." >&2
    exit 0
fi
printf '  %d changed file(s)\n' "${#CHANGED[@]}"

# ── 1. Version files must not be touched ───────────────────────────────────
# version-at-land assigns versions after merge from the diff. A contributor
# bump only creates a conflict that makes the PR obsolete.
say "1. Version files (must be untouched)"
VERSION_HITS=()
for f in ${CHANGED[@]+"${CHANGED[@]}"}; do
    case "$f" in
        CMakeLists.txt|.claude-plugin/plugin.json|.claude-plugin/marketplace.json|CHANGELOG.md)
            VERSION_HITS+=("$f") ;;
    esac
done
if [ "${#VERSION_HITS[@]:-0}" -gt 0 ]; then
    if git diff "$MERGE_BASE"..HEAD -- ${VERSION_HITS[@]+"${VERSION_HITS[@]}"} | grep -qE '^\+.*(VERSION [0-9]|"version")'; then
        bad "version/changelog edits detected: ${VERSION_HITS[*]:-}"
        echo "        Pulp assigns versions post-merge (version-at-land). Revert these."
    else
        ok "touched but no version lines changed"
    fi
else
    ok "no version or changelog files touched"
fi

# ── 2. Tests ship with fixes ───────────────────────────────────────────────
say "2. Tests accompany source changes"
SRC_CHANGED=0; TEST_CHANGED=0
for f in ${CHANGED[@]+"${CHANGED[@]}"}; do
    case "$f" in
        test/*|*/test_*|*_test.*) TEST_CHANGED=1 ;;
        core/*|apple/*|ship/*|tools/cli/*) SRC_CHANGED=1 ;;
    esac
done
if [ "$SRC_CHANGED" -eq 1 ] && [ "$TEST_CHANGED" -eq 0 ]; then
    bad "source changed under core/ (or apple/, ship/, tools/cli/) with no test change"
    echo "        This repo requires a test in the same change. Add one, and confirm"
    echo "        it FAILS with your fix reverted."
elif [ "$SRC_CHANGED" -eq 1 ]; then
    ok "source and tests both changed"
    note "confirm each new test FAILS without your fix — revert, rebuild, observe, restore"
    warn=0  # the reminder above is guidance, not a defect
else
    ok "no shipped-source changes"
fi

# ── 3. Size / structure ────────────────────────────────────────────────────
# Not a hard gate: a large generated or data file is legitimate. It is a prompt
# to justify, because oversized files are the most common reason a contribution
# needs restructuring before it can land.
say "3. Size and structure"
BIG=0
for f in ${CHANGED[@]+"${CHANGED[@]}"}; do
    [ -f "$f" ] || continue
    case "$f" in *.json|*.md|*.txt|*.svg|*.lock) continue ;; esac
    lines=$(wc -l < "$f" 2>/dev/null || echo 0)
    if [ "${lines:-0}" -gt 1000 ]; then
        note "$f is ${lines} lines — over ~1000; be ready to justify or split"
        BIG=1
    fi
done
[ "$BIG" -eq 0 ] && ok "no touched file over ~1000 lines"

ADDED=$(git diff --numstat "$MERGE_BASE"..HEAD | awk '{a+=$1} END {print a+0}')
if [ "${ADDED:-0}" -gt 1500 ]; then
    note "+${ADDED} lines — consider splitting into independent patches by concern"
else
    ok "+${ADDED} lines added"
fi

# ── 4. Cheap repo gates ────────────────────────────────────────────────────
# gates.sh is sub-second and offline. It is the same set the maintainer's
# pre-push hook runs, minus the heavy diff-coverage build.
say "4. Repo gates (fast, offline)"
if [ -x tools/scripts/gates.sh ]; then
    if PULP_SKIP_DIFF_COVER=1 tools/scripts/gates.sh "$BASE" >/tmp/contrib-gates.log 2>&1; then
        ok "gates.sh clean"
    else
        if grep -q "ModuleNotFoundError: No module named 'tomllib'" /tmp/contrib-gates.log; then
            skip "gates.sh: needs Python 3.11+ (tomllib) — your python3 is older; maintainer CI covers it"
        else
            bad "gates.sh reported problems — see /tmp/contrib-gates.log"
            grep -iE "✗|FAILED|NOT updated" /tmp/contrib-gates.log | head -6 | sed 's/^/        /'
        fi
    fi
else
    skip "tools/scripts/gates.sh not present"
fi

# ── 5. Diff coverage ───────────────────────────────────────────────────────
# The real gate is 75% ON THE DIFF (tools/scripts/coverage_config.json), not a
# whole-repo percentage. Optional here: it needs a coverage build plus
# diff-cover, and a contributor missing either should still be able to hand off.
say "5. Diff coverage (target: 75% of changed lines)"
if ! command -v diff-cover >/dev/null 2>&1 && ! python3 -c "import diff_cover" >/dev/null 2>&1; then
    skip "diff-cover not installed — coverage not measured locally (maintainer CI enforces it)"
elif [ ! -x tools/scripts/local_diff_cover.sh ]; then
    skip "tools/scripts/local_diff_cover.sh not present"
else
    echo "  running coverage build (slow — several minutes)…"
    if tools/scripts/local_diff_cover.sh ${TARGETS[@]+"${TARGETS[@]}"} >/tmp/contrib-cover.log 2>&1; then
        ok "diff coverage at or above threshold"
    else
        bad "diff coverage below threshold — see /tmp/contrib-cover.log"
        grep -iE "coverage|%" /tmp/contrib-cover.log | tail -4 | sed 's/^/        /'
    fi
fi

# ── Summary ────────────────────────────────────────────────────────────────
say "Summary"
if [ "${#NOTES[@]:-0}" -gt 0 ]; then
    echo "  Copy these into your handoff under \"What I could not do\":"
    for n in ${NOTES[@]+"${NOTES[@]}"}; do echo "    - $n"; done
    echo ""
fi

if [ "$fail" -ne 0 ]; then
    echo "  Not ready — fix the FAIL items above, then re-run."
    exit 1
fi
echo "  Ready to hand off."
echo "  Next: write the handoff (base commit, suggested patch split, what each"
echo "  change fixes, verification, what you could not do). See the 'contribute'"
echo "  skill for the format."
exit 0
