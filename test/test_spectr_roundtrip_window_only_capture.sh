#!/usr/bin/env bash
#
# Regression test for pulp-internal task #81 — `tools/import-validation/
# spectr-roundtrip.sh` must NOT silently fall back to a full-screen
# screencapture when the per-window capture fails, because the desktop /
# terminal background then bleeds into the histogram diff against the
# REFERENCE render and the similarity score reflects wallpaper instead
# of plugin UI.
#
# This test pins:
#   1. The script parses cleanly under `bash -n` (no syntax regressions).
#   2. The legacy unconditional fullscreen fallback line is GONE.
#   3. The opt-in env var `PULP_HARNESS_ALLOW_FULLSCREEN` is documented
#      and used to gate the fullscreen path.
#   4. Every retained `screencapture` invocation uses `-o` (drop shadow),
#      so even the explicit opt-in fullscreen path doesn't grow back
#      the same drop-shadow-noise problem.
#
# Hermetic: reads only the script source; no real screencapture, no
# launch of Spectr, no Pillow / Python deps. Exits 0 on pass.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
HARNESS="$SCRIPT_DIR/tools/import-validation/spectr-roundtrip.sh"

if [ ! -f "$HARNESS" ]; then
    echo "FAIL: $HARNESS not found" >&2
    exit 1
fi

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# 1. Syntax — bash -n.
if ! bash -n "$HARNESS"; then
    fail "spectr-roundtrip.sh has bash syntax errors"
fi
pass "syntax: bash -n clean"

# 2. The pre-#81 fallback `screencapture -x "$OUT"` (no -l, no -o, no
#    -W) must NOT appear as an unconditional invocation. Specifically,
#    no `screencapture -x "$OUT"` followed by EOL with no further flags
#    — the only acceptable fullscreen invocation is gated by an env
#    var AND uses `-o`.
#
# Use POSIX `[[:space:]]`, not GNU `\s`. BSD/macOS grep does not honor
# `\s` as whitespace, so the regex would silently miss an indented
# `screencapture -x "$OUT"` line and the regression check would
# *pass* even when the harness had reintroduced the bug. See Codex P2
# on PR #1849.
if grep -E '^[[:space:]]*screencapture -x "\$OUT"[[:space:]]*$' "$HARNESS" >/dev/null; then
    fail "found bare unconditional fullscreen screencapture (re-introduces task #81 regression)"
fi
pass "no unconditional fullscreen fallback"

# 3. Opt-in env var present.
if ! grep -q "PULP_HARNESS_ALLOW_FULLSCREEN" "$HARNESS"; then
    fail "PULP_HARNESS_ALLOW_FULLSCREEN opt-out gate not documented in the harness"
fi
pass "PULP_HARNESS_ALLOW_FULLSCREEN gate present"

# 4. Every retained screencapture *invocation* uses -o (drop shadow).
# We match lines that BEGIN with whitespace then `screencapture` —
# real invocations live at the start of a line (modulo indentation).
# Error/log strings that mention "screencapture" inside `red "..."`
# or `echo "..."` are excluded by the leading-anchor.
bad=$(grep -nE '^[[:space:]]*screencapture\b' "$HARNESS" | grep -v -- '-o' || true)
if [ -n "$bad" ]; then
    echo "FAIL: screencapture invocation without -o (drop shadow) — these lines:" >&2
    echo "$bad" >&2
    exit 1
fi
pass "every screencapture invocation uses -o (drop shadow)"

echo "OK — all 4 assertions passed."
