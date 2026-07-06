#!/bin/bash
# SuperConvolver release: invoke the generic Pulp combined-installer recipe
# (tools/scripts/build_combined_installer.sh) — ONE signed + notarized installer
# whose Customize pane offers the standalone app, AU/VST3/CLAP, and (when built)
# the Diagnostics helper. This file is just the SuperConvolver-specific inputs;
# the packaging logic lives in the shared tool so every plugin uses the same one.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
VER="${VER:-1.0.3}"
APP_ID="${APP_ID:-D10A184D5A207EAA926955447DC27E2AD965DFB8}"   # Developer ID Application
INST_ID="${INST_ID:-0E91CD0D8592220A75AE9D13D4031E36472EE58D}" # Developer ID Installer
OUT="${OUT:-/tmp/sc-release}"
DIAG_APP="${DIAG_APP:-/Volumes/Workshop/Code/pulp-diagnostickit/build/SuperConvolver Diagnostics.app}"
DIAG_ENT="${DIAG_ENT:-/Volumes/Workshop/Code/pulp-diagnostickit/DiagnosticKit.entitlements}"

args=(--name SuperConvolver --version "$VER"
      --sign-identity "$APP_ID" --installer-identity "$INST_ID" --out "$OUT"
      --plugin au   "$BUILD/AU/SuperConvolver.component"
      --plugin vst3 "$BUILD/VST3/SuperConvolver.vst3"
      --plugin clap "$BUILD/CLAP/SuperConvolver.clap"
      --app "Standalone app" "$BUILD/examples/super-convolver/SuperConvolver.app")
[[ -d "$DIAG_APP" ]] && args+=(--app "Diagnostics helper" "$DIAG_APP" "$DIAG_ENT")

exec "$ROOT/tools/scripts/build_combined_installer.sh" "${args[@]}"
