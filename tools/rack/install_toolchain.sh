#!/usr/bin/env bash
# Install the Rack generator where the app can reach it, and prove it runs.
#
# Two reasons this is a script rather than a line in a README.
#
# The destination is deliberate. macOS gates removable-volume access behind a
# MODAL consent dialog, so an app resolving its toolchain to a checkout on an
# external volume parks its UI thread behind that dialog on the first build.
# Nothing under Application Support is gated.
#
# And the generator is not one directory. It shells out to a panel emitter that
# loads fonts from `../../external/fonts`, and rewrites a module pack at
# `../../examples/forge-modular`. Installing `tools/rack` alone leaves an app
# that spawns the generator, streams a traceback, and looks broken — which is
# exactly what happened. So the copy is defined here, once, and verified by
# actually emitting a panel rather than by checking that files exist.

set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="${FORGE_MODULAR_HOME:-$HOME/Library/Application Support/Forge Modular}"

# Everything the generator reaches for, relative to the repo root.
PARTS=(
  "tools/rack"                 # the generator itself
  "external/fonts"             # shape_text loads Inter from here
  "examples/forge-modular"     # the module pack it edits and rebuilds
  # The pack compiles against Pulp headers. Without these the generator gets
  # all the way to a validated panel and then fails at the compiler, which is
  # the most expensive place to discover a missing file: the model has already
  # been called. Kept to the six trees the pack's CMakeLists names, not the
  # whole repo.
  "core/signal/include"
  "core/format/include"
  "core/audio/include"
  "core/state/include"
  "core/platform/include"
  "core/runtime/include"
)

echo "installing the Rack toolchain"
echo "  from: $SRC"
echo "  to:   $DEST"

for part in "${PARTS[@]}"; do
  if [ ! -e "$SRC/$part" ]; then
    echo "  MISSING in source: $part" >&2
    exit 1
  fi
  mkdir -p "$DEST/$(dirname "$part")"
  rsync -a --delete \
    --exclude __pycache__ --exclude '*.pyc' \
    --exclude build --exclude '*.o' \
    "$SRC/$part/" "$DEST/$part/"
  printf '  %-26s %s\n' "$part" "$(du -sh "$DEST/$part" | awk '{print $1}')"
done

# The acceptance test is a real panel, not a file listing. A complete-looking
# tree that cannot emit an SVG is the failure this script exists to prevent.
echo "verifying"
MODULES="$DEST/examples/forge-modular/modules"
PLUGIN="$MODULES/_plugin.json"
# Picked with a glob rather than `ls | grep | head`: under `pipefail`, head
# closing the pipe early makes ls die on SIGPIPE and the script exits 141
# having verified nothing.
SAMPLE=""
for f in "$MODULES"/*.json; do
  case "$(basename "$f")" in _plugin.json) continue ;; esac
  SAMPLE="$f"
  break
done
if [ -z "${SAMPLE:-}" ]; then
  echo "  no module manifest to verify against" >&2
  exit 1
fi

if (cd "$DEST/tools/rack" && python3 forge_modular.py panels "$PLUGIN" "$SAMPLE" >/dev/null 2>&1); then
  echo "  emitted a panel — toolchain is complete"
else
  echo "  FAILED to emit a panel from the installed copy:" >&2
  (cd "$DEST/tools/rack" && python3 forge_modular.py panels "$PLUGIN" "$SAMPLE" 2>&1 | tail -5) >&2
  exit 1
fi
