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

# This tree is recursively chmodded and receives rsync --delete below. Resolve
# it once and reject targets where either operation could escape the product's
# own directory. In particular, an environment typo must never turn "$HOME" or
# "/" into the toolchain, and an existing symlink is not a destination at all.
DEST="$(python3 - "$DEST" "$HOME" "$SRC" <<'PY'
import os
import pathlib
import sys

raw = os.path.abspath(os.path.expanduser(sys.argv[1]))
home = os.path.realpath(os.path.abspath(os.path.expanduser(sys.argv[2])))
source = os.path.realpath(os.path.abspath(sys.argv[3]))
if os.path.islink(raw):
    raise SystemExit(f"unsafe Forge Modular destination is a symlink: {raw}")
resolved = os.path.realpath(raw)
parts = pathlib.Path(resolved).parts
if resolved in ("/", home, source) or len(parts) < 4:
    raise SystemExit(f"unsafe Forge Modular destination is too broad: {resolved}")
print(resolved)
PY
)"

# Everything the generator reaches for, relative to the repo root.
PARTS=(
  "tools/rack"                 # the generator itself
  "tools/dsp_vocabulary.py"    # the list of DSP the model is allowed to use
  "docs/status/agent-capabilities.json" # its checked capability source
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

# A PREVIOUS install may have left an unwritable copy behind. This is not
# hypothetical: before the chmod pass below existed, the first install on a clean
# machine copied the read-only modes of the app bundle and then failed part way
# through -- and every later attempt failed the same way, because rsync cannot
# overwrite what it cannot write. Reclaiming the destination first makes the
# fix arrive on machines that already have the broken copy.
[ -d "$DEST" ] && chmod -R u+rwX "$DEST" 2>/dev/null

echo "installing the Rack toolchain"
echo "  from: $SRC"
echo "  to:   $DEST"

for part in "${PARTS[@]}"; do
  if [ ! -e "$SRC/$part" ]; then
    echo "  MISSING in source: $part" >&2
    exit 1
  fi
  mkdir -p "$DEST/$(dirname "$part")"
  if [ -d "$SRC/$part" ]; then
    # --delete everywhere EXCEPT the module pack. The pack is where generated
    # work lands -- patches/ and the generated module sources -- and deleting
    # whatever the source tree lacks destroys precisely that. It did: a module
    # built from the app opened in Rack once, and the next toolchain reinstall
    # removed its .vcv, after which Open in Rack reported a file that was not
    # there. rsync -a also stamps the directory with the SOURCE's mtime, which
    # is what made the deletion look like it had never happened.
    # The module pack holds GENERATED state, and it has to stay internally
    # consistent: plugin.json, modules/*.json and src/generated_modules.hpp
    # describe each other, and patches/ names what they define. Rack refuses to
    # load the WHOLE plugin over one mismatch --
    #   "Manifest contains module DIV but it is not defined in plugin"
    # takes all 29 modules with it.
    #
    # A first attempt only dropped --delete, which was worse than useless: the
    # generated manifests survived while the generated SOURCE was overwritten
    # from the tree, which is precisely how that mismatch was created. So the
    # generated set is left alone entirely, and only the parts a toolchain
    # update genuinely needs to refresh are synced.
    # An exclude FILE rather than an array: macOS ships bash 3.2, where an
    # empty array expanded under `set -u` is an unbound variable and aborts the
    # install.
    # The modes this produces are the SOURCE's, and the source is read-only:
    # see the chmod pass after this loop, which is what makes the copy
    # writable.
    exclude_file="$(mktemp)"
    printf '%s\n' __pycache__ '*.pyc' build '*.o' > "$exclude_file"
    # THE TOOLCHAIN STAMP IS NOT ORDINARY CONTENT.
    #
    # It says which release laid this toolchain down, and the app compares it
    # against the one inside the bundle to decide which copy runs. A source
    # CHECKOUT has no stamp, so a --delete sync from one would remove the
    # destination's -- which would make a developer's hand-installed copy look
    # older than the release and lose to it. So: copy a stamp when the source
    # has one (an install from inside the bundle), and leave the destination's
    # alone when it does not.
    source_has_stamp=0
    if [ "$part" = "tools/rack" ]; then
      if [ -f "$SRC/$part/FORGE_TOOLCHAIN_STAMP" ]; then
        source_has_stamp=1
      else
        # A checkout has no release stamp. Preserve either the current stamp
        # or its one known legacy predecessor so a developer refresh does not
        # make an installed release look older than the bundle it came from.
        printf '%s\n' FORGE_TOOLCHAIN_STAMP VERSION >> "$exclude_file"
      fi
    fi
    if [ "$part" = "examples/forge-modular" ]; then
      # Refresh every committed built-in as one coherent set. Generated modules
      # explicitly identify themselves in their manifest and are overlaid only
      # after that refresh; preserving the whole old pack made a new toolchain
      # stamp claim stale built-ins were current.
      preserve_root="$(mktemp -d)"
      mkdir -p "$preserve_root/modules" "$preserve_root/src" \
               "$preserve_root/res" "$preserve_root/patches" \
               "$preserve_root/legacy"
      if [ -d "$DEST/$part/patches" ]; then
        rsync -a "$DEST/$part/patches/" "$preserve_root/patches/"
      fi
      for manifest in "$DEST/$part/modules"/*.json; do
        [ -f "$manifest" ] || continue
        base="$(basename "$manifest")"
        [ "$base" = "_plugin.json" ] && continue
        if grep -Eq '"forge_generated"[[:space:]]*:[[:space:]]*true' "$manifest"; then
          cp "$manifest" "$preserve_root/modules/$base"
          slug="$(sed -n 's/.*"slug"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)"
          [ -n "$slug" ] && [ -f "$DEST/$part/src/$slug.cpp" ] && \
            cp "$DEST/$part/src/$slug.cpp" "$preserve_root/src/"
          [ -n "$slug" ] && [ -f "$DEST/$part/res/$slug.svg" ] && \
            cp "$DEST/$part/res/$slug.svg" "$preserve_root/res/"
        elif [ ! -f "$SRC/$part/modules/$base" ]; then
          # Old generators did not mark their output. Keep the only copy for
          # recovery, but do not activate an unclassified file as a built-in
          # under the new release stamp.
          cp "$manifest" "$preserve_root/legacy/$base"
        fi
      done
      printf '%s\n' 'patches/' 'user-module-backup/' >> "$exclude_file"
      rsync -a --delete --exclude-from="$exclude_file" \
        "$SRC/$part/" "$DEST/$part/"
      rsync -a "$preserve_root/modules/" "$DEST/$part/modules/"
      rsync -a "$preserve_root/src/" "$DEST/$part/src/"
      rsync -a "$preserve_root/res/" "$DEST/$part/res/"
      rsync -a "$preserve_root/patches/" "$DEST/$part/patches/"
      legacy_found=0
      for legacy_file in "$preserve_root/legacy"/*; do
        [ -f "$legacy_file" ] && legacy_found=1
      done
      if [ "$legacy_found" -eq 1 ]; then
        legacy="$DEST/$part/user-module-backup/$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$legacy"
        rsync -a "$preserve_root/legacy/" "$legacy/"
        echo "  preserved unclassified legacy module manifests at $legacy"
      fi
      rm -rf "$preserve_root"
    else
      rsync -a --delete --exclude-from="$exclude_file" \
        "$SRC/$part/" "$DEST/$part/"
    fi
    if [ "$part" = "tools/rack" ] && [ "$source_has_stamp" -eq 1 ]; then
      # Migration is deliberately exact. Once the new stamp is present, only
      # the old collision-prone filename is obsolete; generated patches,
      # modules, caches and unrelated user/private state are not migration
      # targets.
      rm -f "$DEST/$part/VERSION"
    fi
    rm -f "$exclude_file"
  else
    cp "$SRC/$part" "$DEST/$part"
    chmod u+rw "$DEST/$part"
  fi
  printf '  %-26s %s\n' "$part" "$(du -sh "$DEST/$part" | awk '{print $1}')"
done

# AND MAKE THE COPY WRITABLE, which is not what it was copied from.
#
# An installed app bundle is root-owned and sealed, and `rsync -a` faithfully
# reproduces its read-only modes -- so on the first build on a clean machine
# the working copy of the module pack arrived r--r--r-- and the panel emitter
# died on `PermissionError: .../res/ATT.svg`, having installed everything and
# verified nothing. This is the copy a generation REWRITES, so it has to be
# writable whatever it came from.
#
# Done here rather than with rsync's --chmod, which macOS's openrsync ACCEPTS
# AND IGNORES: the flag produced no error, no warning, and no change to a
# single mode bit. A tool that does what it says, afterwards, is the only way
# to know.
chmod -R u+rwX "$DEST"

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

# The panel shaper. Every label on every panel is outlined by it, so an
# installed toolchain without one cannot emit a panel at all -- it gets as far
# as a model call and dies. It is a built binary rather than a source file, so
# it is not in PARTS; it is copied when the source tree has one.
#
# Not built here on purpose: building it needs a populated Skia checkout, which
# a machine being set up from scratch is exactly the machine that lacks. Saying
# what is missing and how to get it beats a twenty-minute detour into Skia.
if [ -x "$SRC/build/shape_text" ]; then
  mkdir -p "$DEST/build"
  cp "$SRC/build/shape_text" "$DEST/build/shape_text"
  chmod u+rwx "$DEST/build/shape_text"
  echo "  copied the panel shaper"
else
  echo "  FAILED: no panel shaper at $SRC/build/shape_text" >&2
  echo "         Build it first:  tools/rack/build_shape_text.sh" >&2
  echo "         Without it the installed toolchain cannot letter a panel," >&2
  echo "         which surfaces as a failed generation after a model call." >&2
  exit 1
fi

# Re-derive the plugin manifest, registrations, generated header and panels
# from the refreshed built-ins plus the explicitly preserved generated set.
# This is the atomic consistency boundary Rack requires.
if ! (cd "$DEST/tools/rack" && python3 forge_modular.py all "$MODULES"/*.json >/dev/null 2>&1); then
  echo "  FAILED to rebuild the merged module pack after upgrade" >&2
  exit 1
fi

# The DSP vocabulary is injected into the contract at call time. When it comes
# back empty the model is handed a prompt with no list of available DSP, so it
# invents headers that do not exist and hand-rolls everything -- and the run
# dies at the compiler, three model calls later. An empty string is a silent
# failure, so it is checked here rather than discovered there.
VOCAB_LINES="$(cd "$DEST/tools/rack" && python3 ../dsp_vocabulary.py 2>/dev/null | wc -l | tr -d ' ')"
if [ "${VOCAB_LINES:-0}" -lt 20 ]; then
  echo "  FAILED: the DSP vocabulary is empty or tiny ($VOCAB_LINES lines)" >&2
  exit 1
fi
echo "  DSP vocabulary: $VOCAB_LINES lines"

if (cd "$DEST/tools/rack" && python3 forge_modular.py panels "$PLUGIN" "$SAMPLE" >/dev/null 2>&1); then
  echo "  emitted a panel — toolchain is complete"
else
  echo "  FAILED to emit a panel from the installed copy:" >&2
  (cd "$DEST/tools/rack" && python3 forge_modular.py panels "$PLUGIN" "$SAMPLE" 2>&1 | tail -5) >&2
  exit 1
fi
