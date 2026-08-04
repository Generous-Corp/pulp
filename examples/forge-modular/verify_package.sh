#!/usr/bin/env bash
# Open the finished installer and read what is actually inside it.
#
#     verify_package.sh "Forge Modular-0.12.5.pkg"
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

PKG="${1:-}"
[[ -n "$PKG" ]] || { echo "usage: verify_package.sh <path to .pkg>" >&2; exit 2; }
[[ -f "$PKG" ]] || { echo "no such package: $PKG" >&2; exit 2; }

# The one string that exists in the current shell and cannot exist in the older
# examples/ one. Keep in step with package.sh's SHELL_MARKER.
SHELL_MARKER="Browse marketplace"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

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

if grep -q 'require-scripts="true"' "$DIST" 2>/dev/null; then
    say_ok "the distribution requires its scripts"
else
    say_bad "require-scripts is not true, so an install could skip the script"
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

# ── the Rack pack, by content ────────────────────────────────────────────────
PACK="$(find "$ROOT/Contents/Resources/rack" -maxdepth 1 -name '*.vcvplugin' \
        2>/dev/null | sort | tail -1)"
if [[ -z "$PACK" ]]; then
    say_bad "no .vcvplugin in the bundle: the installer promises modules it lacks"
else
    mods=$(/usr/bin/tar --zstd -xOf "$PACK" 'ForgeModular/plugin.json' 2>/dev/null \
           | /usr/bin/python3 -c 'import json,sys; print(len(json.load(sys.stdin)["modules"]))' \
             2>/dev/null)
    if [[ -n "$mods" && "$mods" -gt 0 ]]; then
        say_ok "the Rack pack carries $mods modules"
    else
        say_bad "the Rack pack is present but its manifest lists no modules"
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
fi

# ── the three plug-in payloads hold code ─────────────────────────────────────
for kind in au vst3 clap; do
    case "$kind" in
        au)   sub="Forge Modular.component" ;;
        vst3) sub="Forge Modular.vst3" ;;
        clap) sub="Forge Modular.clap" ;;
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
    fi
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
