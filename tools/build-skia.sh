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
# Output: external/skia-build/build/{include,share,<platform>-gpu}/ (skia-builder layout)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PULP_ROOT="$(dirname "$SCRIPT_DIR")"
SKIA_BUILDER_DIR="$PULP_ROOT/external/skia-builder"
SKIA_BUILD_OUTPUT="$PULP_ROOT/external/skia-build"
# The source fallback publishes into the checkout-owned tree. A symlink here
# could redirect the publish (and its cleanup) into an immutable shared cache.
if [ -L "$SKIA_BUILD_OUTPUT" ]; then
    echo "ERROR: refusing to publish source-built Skia through symlink $SKIA_BUILD_OUTPUT" >&2
    echo "Use a real checkout-owned external/skia-build directory; shared caches are fetcher-owned." >&2
    exit 1
fi
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

# skia-builder writes everything under its build/ directory: the Skia checkout
# (build/src/skia, with Dawn under third_party/externals), the headers
# (build/include), ICU data (build/share), and one slice per platform
# (build/<platform>-gpu/lib/Release/...). The checkout and build/tmp are reused
# as caches across runs; the outputs are not. Clear this run's outputs first so
# nothing an older revision left behind (a header, an arch subdirectory) can be
# mistaken for this build.
BUILDER_OUT="$SKIA_BUILDER_DIR/build"
SKIA_SOURCE_DIR="$BUILDER_OUT/src/skia"
rm -rf "$BUILDER_OUT/include" "$BUILDER_OUT/share"
for build_platform in "${BUILD_PLATFORMS[@]}"; do
    rm -rf "$BUILDER_OUT/$build_platform-gpu"
done

# Build
cd "$SKIA_BUILDER_DIR"

for build_platform in "${BUILD_PLATFORMS[@]}"; do
    python3 build-skia.py "$build_platform" -branch "$SKIA_BRANCH" \
        -revision "$SKIA_EXPECTED_COMMIT" --shallow
done

# skia-builder checks out the revision itself; verify the source it actually
# built, and the Dawn that git-sync-deps resolved from that revision's DEPS,
# before any output reaches $SKIA_BUILD_OUTPUT.
actual_skia_commit=""
if [ -e "$SKIA_SOURCE_DIR/.git" ]; then
    actual_skia_commit="$(git -C "$SKIA_SOURCE_DIR" rev-parse HEAD)"
fi
if [ "$actual_skia_commit" != "$SKIA_EXPECTED_COMMIT" ]; then
    echo "ERROR: built Skia commit ${actual_skia_commit:-<missing>} does not match pinned $SKIA_EXPECTED_COMMIT" >&2
    echo "Use the verified release asset, or explicitly override SKIA_EXPECTED_COMMIT for a development-only build." >&2
    exit 1
fi
dawn_src="$SKIA_SOURCE_DIR/third_party/externals/dawn"
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

# Publish the verified generation as $SKIA_BUILD_OUTPUT/build, the skia-builder
# release layout FindSkia probes first. Only this run's headers, ICU data, and
# platform slices are published, and the whole directory replaces the previous
# one: it is staged in a sibling directory and renamed into place, so a slice
# from another revision cannot survive next to this one and a failed copy
# leaves the previous generation untouched.
PUBLISH_DIR="$SKIA_BUILD_OUTPUT/build"
echo ""
echo "Publishing verified build output to $PUBLISH_DIR..."
mkdir -p "$SKIA_BUILD_OUTPUT"
stage_dir="$(mktemp -d "$SKIA_BUILD_OUTPUT/.build.staging.XXXXXX")"
retired_dir=""
cleanup_publish() {
    rm -rf "$stage_dir"
    if [ -n "$retired_dir" ] && [ -e "$retired_dir" ] && [ ! -e "$PUBLISH_DIR" ]; then
        mv "$retired_dir" "$PUBLISH_DIR"
    fi
}
trap cleanup_publish EXIT

if [ ! -d "$BUILDER_OUT/include" ]; then
    echo "ERROR: skia-builder produced no headers at $BUILDER_OUT/include" >&2
    exit 1
fi
cp -R "$BUILDER_OUT/include" "$stage_dir/include"
if [ -d "$BUILDER_OUT/share" ]; then
    cp -R "$BUILDER_OUT/share" "$stage_dir/share"
fi
for build_platform in "${BUILD_PLATFORMS[@]}"; do
    if [ ! -d "$BUILDER_OUT/$build_platform-gpu/lib" ]; then
        echo "ERROR: skia-builder produced no $build_platform libraries at $BUILDER_OUT/$build_platform-gpu/lib" >&2
        exit 1
    fi
    cp -R "$BUILDER_OUT/$build_platform-gpu" "$stage_dir/$build_platform-gpu"
done

if [ -e "$PUBLISH_DIR" ]; then
    retired_dir="$SKIA_BUILD_OUTPUT/.build.retired.$$"
    rm -rf "$retired_dir"
    mv "$PUBLISH_DIR" "$retired_dir"
fi
mv "$stage_dir" "$PUBLISH_DIR"
if [ -n "$retired_dir" ]; then
    rm -rf "$retired_dir"
fi
# This tree is source-built, not a fetched release asset. Remove fetcher's
# legacy provenance files so a later fetch cannot treat these bytes as a warm
# verified archive generation.
rm -f "$SKIA_BUILD_OUTPUT/.skia-asset-sha256" \
      "$SKIA_BUILD_OUTPUT/.skia-generation-manifest.json" \
      "$SKIA_BUILD_OUTPUT/.skia-source-archive.zip"
trap - EXIT

echo ""
echo "=== Skia build complete ==="
echo "Set SKIA_DIR=$SKIA_BUILD_OUTPUT when configuring Pulp:"
echo "  cmake -B build -DSKIA_DIR=$SKIA_BUILD_OUTPUT"
