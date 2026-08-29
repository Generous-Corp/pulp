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
# Reproducible fallbacks default to the immutable revisions recorded beside the
# release assets. Explicit overrides remain available for builder development.
SKIA_BUILDER_URL="${SKIA_BUILDER_URL:-https://github.com/danielraffel/skia-builder.git}"
SKIA_BUILDER_REF="${SKIA_BUILDER_REF:-$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(next(x for x in d["dependencies"] if x["name"] == "Skia")["determinism"]["skia_builder_ref"])' "$PULP_ROOT/tools/deps/manifest.json")}"
SKIA_BRANCH="${SKIA_BRANCH:-$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(next(x for x in d["dependencies"] if x["name"] == "Skia")["determinism"]["skia_branch"])' "$PULP_ROOT/tools/deps/manifest.json")}"
SKIA_EXPECTED_COMMIT="${SKIA_EXPECTED_COMMIT:-$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(next(x for x in d["dependencies"] if x["name"] == "Skia")["determinism"]["skia_commit"])' "$PULP_ROOT/tools/deps/manifest.json")}"

PLATFORM="${1:-mac}"

echo "=== Pulp Skia Builder ==="
echo "Platform: $PLATFORM"
echo "Builder URL: $SKIA_BUILDER_URL"
echo "Builder ref: $SKIA_BUILDER_REF"
echo "Skia branch: $SKIA_BRANCH"
echo "Expected Skia commit: $SKIA_EXPECTED_COMMIT"
echo "Output: $SKIA_BUILD_OUTPUT"
echo ""

# Clone skia-builder if not present
if [ ! -d "$SKIA_BUILDER_DIR" ]; then
    echo "Cloning skia-builder..."
    git clone "$SKIA_BUILDER_URL" "$SKIA_BUILDER_DIR"
fi

# Re-point the existing clone's `origin` to $SKIA_BUILDER_URL if it drifted
# (e.g. an older checkout from olilarkin/skia-builder + a newer SKIA_BUILDER_URL
# override). Without this, subsequent `git fetch origin <ref>` calls would
# silently pull from the stale remote and either fail to resolve the requested
# branch or build from the wrong fork.
current_origin=$(git -C "$SKIA_BUILDER_DIR" remote get-url origin 2>/dev/null || echo "")
if [ "$current_origin" != "$SKIA_BUILDER_URL" ]; then
    echo "Updating origin: $current_origin → $SKIA_BUILDER_URL"
    git -C "$SKIA_BUILDER_DIR" remote set-url origin "$SKIA_BUILDER_URL"
fi

echo "Syncing skia-builder to $SKIA_BUILDER_REF..."
git -C "$SKIA_BUILDER_DIR" fetch --depth 1 origin "$SKIA_BUILDER_REF"
git -C "$SKIA_BUILDER_DIR" checkout --detach FETCH_HEAD

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

# skia-builder currently accepts a branch name, not an arbitrary commit. Never
# publish local fallback output merely because that mutable branch resolved:
# the built checkout must still equal the immutable manifest revision.
actual_skia_commit="$(git -C "$SKIA_BUILDER_DIR/src/skia" rev-parse HEAD)"
if [ "$actual_skia_commit" != "$SKIA_EXPECTED_COMMIT" ]; then
    echo "ERROR: built Skia commit $actual_skia_commit does not match pinned $SKIA_EXPECTED_COMMIT" >&2
    echo "Use the verified release asset, or explicitly override SKIA_EXPECTED_COMMIT for a development-only build." >&2
    exit 1
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
