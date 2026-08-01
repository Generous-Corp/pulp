#!/usr/bin/env bash
# Prove the three surfaces: the CLI, the app by clicking Build, and a DAW.
#
# This is Step 7. It runs on whichever machine you point it at -- locally by
# default, or a remote one via FORGE_HOST -- so the procedure is exercised
# before it is trusted somewhere new.
#
#   tools/rack/prove_surfaces.sh            # all three
#   tools/rack/prove_surfaces.sh cli        # just one
#
# AUDIO: the app and the DAW both open an audio device. Each run is capped and
# torn down; say so before running this where someone can hear it.
#
# Every surface reports PASS, FAIL or SKIP. A SKIP is never a PASS -- if a
# surface cannot be exercised here, that is what it says, and why.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="${FORGE_MODULAR_TOOLS:-$HOME/Library/Application Support/Forge Modular/tools/rack}"
ONLY="${1:-all}"
pass=0; fail=0; skip=0

# `cap SECONDS command...` -- a time limit that works on a machine
# with no coreutils. One shim, shared, because two copies drift.
. "$HERE/cap.sh"

# Whether this session can show a window at all -- a locked screen is a
# logged-in session that cannot.
. "$HERE/session.sh"

# Which generator this proof is about, and whether it is the one being read.
. "$HERE/toolchain.sh"
toolchain_report "$HERE" "$TOOLS"

# Run somewhere else, if asked. The header has promised FORGE_HOST since this
# file was written and nothing ever read it: setting it ran the proof on THIS
# machine and printed PASS, which reads exactly like a proof of the machine you
# named. The most important claim this repo makes -- that it works on the M5 --
# had an instrument that could not tell the two apart.
#
# Re-exec over SSH rather than proxying each surface: the app and the DAW need
# a window server on the machine being proved, so the proof has to run there
# whole or not at all.
if [ -n "${FORGE_HOST:-}" ] && [ "${FORGE_PROVE_REMOTE:-}" != "1" ]; then
    case "$FORGE_HOST" in
        localhost|127.0.0.1|"$(hostname -s)") ;;      # already here
        *)
            printf 'proving on %s over SSH\n' "$FORGE_HOST"
            remote_script="$TOOLS/prove_surfaces.sh"
            if ! ssh -o ConnectTimeout=10 "$FORGE_HOST" \
                     "test -f '$remote_script'" 2>/dev/null; then
                printf '  the toolchain is not installed on %s (%s)\n' \
                       "$FORGE_HOST" "$remote_script" >&2
                printf '  run tools/rack/setup_m5.sh first — refusing to fall\n' >&2
                printf '  back to this machine and call it a proof of that one.\n' >&2
                exit 2
            fi
            exec ssh -o ConnectTimeout=10 "$FORGE_HOST" \
                 "FORGE_PROVE_REMOTE=1 bash '$remote_script' '$ONLY'"
            ;;
    esac
fi

# WHY a generation stopped, in the generator's own words.
#
# This reported `tail -2 | head -1` -- an arbitrary line -- and on a machine
# whose model CLI cannot reach its credential that produced:
#
#   FAIL  the CLI did not produce a patch:   window on that machine, or
#         unlock the keychain first:
#
# a fragment from the middle of a sentence, naming neither the problem nor the
# fix. The generators end with a small set of known messages; report the first
# one that appears, with the line after it, and fall back to the tail only when
# none is found.
generator_reason() {
    local out="$1" line
    for marker in "gave up after" "model call failed" \
                  "not logged in for this session" \
                  "could not fetch the library catalog" \
                  "could not fetch the module index" \
                  "contract is not sound" "did not contain both a json" \
                  "duplicate addModel" "SDK not found" \
                  "two manifests claim" "already running against this module pack"; do
        line="$(printf '%s' "$out" | grep -m 1 -A 1 -F "$marker" | tr '\n' ' ')"
        if [ -n "$line" ]; then
            printf '%s' "$(printf '%s' "$line" | sed 's/  */ /g; s/^ //; s/ $//')"
            return
        fi
    done
    printf '%s' "$(printf '%s' "$out" | tail -2 | head -1)"
}

ok()   { printf '  PASS  %s\n' "$*"; pass=$((pass + 1)); }
bad()  { printf '  FAIL  %s\n' "$*"; fail=$((fail + 1)); }
none() { printf '  SKIP  %s\n' "$*"; skip=$((skip + 1)); }
step() { printf '\n%s\n' "$*"; }

# --- surface one: the command line -------------------------------------------
if [ "$ONLY" = "all" ] || [ "$ONLY" = "cli" ]; then
    step "1. the CLI"
    if [ ! -d "$TOOLS" ]; then
        none "no toolchain at $TOOLS"
    elif ! (cd "$TOOLS" && python3 -c "
import sys; sys.path.insert(0,'.')
import patch as P
sys.exit(0 if P.inventory() else 1)
" 2>/dev/null); then
        none "the module inventory is empty — VCV Rack is not installed here, so
        there is nothing to build a patch out of"
    else
        out="$(cd "$TOOLS" && cap 1500 python3 patch.py build \
              "a classic subtractive voice with a filter envelope" 2>&1)"
        path="$(printf '%s' "$out" | sed -n 's/.*cables → \(.*\)$/\1/p' | tail -1)"
        if [ -n "$path" ] && [ -f "$path" ]; then
            # Not "the log said built" -- the file, and the idiom it claimed.
            v="$(cd "$TOOLS" && python3 idiom_check.py "$path" subtractive-voice 2>&1)"
            if printf '%s' "$v" | grep -q holds; then
                ok "a patch was generated and holds its idiom: $(basename "$path")"
            else
                bad "generated, but not the patch that was asked for: $v"
            fi
        else
            bad "the CLI did not produce a patch: $(generator_reason "$out")"
        fi
    fi
fi

# --- surface two: the app, by clicking ---------------------------------------
if [ "$ONLY" = "all" ] || [ "$ONLY" = "app" ]; then
    step "2. the standalone app, by pressing Build"
    # Both locations, because the app installs to ~/Applications and only
    # /Applications was looked at -- so on a machine where the app WAS
    # installed this step reported "not installed here" and skipped itself.
    APP=""
    for candidate in "$HOME/Applications/Forge Modular.app" \
                     "/Applications/Forge Modular.app"; do
        [ -d "$candidate" ] && APP="$candidate" && break
    done
    if [ -z "$APP" ]; then
        none "the app is not installed here"
    elif ! pgrep -x "loginwindow" >/dev/null 2>&1; then
        none "no GUI session — the window cannot be driven from a terminal alone"
    elif screen_is_locked; then
        # A LOCKED screen is a logged-in session, so the loginwindow check above
        # passes and this used to run anyway. It cannot work: loginwindow owns
        # every point on the display, the app never becomes frontmost, and the
        # driver rightly refuses to click. Worse, Skia cannot create a GPU
        # context behind the lock, so the app logs a flood of dropped draws --
        # which reads exactly like a rendering regression and is not one. That
        # misdiagnosis is the reason this branch exists.
        none "the screen is locked — the app cannot take focus or a GPU surface.
        Unlock it and re-run; this is a skip, not a failure of the app"
    else
        printf '  launching the app; it will take an audio device for up to 25 minutes\n'
        open -a "$APP"
        sleep 6
        out="$(cd "$HERE" && cap 1500 python3 drive_app.py patch \
              "a classic subtractive voice with a filter envelope" 2>&1)"
        printf '%s\n' "$out" | tail -3 | sed 's/^/      /'
        if printf '%s' "$out" | grep -q "^SKIP:"; then
            # Not a failure of the app: the driver reads the screen to know
            # which pane it is on, and an SSH session has no Screen Recording
            # permission. Reported as the skip it is.
            none "the app cannot be driven from here — no Screen Recording
        permission. Run this from a Terminal window ON this machine."
        elif printf '%s' "$out" | grep -q "^PASS"; then
            ok "the app built a patch when Build was pressed"
        else
            bad "driving the app did not reach a pass"
        fi
        osascript -e 'tell application "Forge Modular" to quit' 2>/dev/null
    fi
fi

# --- surface three: a DAW ----------------------------------------------------
if [ "$ONLY" = "all" ] || [ "$ONLY" = "daw" ]; then
    step "3. REAPER, all three formats"
    if [ ! -d /Applications/REAPER.app ]; then
        none "REAPER is not installed here"
    else
        for fmt in AU VST3 CLAP; do
            case "$fmt" in
                AU)   p=~/Library/Audio/Plug-Ins/Components/"Forge Modular.component" ;;
                VST3) p=~/Library/Audio/Plug-Ins/VST3/"Forge Modular.vst3" ;;
                CLAP) p=~/Library/Audio/Plug-Ins/CLAP/"Forge Modular.clap" ;;
            esac
            if [ ! -e "$p" ]; then
                none "$fmt is not installed"
            elif codesign --verify --deep --strict "$p" 2>/dev/null; then
                # Signature and presence is what can be asserted without a
                # human at the machine; loading each format in REAPER is the
                # part a person watches, and saying otherwise would be a lie.
                ok "$fmt is installed and its signature verifies"
            else
                bad "$fmt is installed but its signature does not verify"
            fi
        done
        none "loading each format in REAPER and generating from inside it is the
        part that needs a person at the machine"
    fi
fi

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
