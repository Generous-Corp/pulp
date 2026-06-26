#!/bin/bash
# Capture the built-in screenshot demo at common example-gallery viewport sizes
# Usage: ./tools/scripts/capture-all-screenshots.sh [build_dir]
#
# Requires: pulp-screenshot tool built in the specified build directory
# Output: docs/examples/img/pulp-demo-<width>x<height>.png

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

# Viewport presets to capture (label, width, height). This script exercises the
# screenshot backend; it does not load plugin bundles or select plugins by name.
declare -a VIEWPORTS=(
    "400x300:400:300"
    "500x300:500:300"
    "600x300:600:300"
    "600x350:600:350"
    "700x350:700:350"
)

failures=0

for entry in "${VIEWPORTS[@]}"; do
    IFS=':' read -r label width height <<< "$entry"
    output="$OUTPUT_DIR/pulp-demo-${label}.png"

    echo "Capturing built-in demo (${width}x${height})..."
    "$SCREENSHOT_TOOL" --demo \
        --width "$width" --height "$height" \
        --theme dark --output "$output" || {
        echo "  Error: failed to capture demo viewport $label"
        failures=$((failures + 1))
    }
done

echo ""
echo "Screenshots saved to $OUTPUT_DIR/"
ls -la "$OUTPUT_DIR"/*.png 2>/dev/null || echo "No screenshots generated"

if [ "$failures" -ne 0 ]; then
    echo "Failed to capture $failures viewport(s)"
    exit 1
fi
