#!/usr/bin/env bash
# Build the panel-lettering shaper: tools/rack/build_shape_text.sh
#
# The emitter shells out to this binary for every label on every panel, and the
# SVGs it produces are committed. So it is a build input for tracked artefacts,
# and it had no build recipe anywhere -- it was compiled by hand into /tmp and
# the emitter defaulted to looking for it there.
#
# macOS clears /tmp. When it did, `forge_modular.py` stopped with
# `FileNotFoundError: '/tmp/shape_text'` and there was nothing in the repo
# describing how to get it back. Panels could not be regenerated from a clone
# at all, which for generated-and-committed files means they could drift with
# no way to prove it.
#
# So: a recipe, and it builds into build/ where a rebuild survives a reboot.
#
#   tools/rack/build_shape_text.sh            # find Skia, build build/shape_text
#   SKIA_DIR=/path/to/skia-build/build/mac-gpu tools/rack/build_shape_text.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$ROOT/build/shape_text"

die() { echo "build_shape_text: $*" >&2; exit 1; }

# Skia arrives as prebuilt static libraries, and a checkout may carry only the
# headers -- which compiles and then fails at link, so the libraries are what
# is looked for, never the include directory.
find_skia() {
    if [ -n "${SKIA_DIR:-}" ]; then
        [ -f "$SKIA_DIR/lib/Release/libskshaper.a" ] && { echo "$SKIA_DIR"; return; }
        die "SKIA_DIR is set but has no lib/Release/libskshaper.a: $SKIA_DIR"
    fi
    local candidate
    for candidate in "$ROOT/external/skia-build/build/mac-gpu" \
                     "$ROOT"/../*/external/skia-build/build/mac-gpu; do
        [ -f "$candidate/lib/Release/libskshaper.a" ] && { echo "$candidate"; return; }
    done
    return 1
}

SKIA="$(find_skia)" || die "no Skia build found.
       This checkout has headers only (external/skia-build/include) and the
       shaper needs the static libraries. Point SKIA_DIR at a populated
       skia-build/build/<platform> directory."

INC="$ROOT/external/skia-build"
[ -f "$INC/include/core/SkFont.h" ] || INC="$SKIA/../.."
[ -f "$INC/include/core/SkFont.h" ] || die "no Skia headers found near $SKIA"

mkdir -p "$(dirname "$OUT")"
echo "  skia:    $SKIA"
echo "  headers: $INC"

# skunicode is split into a core and an ICU backend, and the shaper needs both:
# linking core alone gets past the compile and fails on ICU symbols.
clang++ -std=c++20 -O2 -o "$OUT" "$HERE/shape_text.cpp" \
    -I"$INC" -I"$INC/modules" \
    "$SKIA/lib/Release/libskshaper.a" \
    "$SKIA/lib/Release/libskunicode_icu.a" \
    "$SKIA/lib/Release/libskunicode_core.a" \
    "$SKIA/lib/Release/libskia.a" \
    -framework CoreFoundation -framework CoreGraphics -framework CoreText \
    -framework CoreServices -framework AppKit \
    || die "compile failed"

echo "built $OUT"

# Prove it runs, not just that it linked. A shaper that produces no path would
# emit panels with no lettering, and every label would silently vanish.
d="$("$OUT" "Hg" "$ROOT/external/fonts/Inter-Regular.ttf" 3.0 center 2>/dev/null)"
case "$d" in
    M*) echo "  shapes text: $(printf '%.40s' "$d")…" ;;
    *)  die "the binary built but produced no path for 'Hg' — it links and
       does not work, which would empty every label on every panel" ;;
esac
