#!/usr/bin/env bash
# build-api-docs.sh — Generate API reference from public headers using Doxygen.
# Output: build/api-docs/html/
#
# Injects the current SDK version from CMakeLists.txt (`project(Pulp VERSION x.y.z)`)
# as Doxygen's PROJECT_NUMBER so `/api/` always shows the right version instead
# of the stale literal baked into docs/doxygen/Doxyfile.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DOXYFILE="$ROOT/docs/doxygen/Doxyfile"
TIMELINE_STRICT_DOXYFILE="$ROOT/docs/doxygen/Doxyfile.timeline-strict"
OUTPUT="$ROOT/build/api-docs"

if ! command -v doxygen &>/dev/null; then
    echo "Error: doxygen not found. Install with: brew install doxygen"
    exit 1
fi

if [ ! -f "$DOXYFILE" ]; then
    echo "Error: Doxyfile not found at $DOXYFILE"
    exit 1
fi
if [ ! -f "$TIMELINE_STRICT_DOXYFILE" ]; then
    echo "Error: strict Timeline Doxyfile not found at $TIMELINE_STRICT_DOXYFILE"
    exit 1
fi

# Extract `project(Pulp VERSION x.y.z)` from the root CMakeLists.txt. Uses
# grep/sed so this works under both BSD and GNU userland (macOS + Linux).
# Falls back to the Doxyfile literal if the regex misses — Doxygen still
# produces output, just with the old version.
SDK_VERSION="$(grep -oE 'VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "$ROOT/CMakeLists.txt" 2>/dev/null \
    | head -1 \
    | sed -E 's/VERSION[[:space:]]+//' || true)"

if [ -z "$SDK_VERSION" ]; then
    echo "Warning: could not parse SDK VERSION from CMakeLists.txt; using Doxyfile literal"
fi

echo "Generating API reference (Pulp ${SDK_VERSION:-unknown})..."
mkdir -p "$ROOT/build"

# Run Doxygen from the docs/doxygen directory so relative paths resolve.
# Stream the Doxyfile through stdin with an appended PROJECT_NUMBER override —
# Doxygen treats later assignments as wins, so the Doxyfile stays untouched.
cd "$ROOT/docs/doxygen"
DOXYGEN_LOG="$(mktemp "${TMPDIR:-/tmp}/pulp-doxygen.XXXXXX")"
STAGING_OUTPUT="$(mktemp -d "$ROOT/build/api-docs-stage.XXXXXX")"
STRICT_OUTPUT="$(mktemp -d "$ROOT/build/api-docs-timeline-check.XXXXXX")"
trap 'rm -f "$DOXYGEN_LOG"; rm -rf "$STAGING_OUTPUT" "$STRICT_OUTPUT"' EXIT

# CI installs whatever Doxygen ubuntu ships (1.9.8 at time of writing) while a
# dev machine usually has a much newer Homebrew build. They do not agree on
# every diagnostic — 1.9.8 errors on an `@param` block Doxygen attaches to a
# friend declaration with unnamed parameters, and 1.17 does not — so a local
# pass is necessary but NOT sufficient. Print the version so a CI-only failure
# is immediately recognizable as a version difference rather than a mystery.
echo "Checking exhaustive Timeline API contracts (local $(doxygen --version)," \
     "CI uses ubuntu's package — a local pass does not guarantee CI passes)..."
if ! {
    cat "$TIMELINE_STRICT_DOXYFILE"
    echo "OUTPUT_DIRECTORY = \"$STRICT_OUTPUT\""
} | doxygen - >"$DOXYGEN_LOG" 2>&1; then
    cat "$DOXYGEN_LOG"
    echo "Error: strict Timeline API documentation check failed"
    exit 1
fi
if ! python3 "$ROOT/tools/scripts/timeline_api_docs_check.py" "$STRICT_OUTPUT/xml"; then
    exit 1
fi

if [ -n "$SDK_VERSION" ]; then
    if {
        cat "$DOXYFILE"
        echo "PROJECT_NUMBER = $SDK_VERSION"
        echo "OUTPUT_DIRECTORY = \"$STAGING_OUTPUT\""
    } | \
        doxygen - >"$DOXYGEN_LOG" 2>&1; then
        DOXYGEN_STATUS=0
    else
        DOXYGEN_STATUS=$?
    fi
else
    if {
        cat "$DOXYFILE"
        echo "OUTPUT_DIRECTORY = \"$STAGING_OUTPUT\""
    } | doxygen - >"$DOXYGEN_LOG" 2>&1; then
        DOXYGEN_STATUS=0
    else
        DOXYGEN_STATUS=$?
    fi
fi
if [ "$DOXYGEN_STATUS" -ne 0 ]; then
    cat "$DOXYGEN_LOG"
    echo "Error: Doxygen exited with status $DOXYGEN_STATUS"
    exit "$DOXYGEN_STATUS"
fi

# Keep successful builds readable even when Doxygen emits thousands of
# diagnostics. Failures print the complete log above.
DOXYGEN_DIAGNOSTIC_LIMIT=40
DOXYGEN_DIAGNOSTIC_COUNT="$(
    grep -Eic '(^|:[[:space:]])(warning|error):' "$DOXYGEN_LOG" || true
)"
if [ "$DOXYGEN_DIAGNOSTIC_COUNT" -gt 0 ]; then
    echo "Doxygen completed with $DOXYGEN_DIAGNOSTIC_COUNT diagnostic lines" \
         "(showing first $DOXYGEN_DIAGNOSTIC_LIMIT):"
    grep -Ei '(^|:[[:space:]])(warning|error):' "$DOXYGEN_LOG" \
        | sed -n "1,${DOXYGEN_DIAGNOSTIC_LIMIT}p" || true
    if [ "$DOXYGEN_DIAGNOSTIC_COUNT" -gt "$DOXYGEN_DIAGNOSTIC_LIMIT" ]; then
        echo "... $((DOXYGEN_DIAGNOSTIC_COUNT - DOXYGEN_DIAGNOSTIC_LIMIT))" \
             "additional diagnostic lines omitted"
    fi
fi

# Validate the staged result before replacing the last known-good tree.
if [ -d "$STAGING_OUTPUT/html" ]; then
    page_count=$(find "$STAGING_OUTPUT/html" -name "*.html" | wc -l | tr -d ' ')
    if [ "$page_count" -eq 0 ]; then
        echo "Error: Doxygen generated no HTML pages"
        exit 1
    fi
    for expected_symbol in DocumentSession Project MasterTransport PlaybackProgramCompiler \
                           SequenceProcessor ExportPlan; do
        if ! grep -R -q "$expected_symbol" "$STAGING_OUTPUT/html"; then
            echo "Error: generated API reference is missing $expected_symbol"
            exit 1
        fi
    done
else
    echo "Error: no output generated"
    exit 1
fi

PREVIOUS_OUTPUT="${OUTPUT}.previous.$$"
if [ -e "$PREVIOUS_OUTPUT" ]; then
    echo "Error: temporary API-doc backup already exists at $PREVIOUS_OUTPUT"
    exit 1
fi
if [ -e "$OUTPUT" ]; then
    mv "$OUTPUT" "$PREVIOUS_OUTPUT"
fi
if ! mv "$STAGING_OUTPUT" "$OUTPUT"; then
    if [ -e "$PREVIOUS_OUTPUT" ]; then
        mv "$PREVIOUS_OUTPUT" "$OUTPUT"
    fi
    echo "Error: could not publish staged API documentation"
    exit 1
fi
if [ -e "$PREVIOUS_OUTPUT" ]; then
    rm -rf "$PREVIOUS_OUTPUT"
fi
echo "Generated $page_count HTML pages in $OUTPUT/html/"
