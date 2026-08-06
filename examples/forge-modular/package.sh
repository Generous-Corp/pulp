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
    arm64|aarch64)
        RACK_PLATFORM="mac-arm64"
        PULP_SDK_PLATFORM="darwin-arm64"
        INSTALLER_ARCH="arm64"
        ;;
    x86_64|amd64)
        RACK_PLATFORM="mac-x64"
        PULP_SDK_PLATFORM="darwin-x64"
        INSTALLER_ARCH="x86_64"
        ;;
    *) echo "unsupported package architecture: $TARGET_ARCH" >&2; exit 2 ;;
esac

[[ -n "$BUILD_DIR" ]] || { echo "--build-dir is required" >&2; exit 2; }
[[ -n "$OUT_DIR" ]] || { echo "--out is required" >&2; exit 2; }
FORGE_REBUILD_ROOT=""
STAGED_ROOT=""
STAGE=""
CHECK=""
cleanup_package_staging() {
    local path
    for path in "$FORGE_REBUILD_ROOT" "$STAGED_ROOT" "$STAGE" "$CHECK"; do
        [[ -n "$path" && -d "$path" ]] && rm -rf "$path"
    done
}
trap cleanup_package_staging EXIT

verify_current_rack_plugin() {
    local pack="$1" platform="$2"
    local source_manifest="$REPO/examples/forge-modular/plugin.json"
    local expected_name manifest_tmp rack_dir build_root cache source_root sdk_dir
    local staged_binary packed_binary

    [[ -f "$pack" ]] || {
        echo "no such Rack pack: $pack" >&2
        return 1
    }
    read -r FM_RACK_SLUG FM_RACK_VERSION FM_RACK_MODULES < <(
        /usr/bin/python3 -c '
import json, sys
manifest = json.load(open(sys.argv[1]))
print(manifest["slug"], manifest["version"], len(manifest["modules"]))
' "$source_manifest"
    ) || return 1
    expected_name="${FM_RACK_SLUG}-${FM_RACK_VERSION}-${platform}.vcvplugin"
    if [[ "$(basename "$pack")" != "$expected_name" ]]; then
        echo "wrong Rack pack identity: expected $expected_name, got $(basename "$pack")" >&2
        return 1
    fi

    manifest_tmp="$(mktemp)"
    if ! /usr/bin/tar --zstd -xOf "$pack" \
            "${FM_RACK_SLUG}/plugin.json" > "$manifest_tmp" 2>/dev/null; then
        rm -f "$manifest_tmp"
        echo "unreadable Rack pack: $pack carries no ${FM_RACK_SLUG}/plugin.json" >&2
        return 1
    fi
    if ! cmp -s "$source_manifest" "$manifest_tmp"; then
        rm -f "$manifest_tmp"
        echo "wrong Rack pack identity: embedded plugin.json does not match the current tree" >&2
        return 1
    fi

    rack_dir="$(cd "$(dirname "$pack")" && pwd)"
    build_root="$(dirname "$rack_dir")"
    cache="$build_root/CMakeCache.txt"
    [[ -f "$cache" ]] || {
        echo "Rack pack has no owning CMake build: $cache" >&2
        return 1
    }
    source_root="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | tail -1)"
    source_root="$(cd "$source_root" 2>/dev/null && pwd)" || true
    if [[ "$source_root" != "$REPO" ]]; then
        echo "Rack pack was not built from this tree: ${source_root:-unknown}" >&2
        return 1
    fi
    if ! grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$cache"; then
        echo "Rack pack must come from a Release build" >&2
        return 1
    fi
    local consumed_status
    consumed_status="$(git -C "$REPO" status --porcelain --untracked-files=all --ignored=matching -- \
        forge-seam examples/forge-modular/modules examples/forge-modular/res \
        examples/forge-modular/design examples/forge-modular/src tools/rack \
        tools/dsp_vocabulary.py external/fonts/Inter-Regular.ttf \
        core/signal/include core/format/include core/audio/include \
        core/state/include core/platform/include core/runtime/include | \
        grep -Ev '^(!!|\?\?) .*/(\.corpus|\.sweeps|__pycache__|\.pytest_cache)(/|$)' || true)"
    if [[ -n "$consumed_status" ]]; then
        echo "package inputs contain tracked changes or untracked/ignored files:" >&2
        echo "$consumed_status" >&2
        return 1
    fi
    if [[ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]]; then
        echo "Rack pack provenance requires a clean tracked source tree" >&2
        return 1
    fi
    cmake --build "$build_root" --target forge-modular-package \
          --parallel "${PULP_PACKAGE_JOBS:-8}"
    if [[ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]]; then
        rm -f "$manifest_tmp"
        echo "Rack rebuild changed tracked source; review and commit it before packaging" >&2
        return 1
    fi

    : > "$manifest_tmp"
    if ! /usr/bin/tar --zstd -xOf "$pack" \
            "${FM_RACK_SLUG}/plugin.json" > "$manifest_tmp" 2>/dev/null; then
        rm -f "$manifest_tmp"
        echo "unreadable Rack pack: $pack carries no ${FM_RACK_SLUG}/plugin.json" >&2
        return 1
    fi
    if ! cmp -s "$source_manifest" "$manifest_tmp"; then
        rm -f "$manifest_tmp"
        echo "rebuilt Rack pack's plugin.json does not match the current tree" >&2
        return 1
    fi
    FM_RACK_MANIFEST_SHA="$(shasum -a 256 "$manifest_tmp" | awk '{print $1}')"
    FM_RACK_SHA="$(shasum -a 256 "$pack" | awk '{print $1}')"
    FM_SOURCE_TREE_SHA="$(git -C "$REPO" rev-parse 'HEAD^{tree}')"

    sdk_dir="$(sed -n 's/^PULP_RACK_SDK_DIR:PATH=//p' "$cache" | tail -1)"
    sdk_dir="$(cd "$sdk_dir" 2>/dev/null && pwd)" || true
    [[ -n "$sdk_dir" && -d "$sdk_dir" ]] || {
        rm -f "$manifest_tmp"
        echo "Rack build has no resolved SDK identity" >&2
        return 1
    }
    FM_RACK_SDK_SHA="$(/usr/bin/python3 -c '
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
' "$sdk_dir")"

    staged_binary="$rack_dir/${FM_RACK_SLUG}/plugin.dylib"
    packed_binary="$(mktemp)"
    if [[ ! -f "$staged_binary" ]] || \
       ! /usr/bin/tar --zstd -xOf "$pack" \
           "${FM_RACK_SLUG}/plugin.dylib" > "$packed_binary" 2>/dev/null; then
        rm -f "$manifest_tmp" "$packed_binary"
        echo "Rack pack has no build-stage plugin.dylib identity" >&2
        return 1
    fi
    FM_RACK_BINARY_SHA="$(shasum -a 256 "$packed_binary" | awk '{print $1}')"
    if ! cmp -s "$staged_binary" "$packed_binary"; then
        rm -f "$manifest_tmp" "$packed_binary"
        echo "Rack pack binary differs from the just-built CMake target" >&2
        return 1
    fi
    rm -f "$packed_binary"
    rm -f "$manifest_tmp"
    [[ -n "$FM_RACK_SHA" && -n "$FM_RACK_MANIFEST_SHA" && \
       -n "$FM_RACK_BINARY_SHA" && -n "$FM_SOURCE_TREE_SHA" && \
       -n "$FM_RACK_SDK_SHA" && "$FM_RACK_MODULES" -gt 0 ]] || {
        echo "unreadable Rack pack identity: $pack" >&2
        return 1
    }
}

rebuild_forge_products() {
    local cache="$1/CMakeCache.txt" source_root base actual prefix clean_source clean_build
    local expected_sdk_source expected_sdk_version expected_sdk_content sdk_json
    [[ -f "$cache" ]] || { echo "Forge build has no CMakeCache.txt: $1" >&2; return 1; }
    source_root="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | tail -1)"
    source_root="$(cd "$source_root" 2>/dev/null && pwd)" || true
    [[ -n "$source_root" ]] && \
        git -C "$source_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
        echo "Forge build has no owning source worktree" >&2; return 1; }
    base="$(tr -d '[:space:]' < "$REPO/forge-seam/patches/BASE")"
    actual="$(git -C "$source_root" rev-parse HEAD)"
    [[ "$actual" == "$base" ]] || {
        echo "Forge build is based on $actual, not pinned source $base" >&2; return 1; }
    prefix="$(sed -n 's/^CMAKE_PREFIX_PATH:[^=]*=//p' "$cache" | tail -1)"
    [[ -n "$prefix" ]] || {
        echo "Forge build has no pinned CMAKE_PREFIX_PATH" >&2; return 1; }
    [[ "$prefix" != *';'* ]] || {
        echo "Forge build must resolve exactly one Pulp SDK prefix, not: $prefix" >&2
        return 1
    }
    prefix="$(cd "$prefix" 2>/dev/null && pwd)" || true
    [[ -n "$prefix" && -d "$prefix" ]] || {
        echo "Forge Pulp SDK prefix does not exist" >&2; return 1; }
    expected_sdk_version="$(resolve_pulp_sdk_version \
        "$prefix" "${FORGE_PULP_SDK_VERSION:-}")" || return 1
    expected_sdk_source="${FORGE_PULP_SDK_SOURCE_SHA:-}"
    expected_sdk_content="${FORGE_PULP_SDK_CONTENT_SHA256:-}"
    [[ -n "$expected_sdk_content" ]] || {
        echo "FORGE_PULP_SDK_CONTENT_SHA256 is required; refusing unverified SDK content" >&2
        return 1
    }
    if [[ -z "$expected_sdk_source" ]]; then
        if [[ "${FORGE_ALLOW_UNPINNED_PULP_SDK:-0}" != 1 ]]; then
            echo "FORGE_PULP_SDK_SOURCE_SHA is required; refusing an unpinned Pulp SDK" >&2
            return 1
        fi
        echo "WARNING: local-only unpinned Pulp SDK override is active" >&2
        expected_sdk_source="$(/usr/bin/python3 -c '
import json,sys
print(json.load(open(sys.argv[1]))["source_git_sha"])
' "$prefix/sdk-provenance.json")"
    fi
    sdk_json="$(/usr/bin/python3 "$REPO/examples/forge-modular/sdk_identity.py" \
        --prefix "$prefix" --platform "$PULP_SDK_PLATFORM" \
        --source-sha "$expected_sdk_source" --version "$expected_sdk_version" \
        --content-sha256 "$expected_sdk_content")" || return 1
    read -r FM_PULP_SDK_VERSION FM_PULP_SDK_SOURCE_SHA FM_PULP_SDK_SHA < <(
        /usr/bin/python3 -c '
import json,sys
d=json.load(sys.stdin)
print(d["version"], d["source_sha"], d["content_sha256"])
' <<< "$sdk_json")
    FORGE_REBUILD_ROOT="$(mktemp -d)"
    clean_source="$FORGE_REBUILD_ROOT/source"
    clean_build="$FORGE_REBUILD_ROOT/build"
    mkdir -p "$clean_source"
    git -C "$source_root" archive "$base" | /usr/bin/tar -xf - -C "$clean_source"
    git -C "$clean_source" init -q
    "$REPO/forge-seam/populate.sh" "$clean_source"
    cmake -S "$clean_source" -B "$clean_build" -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_PREFIX_PATH="$prefix"
    cmake --build "$clean_build" --target ForgeModular_Standalone ForgeModular_AU \
          ForgeModular_VST3 ForgeModular_CLAP --parallel "${PULP_PACKAGE_JOBS:-8}"
    FORGE_SOURCE_BASE="$base"
    FORGE_SEAM_TREE_SHA="$(git -C "$REPO" rev-parse 'HEAD^{tree}')"
    BUILD_DIR="$clean_build"
}

write_rack_provenance() {
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
' "$1" "$FM_RACK_SHA" "$FM_RACK_BINARY_SHA" "$FM_RACK_MANIFEST_SHA" \
  "$FM_RACK_MODULES" "$RACK_PLATFORM" "$FM_RACK_SDK_SHA" "$FM_SOURCE_TREE_SHA" \
  "$FORGE_SOURCE_BASE" "$FORGE_SEAM_TREE_SHA" "$FM_APP_BINARY_SHA" \
  "$FM_AU_BINARY_SHA" "$FM_VST3_BINARY_SHA" "$FM_CLAP_BINARY_SHA" \
  "$FM_PULP_SDK_VERSION" "$FM_PULP_SDK_SOURCE_SHA" "$FM_PULP_SDK_SHA" \
  "$FM_SHAPE_TEXT_SHA"
}

[[ -n "$RACK_PLUGIN" ]] || {
    echo "--rack-plugin is required; pass the artifact produced by the current Rack build" >&2
    exit 2
}
verify_current_rack_plugin "$RACK_PLUGIN" "$RACK_PLATFORM" || exit 1
echo "[installer] rack pack: $RACK_PLUGIN"
echo "[installer] rack pack identity: $FM_RACK_MODULES modules, manifest $FM_RACK_MANIFEST_SHA, archive $FM_RACK_SHA"
echo "[installer] build provenance: tree $FM_SOURCE_TREE_SHA, SDK $FM_RACK_SDK_SHA, binary $FM_RACK_BINARY_SHA"

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
rebuild_forge_products "$BUILD_DIR" || exit 1
"$REPO/tools/rack/build_shape_text.sh"
FM_SHAPE_TEXT_SHA="$(/usr/bin/python3 \
    "$REPO/examples/forge-modular/binary_identity.py" "$REPO/build/shape_text")"

if [[ -n "${APP_OVERRIDE:-}" ]]; then
    echo "APP_OVERRIDE cannot prove current Forge build identity" >&2
    exit 2
elif [[ -d "$BUILD_DIR/modular/Forge Modular.app" ]]; then
    APP="$BUILD_DIR/modular/Forge Modular.app"
else
    APP="$BUILD_DIR/examples/forge-modular/Forge Modular.app"
fi
echo "[installer] app: $APP"

AU="$BUILD_DIR/AU/Forge Modular.component"
VST3="$BUILD_DIR/VST3/Forge Modular.vst3"
CLAP="$BUILD_DIR/CLAP/Forge Modular.clap"

bundle_binary() {
    local bundle="$1" want="${1##*/}" binary
    want="${want%.*}"
    binary="$bundle/Contents/MacOS/$want"
    [[ -f "$binary" ]] || binary="$(find "$bundle/Contents/MacOS" -maxdepth 1 \
        -type f -perm -u+x ! -name '*.dylib' -print -quit 2>/dev/null)"
    [[ -f "$binary" ]] || return 1
    echo "$binary"
}
FM_APP_BINARY_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(bundle_binary "$APP")")"
FM_AU_BINARY_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(bundle_binary "$AU")")"
FM_VST3_BINARY_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(bundle_binary "$VST3")")"
FM_CLAP_BINARY_SHA="$(/usr/bin/python3 "$REPO/examples/forge-modular/binary_identity.py" "$(bundle_binary "$CLAP")")"

# THE GENERATOR MUST SHIP. The 0.12.1 package carried the app, the plugins and
# the Rack modules, and ZERO python -- so on a machine that had never seen the
# source, Build did nothing. It only ever worked on the build machine because a
# manual step had written the tools into Application Support by hand.
#
# Staged into a COPY of the bundle, before signing, so the signature covers
# them and the source tree is never mutated by packaging.
STAGED_ROOT="$(mktemp -d)"
ditto "$APP" "$STAGED_ROOT/$(basename "$APP")"
APP="$STAGED_ROOT/$(basename "$APP")"
TOOLS_DEST="$APP/Contents/Resources/tools/rack"
rm -rf "$TOOLS_DEST"
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
    write_rack_provenance \
       "$STAGE/Applications/Forge Modular.app/Contents/Resources/rack/ForgeModular.provenance.json"

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
    mkdir -p "$CHECK/payload"
    /usr/bin/tar xzf "$CHECK/expanded/Payload" -C "$CHECK/payload"
    _unsigned_pack_dir="$CHECK/payload/Applications/Forge Modular.app/Contents/Resources/rack"
    _unsigned_packs=()
    while IFS= read -r _pack; do
        _unsigned_packs[${#_unsigned_packs[@]}]="$_pack"
    done < <(find "$_unsigned_pack_dir" -maxdepth 1 -type f -name '*.vcvplugin' \
             2>/dev/null | sort)
    [[ ${#_unsigned_packs[@]} -eq 1 ]] || {
        echo "unsigned package has ${#_unsigned_packs[@]} Rack packs, expected exactly one" >&2
        exit 1
    }
    [[ "$(basename "${_unsigned_packs[0]}")" == "$(basename "$RACK_PLUGIN")" ]] || {
        echo "unsigned package carries the wrong Rack pack name" >&2; exit 1; }
    _unsigned_sha="$(shasum -a 256 "${_unsigned_packs[0]}" | awk '{print $1}')"
    [[ "$_unsigned_sha" == "$FM_RACK_SHA" ]] || {
        echo "unsigned package Rack pack differs from current build" >&2; exit 1; }
    write_rack_provenance "$CHECK/expected-provenance.json"
    cmp -s "$CHECK/expected-provenance.json" \
        "$_unsigned_pack_dir/ForgeModular.provenance.json" || {
        echo "unsigned package Rack provenance differs from current build" >&2
        exit 1
    }
    _unsigned_bundles=(
        "$CHECK/payload/Applications/Forge Modular.app"
        "$CHECK/payload/Library/Audio/Plug-Ins/Components/Forge Modular.component"
        "$CHECK/payload/Library/Audio/Plug-Ins/VST3/Forge Modular.vst3"
        "$CHECK/payload/Library/Audio/Plug-Ins/CLAP/Forge Modular.clap")
    _unsigned_hashes=("$FM_APP_BINARY_SHA" "$FM_AU_BINARY_SHA"
                      "$FM_VST3_BINARY_SHA" "$FM_CLAP_BINARY_SHA")
    for _fm_i in 0 1 2 3; do
        _unsigned_binary="$(bundle_binary "${_unsigned_bundles[$_fm_i]}")" || {
            echo "unsigned package is missing a Forge binary" >&2; exit 1; }
        _unsigned_binary_sha="$(/usr/bin/python3 \
            "$REPO/examples/forge-modular/binary_identity.py" "$_unsigned_binary")"
        [[ "$_unsigned_binary_sha" == "${_unsigned_hashes[$_fm_i]}" ]] || {
            echo "unsigned package Forge binary differs from rebuilt target" >&2
            exit 1
        }
    done
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
write_rack_provenance \
    "$APP/Contents/Resources/rack/ForgeModular.provenance.json"

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

# The consent pane. Apple shows it before anything is written, which is where
# the Rack SDK / GPLv3 note belongs -- the moment the user is deciding.
PKG_LICENSE_FILE="${PKG_LICENSE_FILE:-$REPO/examples/forge-modular/LICENSE-INSTALLER.txt}"

ARGS=(--name "Forge Modular" --version "$VERSION" --out "$OUT_DIR"
      --architectures "$INSTALLER_ARCH"
      --sign-identity "$PULP_SIGN_IDENTITY_HASH"
      --installer-identity "$PULP_SIGN_INSTALLER_HASH"
      --license "$PKG_LICENSE_FILE"
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

# NOT `exec`. The package has to be opened afterwards and read back, because
# every delivery failure this project has had was a build script reporting
# success over an artifact nobody looked inside.
"$REPO/tools/scripts/build_combined_installer.sh" "${ARGS[@]}"

PKG="$OUT_DIR/Forge Modular-$VERSION.pkg"
"$REPO/examples/forge-modular/verify_package.sh" "$PKG" "$VERSION" \
    "$RACK_PLUGIN" "$BUILD_DIR"
