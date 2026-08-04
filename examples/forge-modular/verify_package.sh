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
# The version this package claims to be. Taken from the file name when not
# given, because that is the one place the released version has always been
# right -- it is everywhere else it was wrong.
WANT_VERSION="${2:-}"
if [[ -z "$WANT_VERSION" ]]; then
    _base="${PKG##*/}"; _base="${_base%.pkg}"
    WANT_VERSION="${_base##*-}"
fi
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

# An empty vocabulary hands the model a contract with no DSP in it, and the run
# dies at the compiler three model calls later.
vocab=$(cd "$ROOT/Contents/Resources/tools/rack" &&
        /usr/bin/python3 ../dsp_vocabulary.py 2>/dev/null | wc -l | tr -d ' ')
if [[ "${vocab:-0}" -ge 20 ]]; then
    say_ok "the shipped DSP vocabulary extracts $vocab lines"
else
    say_bad "the shipped DSP vocabulary extracts $vocab lines — the model would be given none"
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
