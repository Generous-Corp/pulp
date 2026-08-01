#!/usr/bin/env bash
# Set up a machine that has never run Forge Modular, from scratch.
#
# Everything here is idempotent and every step says what it did. Run from the
# machine that BUILT the binaries; it copies them over SSH.
#
#   tools/rack/setup_m5.sh              # the whole thing
#   tools/rack/setup_m5.sh --check      # report what is missing, change nothing
#
# The host defaults to `m5`; override with FORGE_HOST. What it does NOT do is
# install VCV Rack: that is a third-party application and a download, and it is
# the operator's call. --check reports its absence rather than deciding.

set -uo pipefail

HOST="${FORGE_HOST:-m5}"
BUILD="${FORGE_BUILD:-/tmp/forge-cur/build}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

say()  { printf '  %s\n' "$*"; }
step() { printf '\n%s\n' "$*"; }

remote() { ssh -o ConnectTimeout=10 "$HOST" "$@"; }

step "1. the machine"
if ! remote 'true' 2>/dev/null; then
    say "cannot reach $HOST over SSH"
    exit 2
fi
say "$(remote 'hostname; sw_vers -productVersion; uname -m' | tr '\n' ' ')"

step "2. what is already there"
remote '
for p in ~/Library/Audio/Plug-Ins/Components/"Forge Modular.component" \
         ~/Library/Audio/Plug-Ins/VST3/"Forge Modular.vst3" \
         ~/Library/Audio/Plug-Ins/CLAP/"Forge Modular.clap" \
         /Applications/"Forge Modular.app" \
         ~/Applications/"Forge Modular.app"; do
    [ -e "$p" ] && echo "  present: $p"
done
sub=$(plutil -extract AudioComponents.0.subtype raw \
      ~/Library/Audio/Plug-Ins/Components/"Forge Modular.component"/Contents/Info.plist 2>/dev/null)
[ -n "$sub" ] && echo "  installed AU subtype: $sub"
[ -d /Applications/"VCV Rack 2 Free.app" ] && echo "  VCV Rack: present" \
                                           || echo "  VCV Rack: MISSING"
[ -d /Applications/REAPER.app ] && echo "  REAPER: present" || echo "  REAPER: MISSING"
python3 -c "import PIL" 2>/dev/null && echo "  PIL: present" || echo "  PIL: MISSING"
'

if [ "$CHECK" -eq 1 ]; then
    step "check only; nothing was changed"
    exit 0
fi

# A stale bundle with a DIFFERENT AU subtype is not overwritten by copying the
# new one -- both register, and the DAW may scan either. Removing first is the
# only way the machine ends up with exactly what was built.
step "3. removing stale artifacts"
# The litter sweep is clean_installs.sh, sent over and run there rather than
# reimplemented inline. Two copies of a removal rule is two chances to differ
# about what is safe to delete, and this one deletes.
scp -q "$REPO/tools/rack/clean_installs.sh" "$HOST:/tmp/forge-clean-installs.sh" \
    && remote 'bash /tmp/forge-clean-installs.sh --yes; rm -f /tmp/forge-clean-installs.sh' \
    || say "could not send the cleaner — stale copies may remain"
remote '
rm -rf ~/Library/Audio/Plug-Ins/Components/"Forge Modular.component" \
       ~/Library/Audio/Plug-Ins/VST3/"Forge Modular.vst3" \
       ~/Library/Audio/Plug-Ins/CLAP/"Forge Modular.clap" \
       /Applications/"Forge Modular.app"
killall -9 AudioComponentRegistrar 2>/dev/null
echo "  removed, and the AU registrar restarted"
'

step "4. copying the signed bundles"
copy() { # <local> <remote dir>
    if [ ! -e "$1" ]; then say "MISSING locally: $1"; return 1; fi
    remote "mkdir -p '$2'"
    rsync -a --delete -e ssh "$1" "$HOST:$2/" && say "copied $(basename "$1")"
}
copy "$BUILD/AU/Forge Modular.component" '~/Library/Audio/Plug-Ins/Components'
copy "$BUILD/VST3/Forge Modular.vst3"    '~/Library/Audio/Plug-Ins/VST3'
copy "$BUILD/CLAP/Forge Modular.clap"    '~/Library/Audio/Plug-Ins/CLAP'
copy "$BUILD/modular/Forge Modular.app"  '/Applications'

step "5. the generator toolchain"
# Copied from the repo rather than from this machine's installed copy, so the
# new machine gets what is committed rather than whatever happens to be here.
#
# An ABSOLUTE remote path, resolved first, with its spaces escaped for the
# REMOTE shell: "Application Support/Forge Modular" is split by that shell
# otherwise, and rsync reports "server receiver mode requires two argument",
# which names neither the spaces nor the path. macOS ships openrsync, which
# has no --protect-args, so the escaping is done here. The bundle copies above
# worked only because their destinations happen to contain no spaces.
RHOME="$(remote 'echo $HOME')"
RSUPPORT="$RHOME/Library/Application Support/Forge Modular"
remote "mkdir -p '$RSUPPORT'"
RESC="${RSUPPORT// /\\ }"
rsync -a --exclude=__pycache__ --exclude='*.pyc' -e ssh \
      "$REPO/tools/" "$HOST:$RESC/tools/" \
      && say "tools copied" || say "TOOLS FAILED TO COPY"
rsync -a --exclude=__pycache__ -e ssh \
      "$REPO/examples/forge-modular/" \
      "$HOST:$RESC/examples/forge-modular/" \
      && say "module pack copied" || say "MODULE PACK FAILED TO COPY"

# The port map, if this machine has one.
#
# It is where a vendor module's jacks come from, and a machine without one
# draws every vendor module as a face with no holes, cables docking at the
# panel edge. That is how it was reported: our own modules had their jacks --
# they come from their manifests -- and the Core audio interface, the one
# vendor module in the patch, had none.
#
# Core's modules are compiled into Rack, so their geometry is the same
# everywhere and carrying the measurement is honest. Anything measured here
# for a plugin the target does not have is simply never looked up.
PORTMAP="$HOME/Library/Application Support/Rack2/forge-portmap.json"
if [ "$CHECK" -eq 0 ] && [ -f "$PORTMAP" ]; then
    remote 'mkdir -p ~/Library/Application\ Support/Rack2' 2>/dev/null
    if scp -q "$PORTMAP" "$HOST:Library/Application Support/Rack2/forge-portmap.json" 2>/dev/null; then
        say "port map copied ($(python3 -c 'import json,sys;print(len(json.load(open(sys.argv[1]))["modules"]))' "$PORTMAP" 2>/dev/null || echo '?') modules)"
    else
        say "PORT MAP FAILED TO COPY — vendor modules will draw without jacks"
    fi
elif [ ! -f "$PORTMAP" ]; then
    say "no port map here to copy — vendor modules draw without jacks until"
    say "  somebody presses SCAN in CARTOG on the target"
fi

step "6. proving the toolchain works there"
remote '
cd ~/Library/Application\ Support/"Forge Modular"/tools/rack 2>/dev/null || {
    echo "  toolchain did not arrive"; exit 1; }
python3 idiom_check.py --self-test --quiet 2>&1 | tail -1 | sed "s/^/  idioms: /"
python3 -c "
import sys; sys.path.insert(0,\".\")
import patch_vocabulary as V
print(\"  vocabulary:\", len(V.render().splitlines()), \"lines\")
"
'

# Spotlight does not notice a bundle that arrives by rsync. The app was
# installed, correct, signed and openable by LaunchServices -- and Cmd-Space
# found nothing, which is indistinguishable from "it did not install". mdimport
# is what puts it in the index.
step "7. making it findable"
# `touch` first, and it is the part that actually matters.
#
# rsync -a preserves the SOURCE directory's mtime, and a rebuild only changes
# the binary inside the bundle -- so the installed .app arrives looking older
# than the last index pass and Spotlight skips it as unchanged. The symptom is
# an app that is installed, signed, notarized, stapled, known to LaunchServices
# and invisible to Cmd-Space, with `mdls` reporting null for every attribute.
# mdimport alone does not fix it; touch + mdimport does, in about 10s.
#
# Indexing is asynchronous, so the check waits. Asking immediately reported
# CANNOT FIND IT for an app that was indexed seconds later, and a false alarm
# is as costly as a missed one -- the next person spends their time on
# Spotlight instead of on the app.
remote 'touch /Applications/"Forge Modular.app"
mdimport /Applications/"Forge Modular.app" >/dev/null 2>&1
found=0
for i in $(seq 1 20); do
    if mdfind '"'"'kMDItemContentType == "com.apple.application-bundle" && kMDItemDisplayName == "Forge Modular"'"'"' 2>/dev/null | grep -q .; then
        found=1; break
    fi
    sleep 1
done
if [ "$found" = 1 ]; then
    echo "  Spotlight can find it (after ${i}s)"
else
    echo "  SPOTLIGHT CANNOT FIND IT after 20s — open it from /Applications in Finder,"
    echo "  or run: mdimport -r /Applications/\"Forge Modular.app\""
fi'

step "8. signature and quarantine"
# A bundle that arrives without a valid signature will be refused by Gatekeeper
# on a machine that did not build it, which reads as "the plugin is broken".
remote '
for p in ~/Library/Audio/Plug-Ins/Components/"Forge Modular.component" \
         ~/Library/Audio/Plug-Ins/VST3/"Forge Modular.vst3" \
         ~/Library/Audio/Plug-Ins/CLAP/"Forge Modular.clap" \
         /Applications/"Forge Modular.app"; do
    [ -e "$p" ] || continue
    if codesign --verify --deep --strict "$p" 2>/dev/null; then
        printf "  signed:   %s\n" "$(basename "$p")"
    else
        printf "  UNSIGNED: %s\n" "$(basename "$p")"
    fi
    xattr -d com.apple.quarantine "$p" 2>/dev/null
done
'

step "done. Step 8 proves it: CLI, the app by clicking Build, and REAPER."
