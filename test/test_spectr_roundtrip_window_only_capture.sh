#!/usr/bin/env bash
#
# The Spectr roundtrip must capture from inside the running app. OS-level
# screenshot tools require desktop permissions, fail silently over SSH, and
# can include unrelated desktop pixels in the comparison image.
#
# This test pins:
#   1. The script parses cleanly under `bash -n` (no syntax regressions).
#   2. No OS-level `screencapture` invocation remains.
#   3. Spectr publishes an observe-profile Inspector session.
#   4. Capture uses all three discovered identity selectors, preventing a
#      different live app instance from being captured accidentally.
#   5. Unsupported capture is reported explicitly.
#
# Hermetic: reads only the script source; no launch of Spectr or image tools.

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

# 2. No OS-level capture command may return as a fallback.
if grep -E '^[[:space:]]*screencapture\b' "$HARNESS" >/dev/null; then
    fail "found OS-level screencapture invocation"
fi
pass "no OS-level screencapture fallback"

# 3. The app must publish an authenticated observe-profile session.
if ! grep -q 'PULP_INSPECT_PROFILE=observe' "$HARNESS"; then
    fail "Spectr is not launched with the Inspector observe profile"
fi
pass "Inspector observe profile enabled"

# 4. Pin the Inspector verb and exact identity tuple.
if ! grep -q 'inspect screenshot --out' "$HARNESS"; then
    fail "Inspector screenshot command is missing"
fi
for selector in session instance publication; do
    if ! grep -q -- "--$selector" "$HARNESS"; then
        fail "Inspector screenshot command is missing --$selector"
    fi
done
pass "Inspector capture uses the discovered session identity"

# 5. Unsupported capture must be surfaced instead of leaving an empty result.
if ! grep -q 'Status 3 means.*lacks capture capability' "$HARNESS"; then
    fail "unsupported Inspector capture status is not explained"
fi
pass "unsupported capture is explicit"

echo "OK — all 5 assertions passed."
