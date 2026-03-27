#!/bin/bash
# Pulp development environment setup
# Run this after cloning to bootstrap a working build environment.
#
# Usage:
#   ./setup.sh              # Interactive setup
#   ./setup.sh --ci         # Non-interactive (CI/automation)
#   ./setup.sh --dry-run    # Show what would be done without doing it

set -e

# ── Configuration ────────────────────────────────────────────────────────────

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY_RUN=false
CI_MODE=false
ERRORS=0

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=true ;;
        --ci)      CI_MODE=true ;;
        --help|-h)
            echo "Usage: ./setup.sh [--ci] [--dry-run]"
            echo ""
            echo "Options:"
            echo "  --ci        Non-interactive mode for CI/automation"
            echo "  --dry-run   Show what would be done without doing it"
            exit 0
            ;;
    esac
done

# ── Helpers ──────────────────────────────────────────────────────────────────

info()  { echo "  ✓ $*"; }
warn()  { echo "  ⚠ $*"; }
fail()  { echo "  ✗ $*"; ERRORS=$((ERRORS + 1)); }
step()  { echo ""; echo "── $* ──"; }
dry()   { if $DRY_RUN; then echo "  [dry-run] $*"; return 0; fi; return 1; }

prompt_yn() {
    if $CI_MODE; then return 0; fi
    local prompt="$1 [Y/n] "
    read -r -p "$prompt" response
    case "$response" in
        [nN]*) return 1 ;;
        *) return 0 ;;
    esac
}

# ── Platform detection ───────────────────────────────────────────────────────

step "Detecting platform"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Darwin)  PLATFORM="macOS" ;;
    Linux)   PLATFORM="Linux" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="Windows" ;;
    *)       PLATFORM="Unknown" ;;
esac

info "$PLATFORM ($ARCH)"

# ── Check: C++20 compiler ───────────────────────────────────────────────────

step "Checking C++20 compiler"

if command -v clang++ &>/dev/null; then
    CLANG_VERSION=$(clang++ --version 2>&1 | head -1)
    info "clang++: $CLANG_VERSION"
elif command -v g++ &>/dev/null; then
    GCC_VERSION=$(g++ --version 2>&1 | head -1)
    info "g++: $GCC_VERSION"
else
    fail "No C++20 compiler found"
    if [ "$PLATFORM" = "macOS" ]; then
        echo "    Fix: xcode-select --install"
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Fix: sudo apt install g++-13  (or equivalent for your distro)"
    fi
fi

# macOS: check Xcode CLT
if [ "$PLATFORM" = "macOS" ]; then
    if xcode-select -p &>/dev/null; then
        info "Xcode Command Line Tools installed"
    else
        warn "Xcode Command Line Tools not found"
        if prompt_yn "Install Xcode Command Line Tools?"; then
            dry "xcode-select --install" || xcode-select --install
        else
            echo "    Manual fix: xcode-select --install"
        fi
    fi
fi

# ── Check: CMake ────────────────────────────────────────────────────────────

step "Checking CMake"

if command -v cmake &>/dev/null; then
    CMAKE_VERSION=$(cmake --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
    CMAKE_MAJOR=$(echo "$CMAKE_VERSION" | cut -d. -f1)
    CMAKE_MINOR=$(echo "$CMAKE_VERSION" | cut -d. -f2)

    if [ "$CMAKE_MAJOR" -gt 3 ] || { [ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -ge 24 ]; }; then
        info "cmake $CMAKE_VERSION"
    else
        fail "cmake $CMAKE_VERSION is too old (need 3.24+)"
        if [ "$PLATFORM" = "macOS" ]; then
            echo "    Fix: brew upgrade cmake"
        else
            echo "    Fix: install CMake 3.24+ from https://cmake.org/download/"
        fi
    fi
else
    fail "CMake not found"
    if [ "$PLATFORM" = "macOS" ]; then
        echo "    Fix: brew install cmake"
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Fix: sudo apt install cmake  (or snap install cmake --classic)"
    fi
fi

# ── Check: git-lfs ──────────────────────────────────────────────────────────

step "Checking git-lfs"

if command -v git-lfs &>/dev/null || git lfs version &>/dev/null; then
    GIT_LFS_VERSION=$(git lfs version 2>&1 | head -1)
    info "git-lfs: $GIT_LFS_VERSION"

    # Ensure git-lfs is initialized
    if ! git -C "$REPO_ROOT" lfs env 2>/dev/null | grep -q "Endpoint"; then
        info "Initializing git-lfs..."
        dry "git lfs install" || git lfs install
    fi
else
    fail "git-lfs not found"
    if [ "$PLATFORM" = "macOS" ]; then
        if command -v brew &>/dev/null; then
            if prompt_yn "Install git-lfs via Homebrew?"; then
                dry "brew install git-lfs && git lfs install" || {
                    brew install git-lfs
                    git lfs install
                }
                info "git-lfs installed"
            fi
        else
            echo "    Fix: brew install git-lfs && git lfs install"
            echo "    Or: https://git-lfs.com"
        fi
    elif [ "$PLATFORM" = "Linux" ]; then
        echo "    Fix: sudo apt install git-lfs && git lfs install"
    fi
fi

# ── Check: git-lfs files pulled ─────────────────────────────────────────────

step "Checking git-lfs files"

# Check if Skia files are actual binaries or just LFS pointers
SKIA_CHECK="$REPO_ROOT/external/skia-build"
if [ -d "$SKIA_CHECK" ]; then
    # Find any .a or .lib file and check if it's an LFS pointer
    LFS_POINTER=false
    for f in $(find "$SKIA_CHECK" -name "*.a" -o -name "*.lib" 2>/dev/null | head -1); do
        if [ -f "$f" ] && head -1 "$f" 2>/dev/null | grep -q "version https://git-lfs"; then
            LFS_POINTER=true
        fi
    done

    if $LFS_POINTER; then
        warn "Skia files are LFS pointers (not actual binaries)"
        echo "    Pulling LFS files..."
        dry "git -C $REPO_ROOT lfs pull" || git -C "$REPO_ROOT" lfs pull
        info "LFS files pulled"
    else
        info "Skia binaries present"
    fi
else
    info "Skia build directory not found (will be fetched during build if needed)"
fi

# ── Check: External SDKs ───────────────────────────────────────────────────

step "Setting up external SDKs"

# VST3 SDK
VST3_DIR="$REPO_ROOT/external/vst3sdk"
if [ -d "$VST3_DIR/pluginterfaces" ]; then
    info "VST3 SDK present"
else
    # Remove broken symlink if present
    [ -L "$VST3_DIR" ] && rm "$VST3_DIR"
    info "Cloning VST3 SDK (MIT license)..."
    dry "git clone --depth 1 --recursive https://github.com/steinbergmedia/vst3sdk.git $VST3_DIR" || \
        git clone --depth 1 --recursive --branch v3.7.12 \
            https://github.com/steinbergmedia/vst3sdk.git "$VST3_DIR"
    info "VST3 SDK ready"
fi

# AudioUnit SDK (macOS only)
if [ "$PLATFORM" = "macOS" ]; then
    AU_DIR="$REPO_ROOT/external/AudioUnitSDK"
    if [ -d "$AU_DIR/include" ]; then
        info "AudioUnitSDK present"
    else
        # Remove broken symlink if present
        [ -L "$AU_DIR" ] && rm "$AU_DIR"
        info "Cloning AudioUnitSDK (Apache 2.0)..."
        dry "git clone --depth 1 https://github.com/apple/AudioUnitSDK.git $AU_DIR" || \
            git clone --depth 1 https://github.com/apple/AudioUnitSDK.git "$AU_DIR"
        info "AudioUnitSDK ready"
    fi
fi

# Linux: check ALSA dev headers
if [ "$PLATFORM" = "Linux" ]; then
    if pkg-config --exists alsa 2>/dev/null; then
        info "ALSA dev headers present"
    else
        fail "ALSA dev headers not found"
        echo "    Fix: sudo apt install libasound2-dev"
    fi
fi

# ── Summary before build ───────────────────────────────────────────────────

if [ $ERRORS -gt 0 ]; then
    echo ""
    echo "Found $ERRORS issue(s). Fix them and re-run ./setup.sh"
    echo "Or run: pulp doctor  (after building)"
    exit 1
fi

if $DRY_RUN; then
    echo ""
    echo "Dry run complete. No changes were made."
    echo "Run ./setup.sh without --dry-run to execute."
    exit 0
fi

# ── Configure ───────────────────────────────────────────────────────────────

step "Configuring CMake"

cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Debug

# ── Build ───────────────────────────────────────────────────────────────────

step "Building"

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake --build "$REPO_ROOT/build" -j"$JOBS"

# ── Test ────────────────────────────────────────────────────────────────────

step "Running tests"

ctest --test-dir "$REPO_ROOT/build" --output-on-failure

# ── Report ──────────────────────────────────────────────────────────────────

step "Setup complete"

echo ""
echo "  Build artifacts:"

for fmt_dir in VST3 CLAP AU; do
    dir="$REPO_ROOT/build/$fmt_dir"
    if [ -d "$dir" ]; then
        for plugin in "$dir"/*; do
            [ -e "$plugin" ] && echo "    $fmt_dir: $(basename "$plugin")"
        done
    fi
done

# Find standalone binaries
for example_dir in "$REPO_ROOT/build/examples"/*/; do
    for bin in "$example_dir"Pulp* "$example_dir"pulp-*; do
        [ -x "$bin" ] && [ ! -d "$bin" ] && echo "    Standalone: $(basename "$bin")"
    done
done

echo ""
echo "  CLI:  $REPO_ROOT/build/tools/cli/pulp"
echo ""
echo "  Next steps:"
echo "    pulp build          # rebuild after changes"
echo "    pulp test           # run tests"
echo "    pulp doctor         # diagnose environment issues"
echo "    pulp validate       # validate plugin formats"
echo ""
