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

# THERE ARE TWO FORGE MODULAR APPS, and this script shipped the wrong one.
#
#   $BUILD_DIR/modular/…              the Forge worktree build. The real shell.
#   $BUILD_DIR/examples/forge-modular/ a separate, much older example shell.
#
# The real UI lives in forge-seam/modular/modular_shell.cpp and is compiled in
# a Forge worktree (forge-seam/populate.sh -> /tmp/forge-cur), NOT in the pulp
# tree. Hardcoding the examples/ path meant no --build-dir value could ever
# reach it, so a 0.12.0 installer shipped a build with none of the product's
# UI in it and every payload the right SIZE. Size was never the question.
#
# Prefer the Forge layout, fall back to the pulp one, and SAY WHICH -- a
# silent choice between two apps of the same name is what caused this.
if [[ -n "${APP_OVERRIDE:-}" ]]; then
    APP="$APP_OVERRIDE"
elif [[ -d "$BUILD_DIR/modular/Forge Modular.app" ]]; then
    APP="$BUILD_DIR/modular/Forge Modular.app"
else
    APP="$BUILD_DIR/examples/forge-modular/Forge Modular.app"
fi
echo "[installer] app: $APP"

# THE GENERATOR MUST SHIP. The 0.12.1 package carried the app, the plugins and
# the Rack modules, and ZERO python -- so on a machine that had never seen the
# source, Build did nothing. It only ever worked on the build machine because a
# manual step had written the tools into Application Support by hand.
#
# Staged into a COPY of the bundle, before signing, so the signature covers
# them and the source tree is never mutated by packaging.
STAGED_ROOT="$(mktemp -d)"
trap 'rm -rf "$STAGED_ROOT"' EXIT
ditto "$APP" "$STAGED_ROOT/$(basename "$APP")"
APP="$STAGED_ROOT/$(basename "$APP")"
TOOLS_DEST="$APP/Contents/Resources/tools/rack"
mkdir -p "$TOOLS_DEST"
ditto "$REPO/tools/rack" "$TOOLS_DEST"
# The uninstaller and the SDK fetch live beside the app that offers them.
for helper in uninstall.sh fetch_rack_sdk.sh; do
    if [[ -f "$REPO/examples/forge-modular/$helper" ]]; then
        ditto "$REPO/examples/forge-modular/$helper" "$APP/Contents/Resources/$helper"
        chmod +x "$APP/Contents/Resources/$helper"
    fi
done
[[ -f "$TOOLS_DEST/patch.py" ]] || { echo "staging failed: no patch.py" >&2; exit 1; }
echo "[installer] staged $(ls "$TOOLS_DEST"/*.py | wc -l | tr -d ' ') generator files + helpers"
AU="$BUILD_DIR/AU/Forge Modular.component"
VST3="$BUILD_DIR/VST3/Forge Modular.vst3"
CLAP="$BUILD_DIR/CLAP/Forge Modular.clap"
RACK_PLUGIN=$(ls "$BUILD_DIR"/rack/ForgeModular-*.vcvplugin 2>/dev/null | head -1 || true)

# A bundle EXISTING is not a bundle with anything in it.
#
# CMake creates the .component/.vst3/.clap directory tree -- Info.plist,
# Resources, an empty Contents/MacOS -- as soon as the target is configured,
# whether or not it was ever built. `-e` is true for that husk, so packaging a
# build dir where the plugin targets had never run produced three 72 KB
# payloads that signed, notarized and installed while containing no code at
# all. The app was 17 MB and the plugins were empty, and nothing said so:
# check_bundle_relocatable reported "self-contained OK" for all three, because
# a bundle with no binary has no dangling links either.
#
# So the test is the binary, and its size. Skia and Dawn link statically into
# each of these; a real one is ~19 MB, and anything under a megabyte means the
# link never happened.
MIN_BINARY_BYTES=1000000
# Present only in the current (forge-seam) shell; absent from the old
# examples/forge-modular one. Update if the UI copy ever drops it.
SHELL_MARKER="Browse marketplace"

check_has_binary() {
    local bundle="$1" macos="$1/Contents/MacOS" binary size
    if [[ ! -d "$macos" ]]; then
        echo "missing: $bundle/Contents/MacOS -- not a bundle" >&2
        return 1
    fi
    # NOT `head -1`. Contents/MacOS also holds libwgpu_native.dylib, copied in
    # beside the executable, and it sorts first -- so the checks below ran
    # against the WebGPU runtime and reported the bundle as the wrong shell
    # while the actual binary was correct. Prefer the file named after the
    # bundle, which is what CFBundleExecutable points at.
    local want="${bundle##*/}"; want="${want%.*}"
    binary="$macos/$want"
    if [[ ! -f "$binary" ]]; then
        binary=$(find "$macos" -maxdepth 1 -type f -perm -u+x ! -name "*.dylib" \
                 2>/dev/null | head -1)
    fi
    if [[ -z "$binary" ]]; then
        echo "EMPTY BUNDLE: $bundle has no executable in Contents/MacOS." >&2
        echo "  Its target was configured but never built. Build it first:" >&2
        echo "  cmake --build \"$BUILD_DIR\" --target ForgeModularApp_AU \\" >&2
        echo "      ForgeModularApp_VST3 ForgeModularApp_CLAP ForgeModularApp_Standalone -j8" >&2
        return 1
    fi
    size=$(stat -f%z "$binary" 2>/dev/null || stat -c%s "$binary" 2>/dev/null || echo 0)
    if (( size < MIN_BINARY_BYTES )); then
        echo "STUB BINARY: $binary is ${size} bytes (expected >= ${MIN_BINARY_BYTES})." >&2
        echo "  A real build links Skia and Dawn statically and is far larger." >&2
        return 1
    fi
    # IDENTITY, not just size. The 0.12.0 installer shipped four payloads of
    # entirely correct size containing an older, different shell -- so every
    # size and relocatability check passed while the product inside was wrong.
    # This string exists only in the current shell (forge-seam), which makes it
    # the cheapest available proof that the binary is the one we mean.
    # `grep -qF` would be the obvious spelling and is WRONG here: it exits on
    # the first match, SIGPIPEs `strings`, and under `set -o pipefail` the
    # whole pipeline then reports failure -- so a binary that DOES carry the
    # marker is rejected for carrying it. Count instead, which drains the
    # stream and cannot be killed early.
    local hits
    hits=$(strings "$binary" 2>/dev/null | grep -cF "$SHELL_MARKER" || true)
    if [[ "${hits:-0}" -eq 0 ]]; then
        echo "WRONG SHELL: $binary does not contain \"$SHELL_MARKER\"." >&2
        echo "  This is the old examples/forge-modular shell, not the Forge one." >&2
        echo "  Build the Forge worktree and point --build-dir at it:" >&2
        echo "    forge-seam/populate.sh && cmake --build /tmp/forge-cur/build -j8" >&2
        echo "    package.sh --build-dir /tmp/forge-cur/build …" >&2
        return 1
    fi
    return 0
}

missing=0
for artifact in "$APP" "$AU" "$VST3" "$CLAP"; do
    if [[ ! -e "$artifact" ]]; then
        echo "missing: $artifact" >&2
        missing=1
    elif ! check_has_binary "$artifact"; then
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
# The modules ride inside the app bundle on this path too, and must be placed
# BEFORE signing or they invalidate the signature. Rack reads from the user's
# Application Support, which a package payload cannot address, so a postinstall
# step is what puts them in place -- the same reason as the unsigned path
# above. Without this the signed installer carried the app and all three
# plugins but silently no modules, which looks identical to one that has them.
if [[ -n "$RACK_PLUGIN" ]]; then
    mkdir -p "$APP/Contents/Resources/rack"
    cp "$RACK_PLUGIN" "$APP/Contents/Resources/rack/"
fi

# The identities live in ~/.config/pulp/secrets/keychain.env as hashes, which
# is what codesign wants anyway -- a name can match two certificates, a hash
# cannot. ensure_signing_ready.sh puts them in the environment.
: "${PULP_SIGN_IDENTITY_HASH:?set PULP_SIGN_IDENTITY_HASH (see ~/.config/pulp/secrets/keychain.env)}"
: "${PULP_SIGN_INSTALLER_HASH:?set PULP_SIGN_INSTALLER_HASH (see ~/.config/pulp/secrets/keychain.env)}"

ARGS=(--name "Forge Modular" --version "$VERSION" --out "$OUT_DIR"
      --sign-identity "$PULP_SIGN_IDENTITY_HASH"
      --installer-identity "$PULP_SIGN_INSTALLER_HASH"
      --app "Forge Modular" "$APP"
      --plugin au "$AU" --plugin vst3 "$VST3" --plugin clap "$CLAP")
# build_combined_installer.sh notarizes by DEFAULT and takes --no-notarize to
# opt out -- the inverse of this script's own flag. Passing --notarize through
# made it reject the whole invocation.
[[ $DO_NOTARIZE -eq 1 ]] || ARGS+=(--no-notarize)

# The consent pane. Apple shows it before anything is written, which is where
# the Rack SDK / GPLv3 note belongs -- the moment the user is deciding.
export PKG_LICENSE_FILE="${PKG_LICENSE_FILE:-$REPO/examples/forge-modular/LICENSE-INSTALLER.txt}"

exec "$REPO/tools/scripts/build_combined_installer.sh" "${ARGS[@]}"
