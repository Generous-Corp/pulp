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
#   package.sh --build-dir DIR --out DIR --rack-plugin FILE [--sign] [--notarize]
#
# Unsigned by default. Signing and notarization reach Apple's servers, so they
# are explicit rather than implied -- an unsigned .pkg is exactly what you want
# while proving the packaging itself works.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# The version rules, shared with the test that drives them alone.
. "$REPO/examples/forge-modular/version_stamp.sh"
BUILD_DIR=""
OUT_DIR=""
VERSION="0.1.0"
DO_SIGN=0
DO_NOTARIZE=0
TARGET_ARCH="${TARGET_ARCH_OVERRIDE:-$(uname -m)}"
RACK_PLUGIN="${RACK_PLUGIN_OVERRIDE:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --architecture) TARGET_ARCH="$2"; shift 2 ;;
        --rack-plugin) RACK_PLUGIN="$2"; shift 2 ;;
        --sign) DO_SIGN=1; shift ;;
        --notarize) DO_SIGN=1; DO_NOTARIZE=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$TARGET_ARCH" in
    arm64|aarch64) RACK_PLATFORM="mac-arm64"; INSTALLER_ARCH="arm64" ;;
    x86_64|amd64) RACK_PLATFORM="mac-x64"; INSTALLER_ARCH="x86_64" ;;
    *) echo "unsupported package architecture: $TARGET_ARCH" >&2; exit 2 ;;
esac

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
trap 'rm -rf "$STAGED_ROOT" "${STAGE:-}" "${CHECK:-}"' EXIT
ditto "$APP" "$STAGED_ROOT/$(basename "$APP")"
APP="$STAGED_ROOT/$(basename "$APP")"
TOOLS_DEST="$APP/Contents/Resources/tools/rack"
mkdir -p "$TOOLS_DEST"
ditto "$REPO/tools/rack" "$TOOLS_DEST"

# ditto copies the directory, not the repository's view of it, so anything
# gitignored beside the tools ships too. `.corpus/` is reference texts fetched
# for the citation checker -- copyrighted books and cloned git repositories,
# deliberately never committed. Shipping it put those inside a SIGNED,
# notarization-bound installer, and the embedded .git directories broke the
# bundle seal on the way: Apple rejected the build with "the signature of the
# binary is invalid" rather than anything about the files themselves.
#
# So prune, then PROVE the prune worked. A silent failure here redistributes
# somebody else's book under our signature, which is the one outcome that
# cannot be fixed after release.
for junk in .corpus .sweeps __pycache__ .git .DS_Store .pytest_cache; do
    find "$TOOLS_DEST" -name "$junk" -maxdepth 3 -exec rm -rf {} + 2>/dev/null || true
done
leaked="$(find "$TOOLS_DEST" \( -name .corpus -o -name .sweeps -o -name .git -o -name __pycache__ \) \
          2>/dev/null | head -3)"
if [[ -n "$leaked" ]]; then
    echo "staging failed: unshippable material survived the prune:" >&2
    echo "$leaked" >&2
    exit 1
fi
# The uninstaller lives beside the app that offers it. The SDK fetch does
# not need staging of its own: fetch_sdk.py ships inside tools/rack above,
# and it is the ONE resolver-and-fetcher (a second shell copy of it shipped
# here once, pointing at a different directory, and nothing ever invoked it).
#
# install_pack.sh is the rule for putting the Rack modules where Rack reads
# them. It ships in the bundle because BOTH of its callers live there: the
# installer's postinstall (as root, for the console user) and the app's own
# repair path (as the user, on any generation that finds no pack).
for helper in uninstall.sh install_pack.sh; do
    if [[ -f "$REPO/examples/forge-modular/$helper" ]]; then
        ditto "$REPO/examples/forge-modular/$helper" "$APP/Contents/Resources/$helper"
        chmod +x "$APP/Contents/Resources/$helper"
    fi
done
# THE GENERATOR IS NOT THE ONLY THING IT READS.
#
# tools_dir() is Contents/Resources/tools/rack, and three readers walk up from
# it to `examples/forge-modular/`: patch.py names our own modules' ports from
# modules/*.json, the shell's rack preview draws panels out of res/, and the
# module summary reads the newest manifest. None of that was staged, so on a
# machine without the source every Forge module in a patch was drawn blank and
# explained by port index. On a development machine the walk lands in the
# checkout and everything is perfect.
#
# Read-only, so it can live in the signed bundle. What the app WRITES goes to
# Application Support instead -- see user_patches_dir() in patch.py.
PACK_DEST="$APP/Contents/Resources/examples/forge-modular"
mkdir -p "$PACK_DEST"
for part in modules res design src; do
    if [[ -d "$REPO/examples/forge-modular/$part" ]]; then
        ditto "$REPO/examples/forge-modular/$part" "$PACK_DEST/$part"
    fi
done
for part in plugin.json LICENSE.md LICENSE-MIT.txt; do
    [[ -f "$REPO/examples/forge-modular/$part" ]] &&
        ditto "$REPO/examples/forge-modular/$part" "$PACK_DEST/$part"
done
# AND THE REST OF WHAT MODULE GENERATION READS.
#
# `generate.py` is not one directory either. It extracts the DSP vocabulary the
# model is given from Pulp's headers, compiles the generated module against
# those same headers, and shells out to a panel shaper that loads a font. None
# of that was staged, so Build on a fresh Mac died with an unhandled
# FileNotFoundError -- AFTER downloading a 40 MB SDK, which is the most
# expensive place a missing file can be discovered.
#
# The alternative was dropping the shaper, and it is not available: Rack draws
# panels with nanosvg, which has no <text> support, so every label has to be an
# outlined path and outlining needs a real shaping stack. 16 MB of statically
# linked Skia is the price of lettering that exists.
#
# Everything here is read-only input. What a generation WRITES goes to
# Application Support -- see ensure_writable_toolchain() in generate.py.
ditto "$REPO/tools/dsp_vocabulary.py" "$APP/Contents/Resources/tools/dsp_vocabulary.py"
mkdir -p "$APP/Contents/Resources/external/fonts"
ditto "$REPO/external/fonts/Inter-Regular.ttf" \
      "$APP/Contents/Resources/external/fonts/Inter-Regular.ttf"
mkdir -p "$APP/Contents/Resources/build"
ditto "$REPO/build/shape_text" "$APP/Contents/Resources/build/shape_text"
for mod in signal format audio state platform runtime; do
    ditto "$REPO/core/$mod/include" "$APP/Contents/Resources/core/$mod/include"
done

[[ -f "$TOOLS_DEST/patch.py" ]] || { echo "staging failed: no patch.py" >&2; exit 1; }

# THE VERSION STAMP. Written here, at package time, because this is the only
# moment that knows which release these files are.
#
# `tools_dir()` used to prefer the copy under Application Support
# unconditionally, so an installer could not update what it installs: a
# toolchain written by 0.11 shadowed every fix 0.12.7 shipped, and the shadowed
# copy was too old to understand `library_catalog.py index` -- it printed its
# usage, exited 2, and the library index silently never rebuilt for four days.
#
# The comparison cannot be an mtime. Every path this takes is a copy, and a
# copy rewrites mtimes; a stamp travels with the files instead. Two lines: the
# version, and when it was packaged.
PACKAGED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
write_toolchain_stamp "$TOOLS_DEST" "$VERSION" "$PACKAGED_AT"
echo "[installer] toolchain stamped $VERSION ($PACKAGED_AT)"

# AND THE VERSION THE BUNDLES REPORT.
#
# The plug-ins are staged into copies for the same reason the app is: the stamp
# must be inside the signature, and the build tree must not be mutated by
# packaging.
stage_and_stamp() {   # <bundle> -> echoes the staged path
    local dest="$STAGED_ROOT/$(basename "$1")"
    ditto "$1" "$dest"
    # A hosted plug-in can be the first Forge surface opened on a clean
    # machine. Give every format the same self-contained generator runtime as
    # the app instead of relying on the app having run and seeded a user dir.
    for runtime_part in tools external build core examples; do
        [[ -d "$APP/Contents/Resources/$runtime_part" ]] || continue
        mkdir -p "$dest/Contents/Resources/$runtime_part"
        ditto "$APP/Contents/Resources/$runtime_part" \
              "$dest/Contents/Resources/$runtime_part"
    done
    stamp_bundle_version "$dest" "$VERSION" >&2 || return 1
    echo "$dest"
}

AU="$BUILD_DIR/AU/Forge Modular.component"
VST3="$BUILD_DIR/VST3/Forge Modular.vst3"
CLAP="$BUILD_DIR/CLAP/Forge Modular.clap"
# Staged and stamped only when they exist; the emptiness checks below still get
# to say "missing" in their own words rather than dying inside ditto.
for _fm_fmt in AU VST3 CLAP; do
    _fm_path="${!_fm_fmt}"
    [[ -d "$_fm_path" ]] || continue
    printf -v "$_fm_fmt" '%s' "$(stage_and_stamp "$_fm_path")"
done
# The app was staged further up (the generator had to go inside it first).
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" \
    "$APP/Contents/Info.plist" >/dev/null
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" \
    "$APP/Contents/Info.plist" >/dev/null
_fm_version_bad=0
for artifact in "$APP" "$AU" "$VST3" "$CLAP"; do
    [[ -d "$artifact" ]] || continue
    check_bundle_version "$artifact" "$VERSION" || _fm_version_bad=1
done
[[ $_fm_version_bad -eq 0 ]] || exit 1
echo "[installer] all four bundles report $VERSION"
# THE MODULES ARE NOT BUILT WHERE THE APP IS.
#
# `pulp_add_rack_plugin` runs in the pulp tree and writes the .vcvplugin into
# THAT build directory; the app is compiled in a Forge worktree. So the
# documented invocation (--build-dir /tmp/forge-cur/build) can never contain
# one, and this used to look only there, find nothing, print a note to stderr
# and carry on.
#
# The 0.12.4 installer shipped modules anyway -- because somebody had copied a
# .vcvplugin into the built app bundle by hand, weeks earlier, and the staging
# step copies whatever is in the bundle. It was a stale pack placed by an
# interactive step nothing records, and the next clean build would have shipped
# an installer with no modules and said so only in a line nobody reads.
#
# So the caller must name the artifact made by the current Rack build. Choosing
# "newest" from several persistent build directories is not provenance: a
# stale pack can be newest, or the only one left after its sources changed.
# The app's own installer pane promises "Includes the Rack plug-in" -- an
# installer that quietly carries an old or absent one is worse than a failure.
[[ -n "$RACK_PLUGIN" ]] || {
    echo "--rack-plugin is required; pass the artifact produced by the current Rack build" >&2
    exit 2
}
if [[ -n "$RACK_PLUGIN" && "$(basename "$RACK_PLUGIN")" != *"-$RACK_PLATFORM.vcvplugin" ]]; then
    echo "wrong Rack pack for $TARGET_ARCH: $RACK_PLUGIN" >&2
    exit 1
fi

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
echo "[installer] rack pack: $RACK_PLUGIN"
# IDENTITY, not size. A .vcvplugin is a zstd tar; a truncated download or a
# half-written copy is still a file of plausible length. Reading the manifest
# back out proves it is the pack we mean, and prints the module count so an
# emptied pack cannot pass for a full one.
if ! _fm_mods=$(/usr/bin/tar --zstd -xOf "$RACK_PLUGIN" 'ForgeModular/plugin.json' 2>/dev/null \
        | /usr/bin/python3 -c 'import json,sys; print(len(json.load(sys.stdin)["modules"]))' 2>/dev/null) \
   || [[ -z "$_fm_mods" || "$_fm_mods" -lt 1 ]]; then
    echo "unreadable rack pack: $RACK_PLUGIN carries no ForgeModular/plugin.json" >&2
    exit 1
fi
echo "[installer] rack pack carries $_fm_mods modules"
[[ $missing -eq 0 ]] || { echo "build the four targets first" >&2; exit 1; }

mkdir -p "$OUT_DIR"

if [[ $DO_SIGN -eq 0 ]]; then
    # Unsigned: assemble the payload with pkgbuild directly so the shape can be
    # proven without credentials. macOS will refuse to open it without a
    # right-click, which is correct and expected for an unsigned build.
    STAGE="$(mktemp -d)"

    mkdir -p "$STAGE/Applications" \
             "$STAGE/Library/Audio/Plug-Ins/Components" \
             "$STAGE/Library/Audio/Plug-Ins/VST3" \
             "$STAGE/Library/Audio/Plug-Ins/CLAP"
    cp -R "$APP" "$STAGE/Applications/"
    cp -R "$AU" "$STAGE/Library/Audio/Plug-Ins/Components/"
    cp -R "$VST3" "$STAGE/Library/Audio/Plug-Ins/VST3/"
    cp -R "$CLAP" "$STAGE/Library/Audio/Plug-Ins/CLAP/"

    # Rack reads from the user's Application Support, which a package payload
    # cannot address, so the modules ride along inside the app and a postinstall
    # step is what places them. Copying here would install them for root and
    # nobody else.
    #
    # Emptied first: a build tree can carry a pack that was put there by hand,
    # and "whatever was already in the bundle" is how a stale one shipped.
    rm -rf "$STAGE/Applications/Forge Modular.app/Contents/Resources/rack"
    mkdir -p "$STAGE/Applications/Forge Modular.app/Contents/Resources/rack"
    cp "$RACK_PLUGIN" \
       "$STAGE/Applications/Forge Modular.app/Contents/Resources/rack/"

    PKG="$OUT_DIR/ForgeModular-$VERSION-unsigned.pkg"
    pkgbuild --root "$STAGE" \
             --scripts "$REPO/examples/forge-modular/scripts" \
             --identifier com.generous.forgemodular \
             --version "$VERSION" \
             --install-location / \
             "$PKG"
    # This flat unsigned package does not have a Distribution wrapper, so read
    # the component back directly and prove both the hook and its payload.
    CHECK="$(mktemp -d)"
    pkgutil --expand "$PKG" "$CHECK/expanded" >/dev/null
    [[ -x "$CHECK/expanded/Scripts/postinstall" ]] || {
        echo "unsigned package has no executable postinstall" >&2; exit 1; }
    _unsigned_packs=$(/usr/bin/tar tzf "$CHECK/expanded/Payload" \
        | grep -c 'Forge Modular.app/Contents/Resources/rack/.*\.vcvplugin$' || true)
    [[ "$_unsigned_packs" -ge 1 ]] || {
        echo "unsigned package has no bundled Rack pack" >&2; exit 1; }
    rm -rf "$CHECK"
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
#
# Emptied first, for the same reason as the unsigned path: a build tree can
# carry a pack somebody placed by hand, and taking "whatever is already in the
# bundle" is how a stale one shipped once already.
rm -rf "$APP/Contents/Resources/rack"
mkdir -p "$APP/Contents/Resources/rack"
cp "$RACK_PLUGIN" "$APP/Contents/Resources/rack/"

# The identities live in ~/.config/pulp/secrets/keychain.env as hashes, which
# is what codesign wants anyway -- a name can match two certificates, a hash
# cannot. ensure_signing_ready.sh puts them in the environment.
if [[ -f "$HOME/.config/pulp/secrets/keychain.env" ]]; then
    source "$HOME/.config/pulp/secrets/keychain.env"
fi
if [[ "${PULP_SKIP_SIGNING_PREFLIGHT:-0}" != 1 ]]; then
    "$REPO/tools/scripts/ensure_signing_ready.sh" >/dev/null
fi
: "${PULP_SIGN_IDENTITY_HASH:?set PULP_SIGN_IDENTITY_HASH (see ~/.config/pulp/secrets/keychain.env)}"
: "${PULP_SIGN_INSTALLER_HASH:?set PULP_SIGN_INSTALLER_HASH (see ~/.config/pulp/secrets/keychain.env)}"

# The panel shaper is a Mach-O executable inside Resources, which codesign
# treats as a resource and seals by hash rather than signing as code. Apple's
# notary service scans every Mach-O in the payload regardless and rejects the
# whole submission over an unsigned one, so it is signed here -- before the
# bundle is sealed around it, or the seal would be invalidated by this.
codesign --force --options runtime --timestamp \
         -s "$PULP_SIGN_IDENTITY_HASH" "$APP/Contents/Resources/build/shape_text"

ARGS=(--name "Forge Modular" --version "$VERSION" --out "$OUT_DIR"
      --architectures "$INSTALLER_ARCH"
      --sign-identity "$PULP_SIGN_IDENTITY_HASH"
      --installer-identity "$PULP_SIGN_INSTALLER_HASH"
      --app-for "Forge Modular" "Forge Modular" "$APP"
      # Rack loads plug-ins from the user's Application Support. A package
      # writes absolute paths as root, so the .vcvplugin cannot be addressed to
      # its real home by the payload -- it rides inside the app bundle and this
      # script moves it afterwards. Without it the modules install and are
      # never seen, which no development machine can notice, because the pack
      # is already in that folder from a build.
      --app-scripts "Forge Modular" "$REPO/examples/forge-modular/scripts"
      --plugin au "$AU" --plugin vst3 "$VST3" --plugin clap "$CLAP")
# build_combined_installer.sh notarizes by DEFAULT and takes --no-notarize to
# opt out -- the inverse of this script's own flag. Passing --notarize through
# made it reject the whole invocation.
[[ $DO_NOTARIZE -eq 1 ]] || ARGS+=(--no-notarize)

# The consent pane. Apple shows it before anything is written, which is where
# the Rack SDK / GPLv3 note belongs -- the moment the user is deciding.
export PKG_LICENSE_FILE="${PKG_LICENSE_FILE:-$REPO/examples/forge-modular/LICENSE-INSTALLER.txt}"

# NOT `exec`. The package has to be opened afterwards and read back, because
# every delivery failure this project has had was a build script reporting
# success over an artifact nobody looked inside.
"$REPO/tools/scripts/build_combined_installer.sh" "${ARGS[@]}"

PKG="$OUT_DIR/Forge Modular-$VERSION.pkg"
"$REPO/examples/forge-modular/verify_package.sh" "$PKG" "$VERSION"
