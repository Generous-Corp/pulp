#!/bin/bash
# Pulp development environment setup
# Run this in any worktree to set up dependencies

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Setting up Pulp development environment..."
echo "Repo root: $REPO_ROOT"

# Clone VST3 SDK if not present
if [ ! -d "$REPO_ROOT/external/vst3sdk/pluginterfaces" ]; then
    echo "Cloning VST3 SDK (MIT license)..."
    git clone --depth 1 --recursive \
        https://github.com/steinbergmedia/vst3sdk.git \
        "$REPO_ROOT/external/vst3sdk"
    echo "VST3 SDK ready."
else
    echo "VST3 SDK already present."
fi

# Configure and build
echo ""
echo "Configuring CMake..."
cmake -B "$REPO_ROOT/build" "$REPO_ROOT"

echo ""
echo "Building..."
cmake --build "$REPO_ROOT/build" -- -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

echo ""
echo "Running tests..."
ctest --test-dir "$REPO_ROOT/build" --output-on-failure

echo ""
echo "Setup complete."
echo ""
echo "Outputs:"
if [ -d "$REPO_ROOT/build/VST3" ]; then
    echo "  VST3:       $REPO_ROOT/build/VST3/"
    ls "$REPO_ROOT/build/VST3/" 2>/dev/null
fi
if [ -f "$REPO_ROOT/build/examples/pulp-gain/PulpGain" ]; then
    echo "  Standalone:  $REPO_ROOT/build/examples/pulp-gain/PulpGain"
fi
if [ -f "$REPO_ROOT/build/examples/pulp-tone/PulpTone" ]; then
    echo "  Standalone:  $REPO_ROOT/build/examples/pulp-tone/PulpTone"
fi
echo ""
echo "To install VST3 to system:"
echo "  cp -R build/VST3/PulpGain.vst3 ~/Library/Audio/Plug-Ins/VST3/"
