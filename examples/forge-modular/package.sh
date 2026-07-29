#!/usr/bin/env bash
# Package Forge Modular: the app, the three plugin formats, and the Rack
# modules, in one component-selectable installer.
#
# The .vcvplugin ships in the same installer as the app deliberately. They are
# two payloads of one product -- generate a module in the app, and it lands in
# the plugin Rack already has -- and asking somebody to install two things to
# get one is the kind of friction that loses people at the first step.
#
# It also keeps the licence boundary where it belongs: the .vcvplugin is the
# only artifact that links the GPLv3 Rack SDK. The app never does; it invokes a
# compiler. Two payloads in one installer preserves that distinction, where one
# merged bundle would blur it.
#
#   package.sh --build-dir DIR --out DIR [--sign] [--notarize]
#
# Unsigned by default. Signing and notarization reach Apple's servers, so they
# are explicit rather than implied -- an unsigned .pkg is exactly what you want
# while proving the packaging itself works.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR=""
OUT_DIR=""
VERSION="0.1.0"
DO_SIGN=0
DO_NOTARIZE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --sign) DO_SIGN=1; shift ;;
        --notarize) DO_SIGN=1; DO_NOTARIZE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ -n "$BUILD_DIR" ]] || { echo "--build-dir is required" >&2; exit 2; }
[[ -n "$OUT_DIR" ]] || { echo "--out is required" >&2; exit 2; }

APP="$BUILD_DIR/examples/forge-modular/Forge Modular.app"
AU="$BUILD_DIR/AU/Forge Modular.component"
VST3="$BUILD_DIR/VST3/Forge Modular.vst3"
CLAP="$BUILD_DIR/CLAP/Forge Modular.clap"
RACK_PLUGIN=$(ls "$BUILD_DIR"/rack/ForgeModular-*.vcvplugin 2>/dev/null | head -1 || true)

missing=0
for artifact in "$APP" "$AU" "$VST3" "$CLAP"; do
    if [[ ! -e "$artifact" ]]; then
        echo "missing: $artifact" >&2
        missing=1
    fi
done
if [[ -z "$RACK_PLUGIN" ]]; then
    # Not fatal: the app is useful without it, and somebody may be packaging
    # only the DAW side. But it is worth saying, because an installer that
    # quietly omits the modules looks identical to one that includes them.
    echo "note: no .vcvplugin found in $BUILD_DIR/rack — the installer will" >&2
    echo "      carry the app and plugins but NOT the Rack modules" >&2
fi
[[ $missing -eq 0 ]] || { echo "build the four targets first" >&2; exit 1; }

mkdir -p "$OUT_DIR"

if [[ $DO_SIGN -eq 0 ]]; then
    # Unsigned: assemble the payload with pkgbuild directly so the shape can be
    # proven without credentials. macOS will refuse to open it without a
    # right-click, which is correct and expected for an unsigned build.
    STAGE="$(mktemp -d)"
    trap 'rm -rf "$STAGE"' EXIT

    mkdir -p "$STAGE/Applications" \
             "$STAGE/Library/Audio/Plug-Ins/Components" \
             "$STAGE/Library/Audio/Plug-Ins/VST3" \
             "$STAGE/Library/Audio/Plug-Ins/CLAP"
    cp -R "$APP" "$STAGE/Applications/"
    cp -R "$AU" "$STAGE/Library/Audio/Plug-Ins/Components/"
    cp -R "$VST3" "$STAGE/Library/Audio/Plug-Ins/VST3/"
    cp -R "$CLAP" "$STAGE/Library/Audio/Plug-Ins/CLAP/"

    if [[ -n "$RACK_PLUGIN" ]]; then
        # Rack reads from the user's Application Support, which a package
        # payload cannot address, so the modules ride along inside the app and
        # a postinstall step is what places them. Copying here would install
        # them for root and nobody else.
        mkdir -p "$STAGE/Applications/Forge Modular.app/Contents/Resources/rack"
        cp "$RACK_PLUGIN" \
           "$STAGE/Applications/Forge Modular.app/Contents/Resources/rack/"
    fi

    PKG="$OUT_DIR/ForgeModular-$VERSION-unsigned.pkg"
    pkgbuild --root "$STAGE" \
             --identifier com.generous.forgemodular \
             --version "$VERSION" \
             --install-location / \
             "$PKG"
    echo "$PKG"
    exit 0
fi

# Signed and optionally notarized: the canonical Pulp recipe, which deep-signs
# each bundle inner-first and refuses to package one that is not relocatable.
ARGS=(--name "Forge Modular" --version "$VERSION" --out "$OUT_DIR"
      --app "Forge Modular" "$APP"
      --plugin au "$AU" --plugin vst3 "$VST3" --plugin clap "$CLAP")
[[ $DO_NOTARIZE -eq 1 ]] && ARGS+=(--notarize)

exec "$REPO/tools/scripts/build_combined_installer.sh" "${ARGS[@]}"
