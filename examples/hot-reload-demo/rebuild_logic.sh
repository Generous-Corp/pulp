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

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here"
while [[ "$root" != "/" && ! -f "$root/tools/ci/governed-build.sh" ]]; do root="$(dirname "$root")"; done
if [[ ! -f "$root/tools/ci/governed-build.sh" ]]; then
    echo "error: could not locate the Pulp checkout root" >&2
    exit 1
fi
GOVERNED_BUILD="$root/tools/ci/governed-build.sh"
BUILD_DIR="${PULP_BUILD_DIR:-$root/build}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: no build dir found (set PULP_BUILD_DIR)" >&2
    exit 1
fi

echo "Rebuilding hot-reload logic via $BUILD_DIR ..."
bash "$GOVERNED_BUILD" cmake --build "$BUILD_DIR" --target hot-reload-demo-logic
echo "Published: $HOME/.pulp/hot-reload-demo/logic.dylib"
echo "If a host has the plugin loaded, the DSP just hot-swapped."
