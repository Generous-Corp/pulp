#!/bin/bash
# build-skia-android.sh — Build Skia Graphite + Dawn for Android arm64
#
# Produces static libraries compatible with Pulp's FindSkia.cmake.
# Uses Skia's own GN build system with Android NDK cross-compilation.
#
# Prerequisites:
#   - python3, git, ninja (brew install ninja)
#   - Android NDK (ANDROID_NDK_HOME or auto-detected)
#
# Usage:
#   ./tools/build-skia-android.sh
#
# Output:
#   external/skia-build/android-gpu/lib/Release/*.a
#   external/skia-build/include/  (shared with other platforms)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PULP_ROOT="$(dirname "$SCRIPT_DIR")"
SKIA_SRC="$PULP_ROOT/external/skia-src"
SKIA_OUTPUT="$PULP_ROOT/external/skia-build"
SKIA_BRANCH="${SKIA_BRANCH:-chrome/m144}"

# Find Android NDK
if [ -z "$ANDROID_NDK_HOME" ]; then
    if [ -z "$ANDROID_HOME" ]; then
        ANDROID_HOME="$HOME/Library/Android/sdk"
    fi
    # Find latest NDK
    ANDROID_NDK_HOME=$(ls -d "$ANDROID_HOME/ndk"/* 2>/dev/null | sort -V | tail -1)
fi

if [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "Error: Android NDK not found. Set ANDROID_NDK_HOME."
    exit 1
fi

echo "=== Pulp Skia Android Builder ==="
echo "Skia branch: $SKIA_BRANCH"
echo "NDK: $ANDROID_NDK_HOME"
echo "Output: $SKIA_OUTPUT/android-gpu/"
echo ""

# ── Step 1: Get Skia source ──────────────────────────────────────────────
if [ ! -d "$SKIA_SRC/.git" ]; then
    echo "Cloning Skia..."
    git clone --depth 1 --branch "$SKIA_BRANCH" https://skia.googlesource.com/skia.git "$SKIA_SRC"
else
    echo "Skia source exists, syncing to $SKIA_BRANCH..."
    git -C "$SKIA_SRC" fetch --depth 1 origin "$SKIA_BRANCH"
    git -C "$SKIA_SRC" checkout FETCH_HEAD
fi

# ── Step 2: Sync dependencies ────────────────────────────────────────────
echo "Syncing Skia dependencies..."
cd "$SKIA_SRC"
python3 tools/git-sync-deps

# ── Step 3: Configure with GN ────────────────────────────────────────────
echo "Configuring Skia for Android arm64..."

BUILD_DIR="out/android-arm64-release"
mkdir -p "$BUILD_DIR"

cat > "$BUILD_DIR/args.gn" << 'SKIA_GN_ARGS'
# Pulp Android arm64 build — Skia Graphite + Dawn
target_os = "android"
target_cpu = "arm64"
is_official_build = true
is_debug = false

# Graphite GPU backend (Dawn/Vulkan)
skia_enable_graphite = true
skia_use_dawn = true
skia_use_vulkan = true

# Text shaping
skia_use_harfbuzz = true
skia_use_icu = true
skia_enable_paragraph = true

# SVG + Lottie
skia_enable_svg = true
skia_enable_skottie = true

# Disable backends we don't need on Android
skia_use_gl = false
skia_use_metal = false
skia_use_direct3d = false

# Use bundled third-party libs (FreeType, expat, etc.)
skia_use_system_freetype2 = false
skia_use_system_harfbuzz = false
skia_use_system_icu = false
skia_use_system_expat = false
skia_use_system_zlib = false
skia_use_system_libpng = false
skia_use_system_libwebp = false
skia_use_system_libjpeg_turbo = false
skia_use_freetype = true
skia_use_expat = true
skia_use_libpng_decode = true
skia_use_libpng_encode = true
skia_use_zlib = true

# Android NDK
ndk = ""
ndk_api = 26

# Optimizations
skia_enable_pdf = false
skia_enable_sksl = true
SKIA_GN_ARGS

# Set NDK path in the GN args
sed -i '' "s|ndk = \"\"|ndk = \"$ANDROID_NDK_HOME\"|" "$BUILD_DIR/args.gn"

echo "GN args:"
cat "$BUILD_DIR/args.gn"
echo ""

# Run GN
bin/gn gen "$BUILD_DIR"

# ── Step 4: Build ────────────────────────────────────────────────────────
echo "Building Skia for Android arm64 (this may take a few minutes)..."
ninja -C "$BUILD_DIR" \
    skia \
    skshaper \
    skparagraph \
    skunicode_core \
    skunicode_icu \
    svg \
    skottie \
    sksg

# ── Step 5: Copy to Pulp output ─────────────────────────────────────────
echo "Copying to $SKIA_OUTPUT/android-gpu/..."
mkdir -p "$SKIA_OUTPUT/android-gpu/lib/Release"

# Copy static libraries
cp "$BUILD_DIR"/*.a "$SKIA_OUTPUT/android-gpu/lib/Release/" 2>/dev/null || true
# Some builds put libs in subdirectories
find "$BUILD_DIR" -name "*.a" -maxdepth 1 -exec cp {} "$SKIA_OUTPUT/android-gpu/lib/Release/" \;

# Copy ALL headers from Skia source (shared with other platforms)
echo "Copying Skia headers..."
# Copy entire include/ tree (preserving structure for root-relative includes)
cd "$SKIA_SRC"
find include -name "*.h" | while read f; do
    mkdir -p "$SKIA_OUTPUT/$(dirname "$f")"
    cp "$f" "$SKIA_OUTPUT/$f"
done
# Copy module headers
for mod in skparagraph skshaper svg skottie sksg; do
    if [ -d "$SKIA_SRC/modules/$mod/include" ]; then
        mkdir -p "$SKIA_OUTPUT/modules/$mod/include"
        cp "$SKIA_SRC/modules/$mod/include/"*.h "$SKIA_OUTPUT/modules/$mod/include/" 2>/dev/null || true
    fi
done

echo ""
echo "=== Skia Android build complete ==="
echo "Libraries: $(ls "$SKIA_OUTPUT/android-gpu/lib/Release/"*.a 2>/dev/null | wc -l | tr -d ' ') static libs"
echo ""
echo "To use: cmake -DSKIA_DIR=$SKIA_OUTPUT -DPULP_ENABLE_GPU=ON ..."
