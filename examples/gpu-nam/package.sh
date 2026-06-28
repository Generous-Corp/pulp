#!/bin/bash
# GPU NAM release: invoke the generic Pulp combined-installer recipe
# (tools/scripts/build_combined_installer.sh) — ONE signed + notarized installer
# whose Customize pane offers the standalone app and the AU/VST3/CLAP plugins.
# This file is just the GPU-NAM-specific inputs; the packaging logic lives in the
# shared tool so every plugin uses the same one.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
VER="${VER:-1.0.0}"
APP_ID="${APP_ID:-D10A184D5A207EAA926955447DC27E2AD965DFB8}"   # Developer ID Application
INST_ID="${INST_ID:-0E91CD0D8592220A75AE9D13D4031E36472EE58D}" # Developer ID Installer
OUT="${OUT:-/tmp/gpu-nam-release}"

args=(--name GpuNam --version "$VER"
      --sign-identity "$APP_ID" --installer-identity "$INST_ID" --out "$OUT"
      --plugin au   "$BUILD/AU/GpuNam.component"
      --plugin vst3 "$BUILD/VST3/GpuNam.vst3"
      --plugin clap "$BUILD/CLAP/GpuNam.clap"
      --app "Standalone app" "$BUILD/examples/gpu-nam/GpuNam.app")

exec "$ROOT/tools/scripts/build_combined_installer.sh" "${args[@]}"
