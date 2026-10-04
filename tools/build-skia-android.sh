#!/bin/bash
# build-skia-android.sh — Build Skia Graphite + Dawn for Android
#
# Produces static libraries compatible with Pulp's FindSkia.cmake.
# Uses Skia's own GN build system with Android NDK cross-compilation.
#
# Prerequisites:
#   - python3, git, ninja (brew install ninja)
#   - Android NDK (ANDROID_NDK_HOME or auto-detected)
#
# Usage:
#   ./tools/build-skia-android.sh [arm64|x86_64|all]
#
# Default ABI is arm64. Pass "x86_64" to build for the emulator or
# "all" to build both ABIs sequentially.
#
# Output:
#   external/skia-build/android-gpu/lib/Release/*.a            (arm64)
#   external/skia-build/android-gpu-x86_64/lib/Release/*.a     (x86_64)
#   external/skia-build/include/  (shared with other platforms)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PULP_ROOT="$(dirname "$SCRIPT_DIR")"
SKIA_SRC="$PULP_ROOT/external/skia-src"
SKIA_OUTPUT="$PULP_ROOT/external/skia-build"
MANIFEST="$PULP_ROOT/tools/deps/manifest.json"

# Android must consume the same immutable Skia/Dawn provider pair as the
# desktop builder. Environment overrides remain useful for development, but
# the manifest is the default authority and every build is checked against it.
read_manifest_pin() {
    python3 - "$MANIFEST" "$1" <<'PY'
import json
import sys

manifest = json.load(open(sys.argv[1], encoding="utf-8"))
skia = next(item for item in manifest["dependencies"] if item["name"] == "Skia")
print(skia["determinism"][sys.argv[2]])
PY
}

SKIA_BRANCH="${SKIA_BRANCH:-$(read_manifest_pin skia_branch)}"
SKIA_EXPECTED_COMMIT="${SKIA_EXPECTED_COMMIT:-$(read_manifest_pin skia_commit)}"
DAWN_EXPECTED_COMMIT="${DAWN_EXPECTED_COMMIT:-$(read_manifest_pin built_dawn)}"

# ── ABI selection ────────────────────────────────────────────────────────
ABI_ARG="${1:-arm64}"
case "$ABI_ARG" in
    arm64|arm64-v8a) ABIS=(arm64);;
    x86_64|x64)      ABIS=(x86_64);;
    all)             ABIS=(arm64 x86_64);;
    *)
        echo "Error: unknown ABI '$ABI_ARG' (expected arm64, x86_64, or all)"
        exit 1
        ;;
esac

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
echo "Skia commit: $SKIA_EXPECTED_COMMIT"
echo "Dawn commit: $DAWN_EXPECTED_COMMIT"
echo "NDK: $ANDROID_NDK_HOME"
echo "ABIs: ${ABIS[*]}"
echo ""

# ── Step 1: Get Skia source ──────────────────────────────────────────────
if [ ! -d "$SKIA_SRC/.git" ]; then
    echo "Cloning Skia..."
    git clone --no-checkout https://skia.googlesource.com/skia.git "$SKIA_SRC"
else
    echo "Skia source exists, refreshing origin..."
    git -C "$SKIA_SRC" remote set-url origin https://skia.googlesource.com/skia.git
fi

echo "Fetching pinned Skia revision..."
git -C "$SKIA_SRC" fetch --depth 1 origin "$SKIA_EXPECTED_COMMIT"
git -C "$SKIA_SRC" checkout --detach "$SKIA_EXPECTED_COMMIT"
ACTUAL_SKIA_COMMIT="$(git -C "$SKIA_SRC" rev-parse HEAD)"
if [ "$ACTUAL_SKIA_COMMIT" != "$SKIA_EXPECTED_COMMIT" ]; then
    echo "Error: checked out Skia $ACTUAL_SKIA_COMMIT, expected $SKIA_EXPECTED_COMMIT" >&2
    exit 1
fi

# The generated public header must resolve against the same source tree. This
# catches stale m149/m151 artifacts before an expensive Android configure.
SKUNICODE_HEADER="$SKIA_SRC/modules/skunicode/include/SkUnicode.h"
if grep -q 'src/base/SkUTF.h' "$SKUNICODE_HEADER"; then
    echo "Error: pinned Skia still references removed src/base/SkUTF.h" >&2
    exit 1
fi
if ! grep -q 'src/core/SkUTF.h' "$SKUNICODE_HEADER"; then
    echo "Error: unexpected SkUnicode.h provenance in pinned Skia checkout" >&2
    exit 1
fi

# ── Step 2: Sync dependencies ────────────────────────────────────────────
echo "Syncing Skia dependencies..."
cd "$SKIA_SRC"
python3 tools/git-sync-deps

DAWN_SRC="$SKIA_SRC/third_party/externals/dawn"
if [ ! -d "$DAWN_SRC/.git" ]; then
    echo "Error: Dawn checkout missing after git-sync-deps" >&2
    exit 1
fi
ACTUAL_DAWN_COMMIT="$(git -C "$DAWN_SRC" rev-parse HEAD)"
if [ "$ACTUAL_DAWN_COMMIT" != "$DAWN_EXPECTED_COMMIT" ]; then
    echo "Error: Dawn checkout $ACTUAL_DAWN_COMMIT does not match manifest pin $DAWN_EXPECTED_COMMIT" >&2
    exit 1
fi

# ── Step 3-5: Configure, build, and stage each requested ABI ──────────────
for ABI in "${ABIS[@]}"; do
    # Map Pulp ABI label → GN target_cpu and output subdirectory
    case "$ABI" in
        arm64)
            GN_CPU="arm64"
            OUT_SUBDIR="android-gpu"         # default/canonical
            ;;
        x86_64)
            GN_CPU="x64"
            OUT_SUBDIR="android-gpu-x86_64"
            ;;
        *)
            echo "Error: unsupported ABI '$ABI' in build loop"
            exit 1
            ;;
    esac

    BUILD_DIR="out/android-${ABI}-release"
    echo ""
    echo "── Configuring Skia for Android ${ABI} (target_cpu=${GN_CPU}) ──────"
    mkdir -p "$SKIA_SRC/$BUILD_DIR"

    cat > "$SKIA_SRC/$BUILD_DIR/args.gn" << SKIA_GN_ARGS
# Pulp Android ${ABI} build — Skia Graphite + Dawn
target_os = "android"
target_cpu = "${GN_CPU}"
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
ndk = "${ANDROID_NDK_HOME}"
ndk_api = 26

# Optimizations
skia_enable_pdf = false
skia_enable_sksl = true
SKIA_GN_ARGS

    echo "GN args for ${ABI}:"
    cat "$SKIA_SRC/$BUILD_DIR/args.gn"
    echo ""

    cd "$SKIA_SRC"
    bin/gn gen "$BUILD_DIR"

    echo "Building Skia for Android ${ABI} (this may take a few minutes)..."
    ninja -C "$BUILD_DIR" \
        skia \
        skshaper \
        skparagraph \
        skunicode_core \
        skunicode_icu \
        svg \
        skottie \
        sksg

    # Dawn's Android build is a CMake graph nested below the GN output. Its
    # default component graph leaves the top-level libdawn_combined.a empty;
    # build Dawn's supported monolithic archive so FindSkia gets one complete,
    # source-matched provider instead of a fragile component-library order.
    if [ -d "$SKIA_SRC/$BUILD_DIR/cmake_dawn" ]; then
        cmake -S "$SKIA_SRC/third_party/externals/dawn" \
            -B "$SKIA_SRC/$BUILD_DIR/cmake_dawn" \
            -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
        cmake --build "$SKIA_SRC/$BUILD_DIR/cmake_dawn" --target webgpu_dawn
        cp "$SKIA_SRC/$BUILD_DIR/cmake_dawn/src/dawn/native/libwebgpu_dawn.a" \
            "$SKIA_SRC/$BUILD_DIR/libdawn_combined.a"
        # The native archive does not contain Dawn's generated proc-table
        # entrypoints (dawnProcSetProcs/dawnProcGetVersion). Keep that small
        # archive beside the monolithic provider so FindSkia's linker group
        # resolves the same pinned Dawn ABI used by Pulp's native callers.
        # Dawn's monolithic archive already defines every wgpu* entrypoint,
        # though, so retain only the proc-table state/version symbols from
        # libdawn_proc.a. Localizing its duplicate wgpu* symbols keeps this
        # provider linkable without silently selecting two Dawn implementations.
        DAWN_PROC_ARCHIVE="$SKIA_SRC/$BUILD_DIR/cmake_dawn/src/dawn/libdawn_proc.a"
        DAWN_PROC_COMPAT="$SKIA_SRC/$BUILD_DIR/libdawn_proc_compat.a"
        DAWN_NM="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -type f -name llvm-nm -print -quit)"
        DAWN_OBJCOPY="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -type f -name llvm-objcopy -print -quit)"
        if [ -z "$DAWN_NM" ] || [ -z "$DAWN_OBJCOPY" ]; then
            echo "Error: Android NDK llvm-nm/llvm-objcopy are required to stage Dawn proc support" >&2
            exit 1
        fi
        cp "$DAWN_PROC_ARCHIVE" "$DAWN_PROC_COMPAT"
        while IFS= read -r symbol; do
            "$DAWN_OBJCOPY" --localize-symbol="$symbol" "$DAWN_PROC_COMPAT"
        done < <("$DAWN_NM" -g --defined-only "$DAWN_PROC_COMPAT" |
            awk '$3 ~ /^wgpu/ { print $3 }' | sort -u)
    fi

    echo "Copying ${ABI} libraries to $SKIA_OUTPUT/${OUT_SUBDIR}/lib/Release/..."
    mkdir -p "$SKIA_OUTPUT/${OUT_SUBDIR}/lib/Release"

    cp "$SKIA_SRC/$BUILD_DIR"/*.a "$SKIA_OUTPUT/${OUT_SUBDIR}/lib/Release/" 2>/dev/null || true
    find "$SKIA_SRC/$BUILD_DIR" -name "*.a" -maxdepth 1 \
        -exec cp {} "$SKIA_OUTPUT/${OUT_SUBDIR}/lib/Release/" \;
done

# Copy ALL headers from Skia source (shared with other platforms)
echo "Copying Skia headers..."
# Copy entire include/ tree (preserving structure for root-relative includes)
cd "$SKIA_SRC"
find include -name "*.h" | while read f; do
    mkdir -p "$SKIA_OUTPUT/$(dirname "$f")"
    cp "$f" "$SKIA_OUTPUT/$f"
done
# Copy module headers
for mod in skparagraph skshaper skunicode svg skottie sksg; do
    if [ -d "$SKIA_SRC/modules/$mod/include" ]; then
        mkdir -p "$SKIA_OUTPUT/modules/$mod/include"
        cp "$SKIA_SRC/modules/$mod/include/"*.h "$SKIA_OUTPUT/modules/$mod/include/" 2>/dev/null || true
    fi
done
if ! cmp -s "$SKUNICODE_HEADER" "$SKIA_OUTPUT/modules/skunicode/include/SkUnicode.h"; then
    echo "Error: staged SkUnicode.h does not match the pinned Skia source" >&2
    exit 1
fi

echo ""
echo "=== Skia Android build complete ==="
for ABI in "${ABIS[@]}"; do
    case "$ABI" in
        arm64)  OUT_SUBDIR="android-gpu";;
        x86_64) OUT_SUBDIR="android-gpu-x86_64";;
    esac
    COUNT=$(ls "$SKIA_OUTPUT/${OUT_SUBDIR}/lib/Release/"*.a 2>/dev/null | wc -l | tr -d ' ')
    echo "  ${ABI}: ${COUNT} static libs in $SKIA_OUTPUT/${OUT_SUBDIR}/lib/Release/"
done
echo ""
echo "To use (arm64 is the default and picks android-gpu/ automatically):"
echo "  cmake -DSKIA_DIR=$SKIA_OUTPUT -DPULP_ENABLE_GPU=ON ..."
echo ""
echo "For x86_64 emulator builds, set ANDROID_ABI=x86_64 and FindSkia.cmake"
echo "will pick android-gpu-x86_64/ automatically."
