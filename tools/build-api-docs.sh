#!/usr/bin/env bash
# build-api-docs.sh — Generate API reference from public headers using Doxygen
# Output: build/api-docs/html/

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

echo "Generating API reference..."
mkdir -p "$OUTPUT"

# Run Doxygen from the docs/doxygen directory so relative paths resolve
cd "$ROOT/docs/doxygen"
doxygen Doxyfile 2>&1 | grep -E "^(Generating|Warning|Error)" || true

# Count documented entities
if [ -d "$OUTPUT/html" ]; then
    page_count=$(find "$OUTPUT/html" -name "*.html" | wc -l | tr -d ' ')
    echo "Generated $page_count HTML pages in $OUTPUT/html/"
else
    echo "Error: no output generated"
    exit 1
fi
