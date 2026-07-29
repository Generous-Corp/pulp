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
# gates.sh honours PYTHON; honour it here too so `PYTHON=python3.12 …` means
# one interpreter for the whole run rather than two disagreeing about the version.
PYTHON="${PYTHON:-python3}"
export PYTHON
TARGETS=("$@")

fail=0
declare -a NOTES=()  # lines for the contributor's "what I could not do"

say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; fail=1; }
note() { printf '  \033[33mwarn\033[0m  %s\n' "$*"; }
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
    lines=$(wc -l < "$f" 2>/dev/null | tr -d " " || echo 0)
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
# A few gates need Python 3.11+ (tomllib, unittest's enterContext). macOS ships
# 3.9, so on a stock Mac those fail for reasons unrelated to the change.
#
# But an old interpreter must never launder a REAL failure. Partition the
# reported problems: anything not on the known version-sensitive list is a
# genuine defect and fails, whatever Python is installed. Only a run whose
# problems are ALL version artefacts is downgraded to "inconclusive".
PY_OK=0
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)' 2>/dev/null && PY_OK=1

# Gates known to misreport on Python < 3.11. Keep this list narrow — a gate
# added here stops being enforced for every contributor on a stock Mac.
VERSION_SENSITIVE='deps-audit self-tests|codecov-config|scheduled_workflow_fork_guard|tomllib|enterContext'

if [ ! -x tools/scripts/gates.sh ]; then
    skip "tools/scripts/gates.sh not present"
elif PULP_SKIP_DIFF_COVER=1 tools/scripts/gates.sh "$BASE" >/tmp/contrib-gates.log 2>&1; then
    ok "gates.sh clean"
else
    grep -iE "failing|✗|FAILED|NOT updated" /tmp/contrib-gates.log \
        | grep -v "one or more gates failed" > /tmp/contrib-gates-problems.txt 2>/dev/null
    grep -vE "$VERSION_SENSITIVE" /tmp/contrib-gates-problems.txt \
        > /tmp/contrib-gates-real.txt 2>/dev/null
    if [ -s /tmp/contrib-gates-real.txt ]; then
        bad "gates.sh reported problems — see /tmp/contrib-gates.log"
        head -8 /tmp/contrib-gates-real.txt | sed 's/^/        /'
        if [ "$PY_OK" -eq 0 ] && [ -s /tmp/contrib-gates-problems.txt ]; then
            echo "        (further problems were suppressed as Python-version artefacts)"
        fi
        # Path-keyed gates fire on files you barely touched. The repo's sanctioned
        # answer is a trailer stating why, not contorting the change to satisfy a
        # match — without this line the contributor is simply stuck at "Not ready".
        if grep -qiE "compat|skill-sync|SKILL.md|config-doc" /tmp/contrib-gates-real.txt; then
            echo ""
            echo "        Some of these are PATH-keyed and may not be meant for your change."
            echo "        Either satisfy them, or state why on the tip commit, e.g.:"
            echo "            Skill-Update: skip skill=<name> reason=\"...\""
            echo "            Config-Doc:   skip reason=\"...\""
            echo "        Put the reason in the handoff too — the maintainer decides."
        fi
    elif [ "$PY_OK" -eq 0 ]; then
        skip "gates.sh inconclusive: $PYTHON is $("$PYTHON" -V 2>&1 | awk '{print $2}'), and every"
        echo "        reported problem is a known Python 3.11+ artefact:"
        head -4 /tmp/contrib-gates-problems.txt | sed 's/^/          /'
        echo "        Nothing here is attributable to your change, but install 3.11+ for a"
        echo "        definitive answer."
    else
        bad "gates.sh failed — see /tmp/contrib-gates.log"
        head -8 /tmp/contrib-gates-problems.txt | sed 's/^/        /'
    fi
fi

# ── 5. Diff coverage ───────────────────────────────────────────────────────
# The real gate is 75% ON THE DIFF (tools/scripts/coverage_config.json), not a
# whole-repo percentage. Optional here: it needs a coverage build plus
# diff-cover, and a contributor missing either should still be able to hand off.
say "5. Tests"
# Checking that a test FILE changed is not the same as running it. Without this
# the script could print "Ready to hand off" for a diff whose tests were never
# built, which is the exact claim the repo says is not a test.
if [ "${#TARGETS[@]:-0}" -eq 0 ]; then
    skip "no test target given — this check ran NO tests; build and run yours, or say so"
elif [ ! -d build-tests ] && [ ! -d build ]; then
    skip "no build dir (build-tests/ or build/) — tests not built or run by this check"
else
    bdir=build-tests; [ -d "$bdir" ] || bdir=build
    echo "  building + running: ${TARGETS[*]} (in $bdir)"
    if cmake --build "$bdir" -j"$(( $(sysctl -n hw.ncpu 2>/dev/null || echo 4) / 2 ))" \
            --target ${TARGETS[@]+"${TARGETS[@]}"} >/tmp/contrib-build.log 2>&1; then
        if ctest --test-dir "$bdir" --output-on-failure \
                 -R "$(printf '%s|' ${TARGETS[@]+"${TARGETS[@]}"} | sed 's/|$//')" \
                 >/tmp/contrib-ctest.log 2>&1; then
            ok "$(grep -oE '[0-9]+% tests passed[^)]*\)' /tmp/contrib-ctest.log | tail -1)"
        else
            bad "tests failed — see /tmp/contrib-ctest.log"
            grep -A6 "The following tests FAILED" /tmp/contrib-ctest.log | head -8 | sed 's/^/        /'
        fi
    else
        bad "build failed for ${TARGETS[*]} — see /tmp/contrib-build.log"
        grep -iE "error:" /tmp/contrib-build.log | head -4 | sed 's/^/        /'
    fi
fi

say "6. Diff coverage (target: 75% of changed lines)"
# Only C/C++ sources produce coverage data. Running the coverage build for a
# docs-, script-, or workflow-only diff costs many minutes and measures nothing —
# and a check that appears to hang is a check people learn to interrupt.
COVERABLE=0
for f in ${CHANGED[@]+"${CHANGED[@]}"}; do
    case "$f" in
        *.cpp|*.hpp|*.cc|*.h|*.mm|*.m) COVERABLE=1 ;;
    esac
done
if [ "$COVERABLE" -eq 0 ]; then
    ok "no C/C++ sources changed — coverage not applicable"
elif ! command -v diff-cover >/dev/null 2>&1 && ! "$PYTHON" -c "import diff_cover" >/dev/null 2>&1; then
    skip "diff-cover not installed — coverage not measured locally (maintainer CI enforces it)"
elif [ ! -x tools/scripts/local_diff_cover.sh ]; then
    skip "tools/scripts/local_diff_cover.sh not present"
elif [ "${#TARGETS[@]:-0}" -eq 0 ] && [ "${CONTRIB_FULL_COVERAGE:-0}" != "1" ]; then
    # A whole-tree coverage build is 30+ minutes. Make that opt-in rather than
    # the default a contributor stumbles into with no warning.
    skip "coverage skipped: pass your test target(s) to measure it, e.g."
    printf '        tools/scripts/contributor_check.sh pulp-test-<subsystem>\n'
    printf '        (or CONTRIB_FULL_COVERAGE=1 for the full ~30min tree build)\n'
else
    echo "  running coverage build for: ${TARGETS[*]:-whole tree} (several minutes)…"
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
