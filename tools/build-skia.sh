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
MANIFEST="$PULP_ROOT/tools/deps/manifest.json"
read_manifest_pin() {
    python3 - "$MANIFEST" "$1" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
skia = next(item for item in manifest["dependencies"] if item["name"] == "Skia")
print(skia["determinism"][sys.argv[2]])
PY
}

SKIA_BUILDER_URL="${SKIA_BUILDER_URL:-https://github.com/danielraffel/skia-builder.git}"
SKIA_BUILDER_REF="${SKIA_BUILDER_REF:-$(read_manifest_pin skia_builder_ref)}"
SKIA_BRANCH="${SKIA_BRANCH:-$(read_manifest_pin skia_branch)}"
SKIA_EXPECTED_COMMIT="${SKIA_EXPECTED_COMMIT:-$(read_manifest_pin skia_commit)}"
DAWN_EXPECTED_COMMIT="${DAWN_EXPECTED_COMMIT:-$(read_manifest_pin built_dawn)}"

# Chromium milestone branches keep receiving commits after a release is pinned,
# so the branch name only selects the clone; the build itself targets the exact
# manifest revision. skia-builder rejects anything but a full lowercase SHA, and
# checking here reports a bad override before any clone or download.
for pin_name in SKIA_EXPECTED_COMMIT DAWN_EXPECTED_COMMIT; do
    if ! printf '%s\n' "${!pin_name}" | grep -Eq '^[0-9a-f]{40}$'; then
        echo "ERROR: $pin_name must be a full 40-character lowercase commit SHA, got '${!pin_name}'" >&2
        exit 1
    fi
done

PLATFORM="${1:-mac}"

echo "=== Pulp Skia Builder ==="
echo "Platform: $PLATFORM"
echo "Builder URL: $SKIA_BUILDER_URL"
echo "Builder ref: $SKIA_BUILDER_REF"
echo "Skia branch: $SKIA_BRANCH"
echo "Expected Skia commit: $SKIA_EXPECTED_COMMIT"
echo "Expected Dawn commit: $DAWN_EXPECTED_COMMIT"
echo "Output: $SKIA_BUILD_OUTPUT"
echo ""

if [ "$PLATFORM" = "all" ]; then
    BUILD_PLATFORMS=(mac)
else
    BUILD_PLATFORMS=("$PLATFORM")
fi

# A dry run resolves the pins and prints the exact builder invocations without
# cloning, fetching, or building anything.
if [ "${PULP_SKIA_DRY_RUN:-0}" = "1" ]; then
    for build_platform in "${BUILD_PLATFORMS[@]}"; do
        echo "skia-build-receipt platform=$build_platform builder_ref=$SKIA_BUILDER_REF skia_revision=$SKIA_EXPECTED_COMMIT dawn=$DAWN_EXPECTED_COMMIT"
        echo "python3 build-skia.py $build_platform -branch $SKIA_BRANCH -revision $SKIA_EXPECTED_COMMIT --shallow"
    done
    exit 0
fi

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

# The builder must accept an immutable revision. An older builder ref only takes
# a branch, and would silently build whatever that branch points at today.
if ! grep -Fq '"-revision"' "$SKIA_BUILDER_DIR/build-skia.py"; then
    echo "ERROR: skia-builder $SKIA_BUILDER_REF does not support -revision; use a builder ref that can build an exact Skia commit" >&2
    exit 1
fi

# Increase file limit on macOS
if [ "$(uname)" = "Darwin" ]; then
    ulimit -n 2048
fi

# Build
cd "$SKIA_BUILDER_DIR"

for build_platform in "${BUILD_PLATFORMS[@]}"; do
    python3 build-skia.py "$build_platform" -branch "$SKIA_BRANCH" \
        -revision "$SKIA_EXPECTED_COMMIT" --shallow
done

# skia-builder checks out the revision itself; verify the source it actually
# built, and the Dawn that git-sync-deps resolved from that revision's DEPS,
# before any output reaches $SKIA_BUILD_OUTPUT.
actual_skia_commit="$(git -C "$SKIA_BUILDER_DIR/src/skia" rev-parse HEAD)"
if [ "$actual_skia_commit" != "$SKIA_EXPECTED_COMMIT" ]; then
    echo "ERROR: built Skia commit $actual_skia_commit does not match pinned $SKIA_EXPECTED_COMMIT" >&2
    echo "Use the verified release asset, or explicitly override SKIA_EXPECTED_COMMIT for a development-only build." >&2
    exit 1
fi
dawn_src="$SKIA_BUILDER_DIR/src/skia/third_party/externals/dawn"
# Test for Dawn's own .git first: git -C on a plain directory inside the Skia
# checkout would otherwise report the Skia HEAD.
actual_dawn_commit=""
if [ -e "$dawn_src/.git" ]; then
    actual_dawn_commit="$(git -C "$dawn_src" rev-parse HEAD)"
fi
if [ "$actual_dawn_commit" != "$DAWN_EXPECTED_COMMIT" ]; then
    echo "ERROR: built Dawn commit ${actual_dawn_commit:-<missing>} does not match pinned $DAWN_EXPECTED_COMMIT" >&2
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
