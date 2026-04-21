#!/usr/bin/env bash
# build-api-docs.sh — Generate API reference from public headers using Doxygen.
# Output: build/api-docs/html/
#
# Injects the current SDK version from CMakeLists.txt (`project(Pulp VERSION x.y.z)`)
# as Doxygen's PROJECT_NUMBER so `/api/` always shows the right version instead
# of the stale literal baked into docs/doxygen/Doxyfile. (#577 PR 4.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DOXYFILE="$ROOT/docs/doxygen/Doxyfile"
OUTPUT="$ROOT/build/api-docs"

if ! command -v doxygen &>/dev/null; then
    echo "Error: doxygen not found. Install with: brew install doxygen"
    exit 1
fi

if [ ! -f "$DOXYFILE" ]; then
    echo "Error: Doxyfile not found at $DOXYFILE"
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
mkdir -p "$OUTPUT"

# Run Doxygen from the docs/doxygen directory so relative paths resolve.
# Stream the Doxyfile through stdin with an appended PROJECT_NUMBER override —
# Doxygen treats later assignments as wins, so the Doxyfile stays untouched.
cd "$ROOT/docs/doxygen"
if [ -n "$SDK_VERSION" ]; then
    { cat "$DOXYFILE"; echo "PROJECT_NUMBER = $SDK_VERSION"; } | \
        doxygen - 2>&1 | grep -E "^(Generating|Warning|Error)" || true
else
    doxygen Doxyfile 2>&1 | grep -E "^(Generating|Warning|Error)" || true
fi

# Count documented entities
if [ -d "$OUTPUT/html" ]; then
    page_count=$(find "$OUTPUT/html" -name "*.html" | wc -l | tr -d ' ')
    echo "Generated $page_count HTML pages in $OUTPUT/html/"
else
    echo "Error: no output generated"
    exit 1
fi
