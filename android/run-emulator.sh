#!/bin/bash
# Launch Android emulator with correct flags for Pulp development.
# MUST use -gpu host (not swiftshader) and QEMU_AUDIO_DRV=coreaudio.
#
# Without these:
# - swiftshader starves CPU → audio HAL dies with pcm_writei I/O errors
# - Default audio backend has broken pipe to macOS speakers

set -e

AVD="${1:-Medium_Phone_API_36.1}"
EMULATOR="$HOME/Library/Android/sdk/emulator/emulator"

echo "Starting emulator: $AVD"
echo "  GPU: host (Metal/MoltenVK)"
echo "  Audio: coreaudio"
echo ""

QEMU_AUDIO_DRV=coreaudio "$EMULATOR" \
    -avd "$AVD" \
    -gpu host \
    -no-snapshot \
    "$@"
