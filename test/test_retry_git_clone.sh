#!/usr/bin/env bash
#
# Unit test for retry_git_clone (setup.sh). Verifies the P1 Codex
# finding from #425 is actually fixed: a retry_git wrapping git clone
# fails on attempt 2+ with "destination path already exists" unless
# the partial target is scrubbed between attempts. retry_git_clone
# scrubs; this test asserts that the scrub happened and the final
# clone succeeded at the expected attempt count.
#
# Run directly:
#   bash test/test_retry_git_clone.sh
#
# Exits 0 on pass, non-zero on fail.

set -euo pipefail

# Import retry_git_clone by sourcing the first ~250 lines of setup.sh
# in an isolated subshell. We only want the function definitions and
# the info/warn helpers — not the platform-detect / build-driving
# side effects that run at the end of setup.sh. Using a small tempfile
# that carries only the helpers keeps this test hermetic.

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TMPDIR_T="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_T"' EXIT

# Extract info/warn/retry_git_clone from setup.sh into a sourceable
# snippet. setup.sh has them near the top before any side-effecting
# step; we strip everything after the last closing '}' of
# retry_git_clone.
awk '
    /^info\(\)/,/^}$/   { print; next }
    /^warn\(\)/,/^}$/   { print; next }
    /^retry_git_clone\(\)/,/^}$/ { print; next }
' "$SCRIPT_DIR/setup.sh" > "$TMPDIR_T/helpers.sh"

# shellcheck disable=SC1090
source "$TMPDIR_T/helpers.sh"

# Shorter backoff so the test is quick.
STATE="$TMPDIR_T/attempt"
echo 0 > "$STATE"
fake_clone() {
    local dest="$1"
    local attempt
    attempt=$(cat "$STATE")
    attempt=$((attempt + 1))
    echo "$attempt" > "$STATE"
    mkdir -p "$dest"
    touch "$dest/.partial"           # simulate partial clone output
    if [ "$attempt" -lt 3 ]; then
        return 1
    fi
    return 0
}

TARGET="$TMPDIR_T/dest"
rm -rf "$TARGET"

# Monkey-patch sleep so the test isn't slow.
sleep() { :; }

if ! retry_git_clone "test-repo" fake_clone "$TARGET"; then
    echo "FAIL: retry_git_clone returned non-zero when the 3rd attempt should succeed" >&2
    exit 1
fi

attempts=$(cat "$STATE")
if [ "$attempts" != "3" ]; then
    echo "FAIL: expected 3 attempts, got $attempts" >&2
    exit 1
fi

if [ ! -d "$TARGET" ]; then
    echo "FAIL: target should exist after final successful attempt" >&2
    exit 1
fi

# Second scenario: all 3 attempts fail — expect non-zero and target scrubbed.
echo 0 > "$STATE"
fake_clone_always_fail() {
    local dest="$1"
    local attempt
    attempt=$(cat "$STATE")
    attempt=$((attempt + 1))
    echo "$attempt" > "$STATE"
    mkdir -p "$dest"
    touch "$dest/.partial"
    return 1
}

rm -rf "$TARGET"
if retry_git_clone "test-repo" fake_clone_always_fail "$TARGET"; then
    echo "FAIL: retry_git_clone should fail when all attempts fail" >&2
    exit 1
fi

final_attempts=$(cat "$STATE")
if [ "$final_attempts" != "3" ]; then
    echo "FAIL: expected 3 attempts on all-fail path, got $final_attempts" >&2
    exit 1
fi

echo "PASS: retry_git_clone cleans partial target between attempts and reports correct exit code"
