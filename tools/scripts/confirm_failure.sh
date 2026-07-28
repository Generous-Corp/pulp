#!/usr/bin/env bash
#
# Confirm a test actually fails without the fix it claims to cover.
#
# A passing test proves nothing on its own — it may assert the same thing the
# code assumes, or never reach the code at all. The check is to break the fix,
# watch the test fail, put it back, and watch it pass. Done by hand that loop
# has a silent failure mode which has produced wrong conclusions in both
# directions:
#
#   The build does not pick up the edit. Restoring a file and rebuilding within
#   the same filesystem second leaves make comparing equal mtimes and deciding
#   the object is current, so the binary still holds the OLD code. Touching the
#   source does not reliably fix this — the touch lands in the same second. A
#   stale binary during the break step makes the control falsely PASS, which
#   reads as "my test does not cover this" and sends you off to rewrite a test
#   that was fine. A stale binary during the restore step reports a failure that
#   is not real.
#
# So this never trusts the timestamps. It deletes the object files for the
# edited source, and then VERIFIES the compiler actually rebuilt it by looking
# for the file in the build output. If the recompile is not observed, it stops
# and says so rather than reporting a result it cannot stand behind.
#
# Usage:
#   confirm_failure.sh --file <path> --break <sed/perl cmd> \
#                      --build-dir <dir> --target <cmake target> \
#                      --test <command> [--jobs N]
#
# The break command is run with the file path appended, e.g.
#   --break "perl -0pi -e 's/policy.priority/0/'"
#
# Exit codes:
#   0  CONFIRMED  — passed before, failed while broken, passes again after
#   1  NOT CONFIRMED — the test passed while the fix was broken; it does not
#      cover the change
#   2  INCONCLUSIVE — the loop could not be run honestly (dirty file, build
#      never picked up the edit, test already failing, restore failed)

set -uo pipefail

FILE=""
BREAK_CMD=""
BUILD_DIR=""
TARGET=""
TEST_CMD=""
JOBS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --file)      FILE="$2"; shift 2 ;;
        --break)     BREAK_CMD="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --target)    TARGET="$2"; shift 2 ;;
        --test)      TEST_CMD="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        -h|--help)   sed -n '3,40p' "$0"; exit 0 ;;
        *) echo "confirm-failure: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

missing=""
[ -n "$FILE" ]      || missing="$missing --file"
[ -n "$BREAK_CMD" ] || missing="$missing --break"
[ -n "$BUILD_DIR" ] || missing="$missing --build-dir"
[ -n "$TARGET" ]    || missing="$missing --target"
[ -n "$TEST_CMD" ]  || missing="$missing --test"
if [ -n "$missing" ]; then
    echo "confirm-failure: required argument(s):$missing" >&2
    exit 2
fi

if [ -z "$JOBS" ]; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

say() { printf 'confirm-failure: %s\n' "$1"; }
die_inconclusive() { say "INCONCLUSIVE — $1"; exit 2; }

[ -f "$FILE" ] || die_inconclusive "no such file: $FILE"

# Restoring through git is exact, where a hand-kept .bak can silently drift.
if ! git ls-files --error-unmatch "$FILE" >/dev/null 2>&1; then
    die_inconclusive "$FILE is not tracked by git, so it cannot be restored exactly"
fi
if ! git diff --quiet -- "$FILE"; then
    die_inconclusive "$FILE has uncommitted changes; commit or stash them so the
    restore is exact"
fi

BUILD_LOG="$(mktemp)"
trap 'rm -f "$BUILD_LOG"' EXIT

# Delete the objects built from this source so the rebuild cannot be skipped on
# a timestamp comparison. Header-only edits have no object of their own, so the
# recompile is verified by observing that SOMETHING was rebuilt.
BASE="$(basename "$FILE")"
IS_HEADER=0
case "$BASE" in *.h|*.hpp|*.hh|*.hxx|*.inc) IS_HEADER=1 ;; esac

# The binary the test runs. Verifying an OBJECT recompiled is not enough: the
# archive can relink while the executable's link is skipped, because make
# compares the archive's mtime against the executable's and they can land in the
# same second. That leaves the test running old code with a fresh object beside
# it — the same stale-artifact trap this script exists to catch, one level up.
TEST_BINARY="$(printf '%s' "$TEST_CMD" | awk '{print $1}')"

binary_fingerprint() {
    [ -f "$TEST_BINARY" ] || { echo "absent"; return; }
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$TEST_BINARY" | awk '{print $1}'
    else
        cksum "$TEST_BINARY" | awk '{print $1 $2}'
    fi
}

# Push the edited file's mtime a few seconds into the future.
#
# This is the whole reason the hand-run loop is unreliable. macOS ships GNU make
# 3.81, which compares mtimes at ONE-SECOND granularity, and a build rebuilds a
# source only when it is strictly NEWER than its object. An edit landing in the
# same second as the previous build therefore looks current: make prints "Built
# target" and compiles nothing, and the test runs the old code. Deleting objects
# does not save you — the target-level check short-circuits before the missing
# object is ever noticed. `touch` to NOW does not save you either, for exactly
# the same reason.
bump_mtime() {
    local stamp
    stamp="$(date -v+5S '+%Y%m%d%H%M.%S' 2>/dev/null ||
             date -d '+5 seconds' '+%Y%m%d%H%M.%S' 2>/dev/null || true)"
    if [ -n "$stamp" ]; then
        touch -t "$stamp" "$FILE" 2>/dev/null || touch "$FILE"
    else
        touch "$FILE"
    fi
}

# Remove every artifact between the edited source and the test binary.
#
# Bumping the source's mtime is necessary but NOT sufficient: it forces the
# object to recompile, and then the fresh object ties its archive's mtime, and
# the archive ties the executable's, so the cascade stalls one level up and the
# test runs old code with a fresh object sitting beside it. Nothing survives a
# comparison that has no artifact to compare against, so the archives and the
# binary go too.
#
# The cost is relink time on the next build, not recompile time — deliberate,
# and cheap against reporting a verdict about code the test never ran.
invalidate() {
    rm -f "$TEST_BINARY" 2>/dev/null || true
    find "$BUILD_DIR" \( -name '*.a' -o -name '*.dylib' -o -name '*.so' \) \
        -delete 2>/dev/null || true
    if [ "$IS_HEADER" -eq 1 ]; then
        # A header's dependents are unknown here, so every object goes.
        find "$BUILD_DIR" -name '*.o' -delete 2>/dev/null || true
    else
        find "$BUILD_DIR" -name "${BASE}.o" -delete 2>/dev/null || true
    fi
}

# Build, and require evidence that the edited file was actually compiled. This
# is the check the hand-run loop does not have.
build_and_verify_recompile() {
    local phase="$1"
    if ! cmake --build "$BUILD_DIR" --target "$TARGET" -j "$JOBS" > "$BUILD_LOG" 2>&1; then
        if grep -qE '\berror:' "$BUILD_LOG"; then
            # A break that does not compile still proves the test depends on the
            # code, but it is not the same evidence, so say which one happened.
            say "$phase: the edit does not compile"
            return 1
        fi
        say "$phase: build failed for a reason other than the edit"
        sed -n '$p' "$BUILD_LOG" >&2
        return 2
    fi
    if [ "$IS_HEADER" -eq 1 ]; then
        grep -qE 'Building [A-Z]+ object' "$BUILD_LOG" && return 0
    else
        grep -qF "${BASE}.o" "$BUILD_LOG" && return 0
    fi
    say "$phase: the build did NOT recompile $BASE"
    say "  The binary still holds the old code, so any result now is meaningless."
    say "  This is the stale-object trap: an edit landing in the same filesystem"
    say "  second as the previous build leaves the object looking current."
    return 2
}

run_test() {
    ( eval "$TEST_CMD" ) > /dev/null 2>&1
}

restore() {
    git checkout -- "$FILE" || return 1
    bump_mtime
    invalidate
}

# ── 1. Positive control: green before we touch anything ──────────────────────
say "checking the test passes before the edit"
invalidate
build_and_verify_recompile "baseline" || die_inconclusive "could not build a clean baseline"
if ! run_test; then
    die_inconclusive "the test already fails before any edit; fix that first"
fi
BASELINE_BINARY="$(binary_fingerprint)"
[ "$BASELINE_BINARY" = "absent" ] &&
    die_inconclusive "cannot find the test binary '$TEST_BINARY' to fingerprint"

# ── 2. Break the fix ─────────────────────────────────────────────────────────
say "breaking the fix in $FILE"
BEFORE_HASH="$(git hash-object "$FILE")"
if ! eval "$BREAK_CMD \"$FILE\""; then
    restore
    die_inconclusive "the break command failed to run"
fi
if [ "$(git hash-object "$FILE")" = "$BEFORE_HASH" ]; then
    restore
    die_inconclusive "the break command changed nothing — check the pattern matches"
fi

bump_mtime
invalidate
build_and_verify_recompile "broken"
BUILD_STATUS=$?
if [ "$BUILD_STATUS" -eq 2 ]; then
    restore
    exit 2
fi

BROKEN_PASSES=0
if [ "$BUILD_STATUS" -eq 0 ]; then
    # The binary must differ from the one the baseline ran. If it does not, the
    # edit never reached what the test executes and any verdict here would be
    # about the old code.
    if [ "$(binary_fingerprint)" = "$BASELINE_BINARY" ]; then
        restore
        die_inconclusive "the test binary is unchanged after breaking $BASE — the
    edit did not reach what the test runs, so no verdict is possible"
    fi
    if run_test; then BROKEN_PASSES=1; fi
fi

# ── 3. Put it back and prove we are green again ──────────────────────────────
say "restoring $FILE"
restore || die_inconclusive "could not restore $FILE — the tree is left dirty"
build_and_verify_recompile "restored" || die_inconclusive "could not rebuild after restoring"
if ! run_test; then
    die_inconclusive "the test does not pass after restoring; the tree may be inconsistent"
fi

# ── 4. Verdict ───────────────────────────────────────────────────────────────
if [ "$BROKEN_PASSES" -eq 1 ]; then
    say "NOT CONFIRMED — the test passed with the fix broken."
    say "  It does not cover this change. Common causes: the assertion restates"
    say "  what the code assumes, the broken path is never reached by the inputs"
    say "  under test, or a guard skips the check when the thing it needs is absent."
    exit 1
fi

say "CONFIRMED — the test passes with the fix and fails without it."
exit 0
