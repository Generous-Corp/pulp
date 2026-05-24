#!/bin/bash
# build-skia.sh — Build Skia Graphite for Pulp
# Uses danielraffel/skia-builder (fork of olilarkin/skia-builder) to produce
# pre-built static libraries. The fork tracks upstream's tag pattern and
# publishes additional iOS/visionOS/mac-x86_64 slices that upstream omits.
#
# Usage:
#   ./tools/build-skia.sh          # Build for current platform
#   ./tools/build-skia.sh mac      # Build for macOS
#   ./tools/build-skia.sh ios      # Build for iOS
#   ./tools/build-skia.sh all      # Build all platforms
#
# Prerequisites: python3, ninja, git
# Output: external/skia-build/{platform}/lib/ + include/

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PULP_ROOT="$(dirname "$SCRIPT_DIR")"
SKIA_BUILDER_DIR="$PULP_ROOT/external/skia-builder"
SKIA_BUILD_OUTPUT="$PULP_ROOT/external/skia-build"
# Default to the fork's chrome/m149 branch HEAD. Override with SKIA_BUILDER_REF
# to pin to a specific commit (omit to track the fork branch head).
SKIA_BUILDER_URL="${SKIA_BUILDER_URL:-https://github.com/danielraffel/skia-builder.git}"
SKIA_BUILDER_REF="${SKIA_BUILDER_REF:-}"
SKIA_BRANCH="${SKIA_BRANCH:-chrome/m149}"

PLATFORM="${1:-mac}"

echo "=== Pulp Skia Builder ==="
echo "Platform: $PLATFORM"
echo "Builder URL: $SKIA_BUILDER_URL"
echo "Builder ref: ${SKIA_BUILDER_REF:-(branch HEAD)}"
echo "Branch: $SKIA_BRANCH"
echo "Output: $SKIA_BUILD_OUTPUT"
echo ""

# Clone skia-builder if not present
if [ ! -d "$SKIA_BUILDER_DIR" ]; then
    echo "Cloning skia-builder..."
    git clone "$SKIA_BUILDER_URL" "$SKIA_BUILDER_DIR"
fi

if [ -n "$SKIA_BUILDER_REF" ]; then
    echo "Syncing skia-builder to $SKIA_BUILDER_REF..."
    git -C "$SKIA_BUILDER_DIR" fetch --depth 1 origin "$SKIA_BUILDER_REF"
    git -C "$SKIA_BUILDER_DIR" checkout --detach "$SKIA_BUILDER_REF"
else
    echo "Syncing skia-builder to $SKIA_BRANCH HEAD..."
    git -C "$SKIA_BUILDER_DIR" fetch --depth 1 origin "$SKIA_BRANCH"
    git -C "$SKIA_BUILDER_DIR" checkout FETCH_HEAD
fi

# Increase file limit on macOS
if [ "$(uname)" = "Darwin" ]; then
    ulimit -n 2048
fi

# Build
cd "$SKIA_BUILDER_DIR"

if [ "$PLATFORM" = "all" ]; then
    python3 build-skia.py mac -branch "$SKIA_BRANCH" --shallow
    # python3 build-skia.py ios -branch "$SKIA_BRANCH" --shallow
    # python3 build-skia.py linux -branch "$SKIA_BRANCH" --shallow
    # python3 build-skia.py win -branch "$SKIA_BRANCH" --shallow
else
    python3 build-skia.py "$PLATFORM" -branch "$SKIA_BRANCH" --shallow
fi

# Copy output to Pulp's expected location
echo ""
echo "Copying build output to $SKIA_BUILD_OUTPUT..."
mkdir -p "$SKIA_BUILD_OUTPUT"

# Copy headers
if [ -d "$SKIA_BUILDER_DIR/build/include" ]; then
    cp -R "$SKIA_BUILDER_DIR/build/include" "$SKIA_BUILD_OUTPUT/"
fi

# Copy platform libraries
for plat in mac ios win linux wasm; do
    if [ -d "$SKIA_BUILDER_DIR/build/$plat" ]; then
        mkdir -p "$SKIA_BUILD_OUTPUT/$plat"
        cp -R "$SKIA_BUILDER_DIR/build/$plat/lib" "$SKIA_BUILD_OUTPUT/$plat/"
    fi
done

echo ""
echo "=== Skia build complete ==="
echo "Set SKIA_DIR=$SKIA_BUILD_OUTPUT when configuring Pulp:"
echo "  cmake -B build -DSKIA_DIR=$SKIA_BUILD_OUTPUT"
