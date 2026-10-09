#!/usr/bin/env bash
#
# The Spectr roundtrip must capture from inside the running app. OS-level
# screenshot tools require desktop permissions, fail silently over SSH, and
# can include unrelated desktop pixels in the comparison image.
#
# This test pins:
#   1. The script parses cleanly under `bash -n` (no syntax regressions).
#   2. No OS-level `screencapture` invocation remains.
#   3. The retired Inspector screenshot route does not return.
#   4. Capture is delegated to the canonical control platform.
#   5. The existing-candidate continuation is explicit.
#   6. The optional CDP browser receipt preflight remains wired.
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

# 3. The retired Inspector screenshot route must not return.
if grep -q 'inspect screenshot' "$HARNESS"; then
    fail "found retired Inspector screenshot command"
fi
pass "no retired Inspector screenshot route"

# 4. Automatic capture must direct users to the canonical control platform.
if ! grep -q 'canonical control platform' "$HARNESS"; then
    fail "canonical control capture handoff is missing"
fi
pass "capture delegates to canonical control"

# 5. The handoff must name the existing-candidate continuation.
if ! grep -q -- '--skip-capture' "$HARNESS"; then
    fail "external capture continuation is missing"
fi
pass "external capture continuation is explicit"

# 6. Acceptance lanes must be able to validate the browser oracle before the
# native diff.  Keep this as a source contract so a future script cleanup
# cannot silently strand the reusable receipt verifier.
if ! grep -q 'SPECTR_BROWSER_RECEIPT' "$HARNESS"; then
    fail "browser receipt environment seam is missing"
fi
if ! grep -q 'verify_browser_fidelity_receipt.py' "$HARNESS"; then
    fail "browser receipt verifier is not invoked"
fi
pass "CDP browser receipt preflight seam is wired"

echo "OK — all 6 assertions passed."
