#!/usr/bin/env bash
# Open the finished installer and read what is actually inside it.
#
#     verify_package.sh "Forge Modular-0.12.5.pkg" 0.12.5 \
#         build/rack/ForgeModular-2.0.0-mac-arm64.vcvplugin
#
# Run by package.sh after every build, and runnable by hand on any .pkg that
# already exists, including one somebody else produced.
#
# WHY THIS EXISTS. In one day this product shipped a 292-byte package that
# signed, notarized, stapled and passed Gatekeeper while containing nothing;
# three plug-in bundles that installed cleanly and held no code; and a 76 MB,
# four-payload, correctly-sized, notarized installer containing an entirely
# different application. Size caught the first two and was useless for the
# third. Only identity separates a correct build from a plausible one, and only
# the finished artifact can be asked -- the script that made it will always say
# it worked.
#
# `pkgutil --payload-files` does NOT recurse into a distribution's nested
# component payloads, so it reports nothing here and reads as an empty package.
# Expand, then read each component.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PKG="${1:-}"
[[ -n "$PKG" ]] || {
    echo "usage: verify_package.sh <pkg> <version> <current .vcvplugin> <Forge build dir>" >&2
    exit 2
}
# The version this package claims to be. Taken from the file name when not
# given, because that is the one place the released version has always been
# right -- it is everywhere else it was wrong.
WANT_VERSION="${2:-}"
if [[ -z "$WANT_VERSION" ]]; then
    _base="${PKG##*/}"; _base="${_base%.pkg}"
    WANT_VERSION="${_base##*-}"
fi
[[ -f "$PKG" ]] || { echo "no such package: $PKG" >&2; exit 2; }
EXPECTED_PACK="${3:-}"
[[ -f "$EXPECTED_PACK" ]] || {
    echo "current Rack build is required for exact package verification: $EXPECTED_PACK" >&2
    exit 2
}
EXPECTED_BUILD_DIR="${4:-}"
[[ -f "$EXPECTED_BUILD_DIR/CMakeCache.txt" ]] || {
    echo "current Forge build directory is required for exact verification" >&2
    exit 2
}

SOURCE_MANIFEST="$REPO/examples/forge-modular/plugin.json"
read -r EXPECTED_SLUG EXPECTED_RACK_VERSION EXPECTED_MODULES < <(
    /usr/bin/python3 -c '
import json, sys
manifest = json.load(open(sys.argv[1]))
print(manifest["slug"], manifest["version"], len(manifest["modules"]))
' "$SOURCE_MANIFEST"
) || exit 1
EXPECTED_NAME="$(basename "$EXPECTED_PACK")"
case "$EXPECTED_NAME" in
    "${EXPECTED_SLUG}-${EXPECTED_RACK_VERSION}-mac-arm64.vcvplugin"|\
    "${EXPECTED_SLUG}-${EXPECTED_RACK_VERSION}-mac-x64.vcvplugin") ;;
    *) echo "current Rack build has the wrong identity: $EXPECTED_NAME" >&2; exit 1 ;;
esac
EXPECTED_MANIFEST="$WORK/expected-plugin.json"
if ! /usr/bin/tar --zstd -xOf "$EXPECTED_PACK" \
        "${EXPECTED_SLUG}/plugin.json" > "$EXPECTED_MANIFEST" 2>/dev/null; then
    echo "current Rack build has no ${EXPECTED_SLUG}/plugin.json: $EXPECTED_PACK" >&2
    exit 1
fi
if ! cmp -s "$SOURCE_MANIFEST" "$EXPECTED_MANIFEST"; then
    echo "current Rack build's plugin.json does not match the current tree" >&2
    exit 1
fi
EXPECTED_PACK_SHA="$(shasum -a 256 "$EXPECTED_PACK" | awk '{print $1}')"
EXPECTED_MANIFEST_SHA="$(shasum -a 256 "$EXPECTED_MANIFEST" | awk '{print $1}')"
EXPECTED_PLATFORM="${EXPECTED_NAME#${EXPECTED_SLUG}-${EXPECTED_RACK_VERSION}-}"
EXPECTED_PLATFORM="${EXPECTED_PLATFORM%.vcvplugin}"
RACK_BUILD_ROOT="$(dirname "$(cd "$(dirname "$EXPECTED_PACK")" && pwd)")"
RACK_CACHE="$RACK_BUILD_ROOT/CMakeCache.txt"
[[ -f "$RACK_CACHE" ]] || {
    echo "current Rack build has no CMake provenance: $RACK_CACHE" >&2
    exit 1
}
RACK_SOURCE_ROOT="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$RACK_CACHE" | tail -1)"
RACK_SOURCE_ROOT="$(cd "$RACK_SOURCE_ROOT" 2>/dev/null && pwd)" || true
[[ "$RACK_SOURCE_ROOT" == "$REPO" ]] || {
    echo "current Rack build belongs to ${RACK_SOURCE_ROOT:-unknown}, not this tree" >&2
    exit 1
}
grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$RACK_CACHE" || {
    echo "current Rack build is not Release" >&2
    exit 1
}
[[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]] || {
    echo "exact verification requires a clean tracked source tree" >&2
    exit 1
}
EXPECTED_SOURCE_TREE_SHA="$(git -C "$REPO" rev-parse 'HEAD^{tree}')"
RACK_SDK_DIR="$(sed -n 's/^PULP_RACK_SDK_DIR:PATH=//p' "$RACK_CACHE" | tail -1)"
RACK_SDK_DIR="$(cd "$RACK_SDK_DIR" 2>/dev/null && pwd)" || true
[[ -n "$RACK_SDK_DIR" && -d "$RACK_SDK_DIR" ]] || {
    echo "current Rack build has no resolved SDK identity" >&2
    exit 1
}
EXPECTED_SDK_SHA="$(/usr/bin/python3 -c '
import hashlib, os, sys
root = os.path.realpath(sys.argv[1])
h = hashlib.sha256()
for base, dirs, files in os.walk(root):
    dirs.sort()
    for name in sorted(files):
        path = os.path.join(base, name)
        if not os.path.isfile(path):
            continue
        rel = os.path.relpath(path, root).encode()
        h.update(len(rel).to_bytes(8, "big")); h.update(rel)
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b""):
                h.update(chunk)
print(h.hexdigest())
' "$RACK_SDK_DIR")"
EXPECTED_BINARY="$WORK/expected-plugin.dylib"
if ! /usr/bin/tar --zstd -xOf "$EXPECTED_PACK" \
        "${EXPECTED_SLUG}/plugin.dylib" > "$EXPECTED_BINARY" 2>/dev/null; then
    echo "current Rack build has no plugin.dylib" >&2
    exit 1
fi
EXPECTED_BINARY_SHA="$(shasum -a 256 "$EXPECTED_BINARY" | awk '{print $1}')"
STAGED_BINARY="$(dirname "$EXPECTED_PACK")/${EXPECTED_SLUG}/plugin.dylib"
if [[ ! -f "$STAGED_BINARY" ]] || ! cmp -s "$STAGED_BINARY" "$EXPECTED_BINARY"; then
    echo "current Rack archive does not match its CMake build-stage binary" >&2
    exit 1
fi
find_bundle_binary() {
    local bundle="$1" want="${1##*/}" binary
    want="${want%.*}"
    binary="$bundle/Contents/MacOS/$want"
    [[ -f "$binary" ]] || binary="$(find "$bundle/Contents/MacOS" -maxdepth 1 \
        -type f -perm -u+x ! -name '*.dylib' -print -quit 2>/dev/null)"
    [[ -f "$binary" ]] || return 1
    echo "$binary"
}
EXPECTED_APP_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(find_bundle_binary "$EXPECTED_BUILD_DIR/modular/Forge Modular.app")")"
EXPECTED_AU_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(find_bundle_binary "$EXPECTED_BUILD_DIR/AU/Forge Modular.component")")"
EXPECTED_VST3_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(find_bundle_binary "$EXPECTED_BUILD_DIR/VST3/Forge Modular.vst3")")"
EXPECTED_CLAP_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(find_bundle_binary "$EXPECTED_BUILD_DIR/CLAP/Forge Modular.clap")")"
[[ -n "$EXPECTED_APP_SHA" && -n "$EXPECTED_AU_SHA" && -n "$EXPECTED_VST3_SHA" && \
   -n "$EXPECTED_CLAP_SHA" ]] || {
    echo "current Forge build is missing one or more exact binaries" >&2
    exit 1
}
EXPECTED_FORGE_BASE="$(tr -d '[:space:]' < "$REPO/forge-seam/patches/BASE")"
EXPECTED_FORGE_SEAM_TREE="$EXPECTED_SOURCE_TREE_SHA"
EXPECTED_PULP_SDK_PREFIX="$(sed -n 's/^CMAKE_PREFIX_PATH:[^=]*=//p' \
    "$EXPECTED_BUILD_DIR/CMakeCache.txt" | tail -1)"
[[ -n "$EXPECTED_PULP_SDK_PREFIX" && "$EXPECTED_PULP_SDK_PREFIX" != *';'* ]] || {
    echo "rebuilt Forge tree did not resolve one exact Pulp SDK" >&2; exit 1; }
EXPECTED_PULP_SDK_SOURCE="${FORGE_PULP_SDK_SOURCE_SHA:-}"
[[ -n "$EXPECTED_PULP_SDK_SOURCE" ]] || {
    echo "FORGE_PULP_SDK_SOURCE_SHA is required for exact verification" >&2; exit 1; }
EXPECTED_PULP_SDK_CONTENT="${FORGE_PULP_SDK_CONTENT_SHA256:-}"
[[ -n "$EXPECTED_PULP_SDK_CONTENT" ]] || {
    echo "FORGE_PULP_SDK_CONTENT_SHA256 is required for exact verification" >&2; exit 1; }
EXPECTED_PULP_SDK_VERSION="${FORGE_PULP_SDK_VERSION:-$(sed -n \
    's/^project(Pulp VERSION \([0-9][0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt" | head -1)}"
case "$EXPECTED_PLATFORM" in
    mac-arm64) EXPECTED_PULP_SDK_PLATFORM="darwin-arm64" ;;
    mac-x64) EXPECTED_PULP_SDK_PLATFORM="darwin-x64" ;;
    *) echo "unsupported Rack platform in package: $EXPECTED_PLATFORM" >&2; exit 1 ;;
esac
EXPECTED_PULP_SDK_JSON="$(/usr/bin/python3 \
    "$REPO/examples/forge-modular/sdk_identity.py" \
    --prefix "$EXPECTED_PULP_SDK_PREFIX" --platform "$EXPECTED_PULP_SDK_PLATFORM" \
    --source-sha "$EXPECTED_PULP_SDK_SOURCE" --version "$EXPECTED_PULP_SDK_VERSION" \
    --content-sha256 "$EXPECTED_PULP_SDK_CONTENT")" \
    || exit 1
EXPECTED_PULP_SDK_SHA="$(/usr/bin/python3 -c \
    'import json,sys; print(json.load(sys.stdin)["content_sha256"])' \
    <<< "$EXPECTED_PULP_SDK_JSON")"
EXPECTED_SHAPE_TEXT_SHA="$(/usr/bin/python3 \
    "$REPO/examples/forge-modular/binary_identity.py" "$REPO/build/shape_text")"
EXPECTED_PROVENANCE="$WORK/expected-provenance.json"
/usr/bin/python3 -c '
import json, sys
keys = ("archive_sha256", "binary_sha256", "manifest_sha256", "module_count",
        "platform", "sdk_sha256", "source_tree_sha", "forge_source_base",
        "forge_seam_tree_sha", "app_binary_sha256", "au_binary_sha256",
        "vst3_binary_sha256", "clap_binary_sha256", "pulp_sdk_version",
        "pulp_sdk_source_sha", "pulp_sdk_content_sha256", "shape_text_sha256")
data = dict(zip(keys, sys.argv[2:]))
data["module_count"] = int(data["module_count"])
with open(sys.argv[1], "w") as f:
    json.dump(data, f, sort_keys=True, separators=(",", ":"))
    f.write("\n")
' "$EXPECTED_PROVENANCE" "$EXPECTED_PACK_SHA" "$EXPECTED_BINARY_SHA" \
  "$EXPECTED_MANIFEST_SHA" "$EXPECTED_MODULES" "$EXPECTED_PLATFORM" \
  "$EXPECTED_SDK_SHA" "$EXPECTED_SOURCE_TREE_SHA" "$EXPECTED_FORGE_BASE" \
  "$EXPECTED_FORGE_SEAM_TREE" "$EXPECTED_APP_SHA" "$EXPECTED_AU_SHA" \
  "$EXPECTED_VST3_SHA" "$EXPECTED_CLAP_SHA" "$EXPECTED_PULP_SDK_VERSION" \
  "$EXPECTED_PULP_SDK_SOURCE" "$EXPECTED_PULP_SDK_SHA" "$EXPECTED_SHAPE_TEXT_SHA"

# The one string that exists in the current shell and cannot exist in the older
# examples/ one. Keep in step with package.sh's SHELL_MARKER.
SHELL_MARKER="Browse marketplace"

bad=0
say_ok()   { echo "  ok     $1"; }
say_bad()  { echo "  WRONG  $1"; bad=$((bad + 1)); }

echo "[verify] $PKG"

pkgutil --expand "$PKG" "$WORK/exp" >/dev/null 2>&1 || {
    echo "  WRONG  pkgutil could not expand it — this is not a valid package" >&2
    exit 1; }

DIST="$WORK/exp/Distribution"
APPCOMP="$WORK/exp/Forge Modular.app.pkg"

# ── the app component runs a script ──────────────────────────────────────────
# The whole reason the Rack modules reach Rack. Absent, the pack ships inside
# the bundle and is never placed, and Rack shows no Forge modules at all --
# which is invisible on any machine that has ever built them.
if [[ -x "$APPCOMP/Scripts/postinstall" ]]; then
    say_ok "the app component carries an executable postinstall"
elif [[ -f "$APPCOMP/Scripts/postinstall" ]]; then
    say_bad "Scripts/postinstall is present but NOT executable, so it never runs"
else
    say_bad "no Scripts/postinstall: the Rack modules would never be placed"
fi

if grep -q 'require-scripts="false"' "$DIST" 2>/dev/null; then
    say_ok "component hooks do not pretend to be distribution JavaScript"
else
    say_bad "the distribution incorrectly requires a JavaScript layer it does not carry"
fi

# ── informed consent is part of the artifact ────────────────────────────────
# An environment variable in package.sh does not configure productbuild. Read
# both the Distribution declaration and the expanded resource, because either
# half missing produces an installer with no useful consent pane.
license_name=$(sed -n 's/.*<license file="\([^"]*\)"\/>.*/\1/p' "$DIST" | head -1)
if [[ -n "$license_name" ]]; then
    say_ok "the distribution declares a license consent pane"
else
    say_bad "the distribution has no license consent pane"
fi
if [[ -n "$license_name" ]] && find "$WORK/exp" -type f -name "$license_name" -print -quit \
        2>/dev/null | grep -q .; then
    say_ok "the consent text is embedded in the installer resources"
else
    say_bad "the declared consent text is absent from installer resources"
fi

# ── the app is not optional ──────────────────────────────────────────────────
# It carries the Rack plug-in, the uninstaller and the generator. Deselecting
# it leaves the plug-in formats with nothing behind them.
if grep -q 'enabled="false" selected="true"' "$DIST" 2>/dev/null; then
    say_ok "the app choice is locked on"
else
    say_bad "the app choice can be deselected"
fi

# ── what the app payload actually contains ───────────────────────────────────
mkdir -p "$WORK/app"
if ! tar xzf "$APPCOMP/Payload" -C "$WORK/app" 2>/dev/null; then
    say_bad "the app payload could not be unpacked"
    echo; echo "$bad problem(s)"; exit 1
fi
ROOT="$WORK/app/Applications/Forge Modular.app"

need_file() {   # $1 = path under the bundle  $2 = what it is for
    if [[ -f "$ROOT/$1" ]]; then
        say_ok "$2"
    else
        say_bad "$2 — missing $1"
    fi
}

# WHICH BUILD IS THIS. An installed 0.12.7 answered 0.11.0, so nothing on the
# machine could tell 12.6 from 12.7 -- which is how a generator from an older
# release shadowed a newer one's fixes for four days. Read out of the EXPANDED
# payload, not out of the build script that claims to have written it.
_got=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
       "$ROOT/Contents/Info.plist" 2>/dev/null || echo "")
if [[ "$_got" == "$WANT_VERSION" ]]; then
    say_ok "the app reports $WANT_VERSION"
else
    say_bad "the app reports \"$_got\", not $WANT_VERSION"
fi
# And the toolchain inside it says which release laid it down, which is what
# lets the app refuse to run an older copy from Application Support.
_stamp=$(sed -n 1p "$ROOT/Contents/Resources/tools/rack/VERSION" 2>/dev/null || echo "")
if [[ "$_stamp" == "$WANT_VERSION" ]]; then
    say_ok "the shipped generator is stamped $WANT_VERSION"
else
    say_bad "the shipped generator's stamp is \"$_stamp\", not $WANT_VERSION"
fi

need_file "Contents/Resources/tools/rack/patch.py"     "the patch generator ships"
need_file "Contents/Resources/tools/rack/generate.py"  "the module generator ships"
need_file "Contents/Resources/tools/rack/fetch_sdk.py" "the SDK fetcher ships"
need_file "Contents/Resources/tools/rack/archive.py"   "the archive helper ships"
need_file "Contents/Resources/uninstall.sh"            "the uninstaller ships"
need_file "Contents/Resources/install_pack.sh"         "the module placer ships"

# The placer is invoked by the postinstall as a program. Shipped without the
# executable bit it is a file the installer cannot run, and the modules stay
# in the bundle -- the same nothing as not shipping it.
if [[ -x "$ROOT/Contents/Resources/install_pack.sh" ]]; then
    say_ok "the module placer is executable"
else
    say_bad "install_pack.sh is not executable, so the postinstall cannot run it"
fi

# ── what MODULE generation reads, by behaviour ───────────────────────────────
# The generator reads four things outside tools/rack, and none of them shipped:
# it called the model, downloaded a 40 MB SDK and then died on an unhandled
# FileNotFoundError. Existence is checked for the cheap ones and BEHAVIOUR for
# the two that can be present and useless.
need_file "Contents/Resources/tools/dsp_vocabulary.py"        "the DSP vocabulary extractor ships"
need_file "Contents/Resources/external/fonts/Inter-Regular.ttf" "the panel font ships"
need_file "Contents/Resources/build/shape_text"               "the panel shaper ships"
need_file "Contents/Resources/examples/forge-modular/plugin.json" "the module pack manifest ships"
for mod in signal format audio state platform runtime; do
    if [[ -d "$ROOT/Contents/Resources/core/$mod/include" ]]; then
        say_ok "Pulp's $mod headers ship"
    else
        say_bad "Pulp's $mod headers are missing — a generated module cannot compile"
    fi
done
if [[ -d "$ROOT/Contents/Resources/examples/forge-modular/src" ]]; then
    say_ok "the module pack's sources ship"
else
    say_bad "the module pack has no src/ — there is nothing to compile"
fi

# A shaper that is present and produces nothing empties every label on every
# panel, and it looks identical to one that works.
shaped="$("$ROOT/Contents/Resources/build/shape_text" Hg \
          "$ROOT/Contents/Resources/external/fonts/Inter-Regular.ttf" \
          3.0 center 2>/dev/null || true)"
case "$shaped" in
    M*) say_ok "the shipped panel shaper actually shapes text" ;;
    *)  say_bad "the shipped panel shaper produced no path for 'Hg'" ;;
esac
shipped_shape_sha="$(/usr/bin/python3 \
    "$REPO/examples/forge-modular/binary_identity.py" \
    "$ROOT/Contents/Resources/build/shape_text" 2>/dev/null || echo "")"
if [[ "$shipped_shape_sha" == "$EXPECTED_SHAPE_TEXT_SHA" ]]; then
    say_ok "the panel shaper is content-identical to the rebuilt Pulp helper"
else
    say_bad "the panel shaper identity is $shipped_shape_sha, not rebuilt helper $EXPECTED_SHAPE_TEXT_SHA"
fi

# An empty vocabulary hands the model a contract with no DSP in it, and the run
# dies at the compiler three model calls later.
vocab=$(cd "$ROOT/Contents/Resources/tools/rack" &&
        /usr/bin/python3 ../dsp_vocabulary.py 2>/dev/null | wc -l | tr -d ' ')
if [[ "${vocab:-0}" -ge 20 ]]; then
    say_ok "the shipped DSP vocabulary extracts $vocab lines"
else
    say_bad "the shipped DSP vocabulary extracts $vocab lines — the model would be given none"
fi

# Reference texts fetched for the citation checker are gitignored, and ditto
# does not read .gitignore -- so they shipped once, inside a signed installer.
# Checked here as well as at staging because this runs against the built
# artifact, which is the only thing that can prove what a user would receive.
leaked="$(find "$ROOT/Contents/Resources" \( -name .corpus -o -name .sweeps -o -name .git \) \
          2>/dev/null | head -1)"
if [[ -z "$leaked" ]]; then
    say_ok "no fetched reference texts or local sweep attempts ride along in the payload"
else
    say_bad "the payload carries $leaked — a signed installer would
         redistribute somebody else's copyrighted text"
fi

# Measured parameter bounds, counted rather than merely present. An empty or
# range-less seed is the failure that looks like success: it ships, it parses,
# and every module still reaches the model with no bounds, which is the state
# the seed exists to end.
ranges=$(/usr/bin/python3 -c '
import json, sys
try:
    mods = json.load(open(sys.argv[1]))["modules"]
except Exception:
    print(0); raise SystemExit
print(sum(1 for m in mods for p in m.get("params") or [] if "minValue" in p))
' "$ROOT/Contents/Resources/tools/rack/portmap-seed.json" 2>/dev/null)
if [[ "${ranges:-0}" -ge 500 ]]; then
    say_ok "the shipped ranges cover $ranges parameters"
else
    say_bad "the shipped ranges cover $ranges parameters — a fresh install would
         reach the model with no bounds and invent values"
fi

# ── the Rack pack, by content ────────────────────────────────────────────────
PACKS=()
while IFS= read -r _pack; do
    PACKS[${#PACKS[@]}]="$_pack"
done < <(find "$ROOT/Contents/Resources/rack" -maxdepth 1 -name '*.vcvplugin' \
         -type f 2>/dev/null | sort)
if [[ ${#PACKS[@]} -eq 0 ]]; then
    say_bad "no .vcvplugin in the bundle: the installer promises modules it lacks"
elif [[ ${#PACKS[@]} -ne 1 ]]; then
    say_bad "the bundle carries ${#PACKS[@]} Rack packs instead of exactly one current build"
else
    PACK="${PACKS[0]}"
    if [[ "$(basename "$PACK")" == "$EXPECTED_NAME" ]]; then
        say_ok "the Rack pack has the current build name $EXPECTED_NAME"
    else
        say_bad "the Rack pack is $(basename "$PACK"), not current build $EXPECTED_NAME"
    fi
    pack_sha="$(shasum -a 256 "$PACK" | awk '{print $1}')"
    if [[ "$pack_sha" == "$EXPECTED_PACK_SHA" ]]; then
        say_ok "the Rack pack is byte-identical to the current build ($pack_sha)"
    else
        say_bad "the Rack pack hash is $pack_sha, not current build $EXPECTED_PACK_SHA"
    fi
    packed_manifest="$WORK/installed-plugin.json"
    if /usr/bin/tar --zstd -xOf "$PACK" \
            "${EXPECTED_SLUG}/plugin.json" > "$packed_manifest" 2>/dev/null; then
        manifest_sha="$(shasum -a 256 "$packed_manifest" | awk '{print $1}')"
        mods=$(/usr/bin/python3 -c '
import json, sys
manifest = json.load(open(sys.argv[1]))
print(len(manifest["modules"]))
' "$packed_manifest" 2>/dev/null)
        if [[ "$manifest_sha" == "$EXPECTED_MANIFEST_SHA" ]]; then
            say_ok "the embedded plugin.json matches the current tree ($manifest_sha)"
        else
            say_bad "the embedded plugin.json hash is $manifest_sha, not current tree $EXPECTED_MANIFEST_SHA"
        fi
        if [[ "$mods" == "$EXPECTED_MODULES" ]]; then
            say_ok "the Rack pack carries all $EXPECTED_MODULES current modules"
        else
            say_bad "the Rack pack carries ${mods:-0} modules, not current tree $EXPECTED_MODULES"
        fi
    else
        say_bad "the Rack pack has no ${EXPECTED_SLUG}/plugin.json"
    fi
    provenance="$ROOT/Contents/Resources/rack/ForgeModular.provenance.json"
    if [[ -f "$provenance" ]] && cmp -s "$provenance" "$EXPECTED_PROVENANCE"; then
        say_ok "the Rack pack provenance binds the current source tree, SDK and binary"
    elif [[ -f "$provenance" ]]; then
        say_bad "the Rack pack provenance does not describe the current source tree, SDK and binary"
    else
        say_bad "the Rack pack has no source, SDK and binary provenance"
    fi
fi

# ── the app binary is the app we mean ────────────────────────────────────────
BIN="$ROOT/Contents/MacOS/Forge Modular"
if [[ ! -f "$BIN" ]]; then
    say_bad "no executable at Contents/MacOS/Forge Modular"
else
    size=$(stat -f%z "$BIN" 2>/dev/null || echo 0)
    if (( size < 1000000 )); then
        say_bad "the app binary is $size bytes — the link never happened"
    # Counted, never `grep -q`: -q exits on the first match, SIGPIPEs
    # `strings`, and under pipefail the pipeline fails -- so a binary that DOES
    # carry the marker gets rejected for carrying it.
    elif [[ "$(strings "$BIN" 2>/dev/null | grep -cF "$SHELL_MARKER" || true)" -eq 0 ]]; then
        say_bad "the app binary does not contain \"$SHELL_MARKER\" — wrong shell"
    else
        say_ok "the app binary is the current shell ($((size / 1048576)) MB)"
    fi
    app_sha="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$BIN")"
    if [[ "$app_sha" == "$EXPECTED_APP_SHA" ]]; then
        say_ok "the app binary is byte-identical to the rebuilt Forge target"
    else
        say_bad "the app binary hash is $app_sha, not rebuilt target $EXPECTED_APP_SHA"
    fi
fi

# ── the three plug-in payloads hold code ─────────────────────────────────────
for kind in au vst3 clap; do
    case "$kind" in
        au)   sub="Forge Modular.component"; expected_binary_sha="$EXPECTED_AU_SHA" ;;
        vst3) sub="Forge Modular.vst3"; expected_binary_sha="$EXPECTED_VST3_SHA" ;;
        clap) sub="Forge Modular.clap"; expected_binary_sha="$EXPECTED_CLAP_SHA" ;;
    esac
    comp="$WORK/exp/Forge Modular.$kind.pkg"
    if [[ ! -f "$comp/Payload" ]]; then
        say_bad "$kind: no payload"
        continue
    fi
    mkdir -p "$WORK/$kind"
    tar xzf "$comp/Payload" -C "$WORK/$kind" 2>/dev/null
    # CMake creates Contents/MacOS at CONFIGURE time, so the directory existing
    # proves nothing; three 72 KB husks signed and installed once on that basis.
    pbin="$WORK/$kind/Contents/MacOS/Forge Modular"
    [[ -f "$pbin" ]] || pbin="$WORK/$kind/$sub/Contents/MacOS/Forge Modular"
    if [[ ! -f "$pbin" ]]; then
        say_bad "$kind: no executable inside the bundle"
    else
        psize=$(stat -f%z "$pbin" 2>/dev/null || echo 0)
        if (( psize < 1000000 )); then
            say_bad "$kind: binary is $psize bytes — an empty husk"
        else
            say_ok "$kind carries a real binary ($((psize / 1048576)) MB)"
        fi
        actual_binary_sha="$(/usr/bin/python3 \
            "$REPO/examples/forge-modular/binary_identity.py" "$pbin")"
        if [[ "$actual_binary_sha" == "$expected_binary_sha" ]]; then
            say_ok "$kind binary is byte-identical to the rebuilt Forge target"
        else
            say_bad "$kind binary hash is $actual_binary_sha, not rebuilt target $expected_binary_sha"
        fi
    fi
    # A DAW showing 0.11.0 beside an app showing 0.12.8 is the same
    # unanswerable question in a worse place.
    pplist="$WORK/$kind/Contents/Info.plist"
    [[ -f "$pplist" ]] || pplist="$WORK/$kind/$sub/Contents/Info.plist"
    kgot=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
           "$pplist" 2>/dev/null || echo "")
    if [[ "$kgot" == "$WANT_VERSION" ]]; then
        say_ok "$kind reports $WANT_VERSION"
    else
        say_bad "$kind reports \"$kgot\", not $WANT_VERSION"
    fi
    proot="$WORK/$kind/$sub"
    [[ -d "$proot" ]] || proot="$WORK/$kind"
    for required in \
        Contents/Resources/tools/rack/patch.py \
        Contents/Resources/tools/rack/generate.py \
        Contents/Resources/tools/dsp_vocabulary.py \
        Contents/Resources/external/fonts/Inter-Regular.ttf \
        Contents/Resources/build/shape_text \
        Contents/Resources/examples/forge-modular/plugin.json; do
        if [[ -f "$proot/$required" ]]; then
            say_ok "$kind carries $required"
        else
            say_bad "$kind cannot generate patches: missing $required"
        fi
    done
done

# ── signature and notarization, reported but never trusted alone ─────────────
# A 292-byte package passed all three of these. They are the last check here,
# not the first, and they are not what makes the verdict.
if pkgutil --check-signature "$PKG" >/dev/null 2>&1; then
    say_ok "signed"
else
    echo "  note   not signed (expected for an unsigned build)"
fi
# `stapler validate` prints "The validate action worked!" and never the word
# "validated", so a checker grepping for that reported every stapled bundle as
# unstapled. Use the exit code.
if xcrun stapler validate "$PKG" >/dev/null 2>&1; then
    say_ok "stapled"
else
    echo "  note   not stapled (expected unless --notarize was used)"
fi

echo
if [[ $bad -eq 0 ]]; then
    echo "[verify] OK — $PKG"
    exit 0
fi
echo "[verify] $bad problem(s) in $PKG" >&2
exit 1
