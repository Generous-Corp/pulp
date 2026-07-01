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
VER="${VER:-1.1.0}"
APP_ID="${APP_ID:-D10A184D5A207EAA926955447DC27E2AD965DFB8}"   # Developer ID Application
INST_ID="${INST_ID:-0E91CD0D8592220A75AE9D13D4031E36472EE58D}" # Developer ID Installer
OUT="${OUT:-/tmp/gpu-nam-release}"

# Assemble the optional sample-content tree the installer offers in its Customize
# pane: NAM example captures + a cabinet IR + a README, laid out as the plugin's
# file choosers expect (Models/, Cabinets/). Installed to
# /Library/Application Support/GPU NAM/ (see gpu_nam_content_dir()).
MODELS="$HERE/models"
CONTENT="$(mktemp -d)/GPU NAM"
mkdir -p "$CONTENT/Models" "$CONTENT/Cabinets"
cp "$MODELS/wavenet.nam"              "$CONTENT/Models/" 2>/dev/null \
  || cp "$MODELS/example.nam"         "$CONTENT/Models/wavenet.nam"
cp "$MODELS/wavenet_a1_standard.nam"  "$CONTENT/Models/"
cp "$MODELS/lstm.nam"                 "$CONTENT/Models/"
cp "$MODELS/cabinet.wav"              "$CONTENT/Cabinets/"
cp "$MODELS/README-content.txt"       "$CONTENT/README.txt"

args=(--name GpuNam --version "$VER"
      --sign-identity "$APP_ID" --installer-identity "$INST_ID" --out "$OUT"
      --plugin au   "$BUILD/AU/GpuNam.component"
      --plugin vst3 "$BUILD/VST3/GpuNam.vst3"
      --plugin clap "$BUILD/CLAP/GpuNam.clap"
      --app "Standalone app" "$BUILD/examples/gpu-nam/GpuNam.app"
      --content "Sample models & cabinet" \
                "NAM example captures (two WaveNet sizes + an LSTM) and a cabinet IR, installed to /Library/Application Support/GPU NAM. Optional — the plugin's file choosers open here first." \
                "/Library/Application Support/GPU NAM" "$CONTENT")

exec "$ROOT/tools/scripts/build_combined_installer.sh" "${args[@]}"
