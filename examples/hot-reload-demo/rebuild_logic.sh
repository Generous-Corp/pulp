#!/usr/bin/env bash
# Recompile the hot-reload demo's DSP "logic" library and publish it to the path
# the loaded shell watches ($HOME/.pulp/hot-reload-demo/logic.dylib). Edit
# logic_tremolo.cpp first (e.g. flip kWaveform), then run this while the plugin
# is loaded in a host — the DSP hot-swaps live, no reload, no audio dropout.
#
# It builds through the SAME CMake build that produced the shell, so the logic's
# build fingerprint matches the shell's; a mismatched build is rejected by the
# reload transaction (the sound keeps playing on the previous DSP).
set -euo pipefail

BUILD_DIR="${PULP_BUILD_DIR:-}"
if [[ -z "$BUILD_DIR" ]]; then
    # Walk up to the repo root (the dir containing CMakeLists.txt + build/).
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    root="$here"
    while [[ "$root" != "/" && ! -d "$root/build" ]]; do root="$(dirname "$root")"; done
    BUILD_DIR="$root/build"
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: no build dir found (set PULP_BUILD_DIR)" >&2
    exit 1
fi

echo "Rebuilding hot-reload logic via $BUILD_DIR ..."
cmake --build "$BUILD_DIR" --target hot-reload-demo-logic
echo "Published: $HOME/.pulp/hot-reload-demo/logic.dylib"
echo "If a host has the plugin loaded, the DSP just hot-swapped."
