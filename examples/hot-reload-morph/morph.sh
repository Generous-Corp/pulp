#!/usr/bin/env bash
# Swap the running Hot-Reload Morph plugin between its two versions by copying a
# prebuilt logic variant over the watched path. The shell's watcher picks it up
# (~150ms) and hot-swaps BOTH the DSP and the editor — no reload, no dropout.
#   morph.sh warm    # sine tremolo + blue WARM editor
#   morph.sh harsh   # square chop  + red HARSH editor
set -euo pipefail
which="${1:-warm}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$here"; while [[ "$root" != "/" && ! -d "$root/build" ]]; do root="$(dirname "$root")"; done
BUILD_DIR="${PULP_BUILD_DIR:-$root/build}"
DEST="$HOME/.pulp/hot-reload-morph/logic.dylib"
case "$which" in
  warm)  SRC="$BUILD_DIR/examples/hot-reload-morph/logic.dylib" ;;
  harsh) SRC="$BUILD_DIR/examples/hot-reload-morph/logic-harsh.dylib" ;;
  *) echo "usage: morph.sh [warm|harsh]" >&2; exit 1 ;;
esac
[[ -f "$SRC" ]] || { echo "error: $SRC not built (cmake --build build --target hot-reload-morph-logic-warm hot-reload-morph-logic-harsh)" >&2; exit 1; }
mkdir -p "$(dirname "$DEST")"
cp "$SRC" "$DEST"
echo "morphed → $which ($DEST). A loaded plugin hot-swaps its DSP + editor now."
