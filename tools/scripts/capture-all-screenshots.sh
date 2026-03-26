#!/bin/bash
# Capture screenshots of all example plugin UIs
# Usage: ./tools/scripts/capture-all-screenshots.sh [build_dir]
#
# Requires: pulp-screenshot tool built in the specified build directory
# Output: docs/examples/img/<plugin-name>.png

set -euo pipefail

BUILD_DIR="${1:-build}"
SCREENSHOT_TOOL="${BUILD_DIR}/tools/screenshot/pulp-screenshot"
OUTPUT_DIR="docs/examples/img"

if [ ! -x "$SCREENSHOT_TOOL" ]; then
    echo "Error: screenshot tool not found at $SCREENSHOT_TOOL"
    echo "Build it first: cmake --build $BUILD_DIR --target pulp-screenshot"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Plugins to capture (name, width, height)
declare -a PLUGINS=(
    "PulpGain:400:300"
    "PulpTone:500:300"
    "PulpEffect:500:300"
    "PulpCompressor:600:300"
    "PulpSynth:700:350"
    "PulpDrums:600:300"
    "PulpSampler:600:350"
)

for entry in "${PLUGINS[@]}"; do
    IFS=':' read -r name width height <<< "$entry"
    output="$OUTPUT_DIR/$(echo "$name" | tr '[:upper:]' '[:lower:]').png"

    echo "Capturing $name (${width}x${height})..."
    "$SCREENSHOT_TOOL" --demo --plugin "$name" \
        --width "$width" --height "$height" \
        --theme dark --output "$output" 2>/dev/null || {
        echo "  Warning: failed to capture $name (plugin may not support --demo)"
    }
done

echo ""
echo "Screenshots saved to $OUTPUT_DIR/"
ls -la "$OUTPUT_DIR"/*.png 2>/dev/null || echo "No screenshots generated"
