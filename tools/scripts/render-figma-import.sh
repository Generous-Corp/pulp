#!/usr/bin/env bash
# render-figma-import.sh — render a pulp-import-design output at the
# right canvas size automatically.
#
# Usage: render-figma-import.sh <ui.js> <out.png> [extra pulp-screenshot args...]
#
# Reads <ui.js>.meta.json (emitted by pulp-import-design) to pick
# --width / --height from the source design's root frame, so callers
# don't have to remember canvas dimensions.

set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <ui.js> <out.png> [extra pulp-screenshot args]" >&2
  exit 64
fi

SCRIPT="$1"; shift
OUT="$1"; shift

META="${SCRIPT}.meta.json"
WIDTH=1000
HEIGHT=600
if [[ -f "$META" ]]; then
  # Tiny inline JSON pluck — no jq dependency required.
  WIDTH=$(python3 -c "import json,sys;d=json.load(open('$META'));print(d['canvas']['width'])")
  HEIGHT=$(python3 -c "import json,sys;d=json.load(open('$META'));print(d['canvas']['height'])")
else
  echo "[render-figma-import] no sidecar at $META — falling back to ${WIDTH}x${HEIGHT}" >&2
fi

# Find pulp-screenshot relative to this script (../../build/...).
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SHOT="${PULP_SCREENSHOT_BIN:-${REPO_ROOT}/build/tools/screenshot/pulp-screenshot}"
if [[ ! -x "$SHOT" ]]; then
  echo "[render-figma-import] pulp-screenshot not found at $SHOT — set PULP_SCREENSHOT_BIN" >&2
  exit 1
fi

echo "[render-figma-import] $SCRIPT @ ${WIDTH}x${HEIGHT} -> $OUT"
"$SHOT" --width "$WIDTH" --height "$HEIGHT" --script "$SCRIPT" --output "$OUT" "$@"
