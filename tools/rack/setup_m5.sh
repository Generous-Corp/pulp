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
# Quit the app BEFORE replacing it.
#
# rm -rf + rsync over a bundle whose binary is mapped into a running process
# is a live executable being deleted and rewritten underneath itself. Both of
# this machine's unexplained crashes happened in that window — at startup, in
# CoreFoundation and CoreText, with malloc reporting a corrupted free list,
# which is what a half-replaced image looks like from the inside. Every other
# crash here has a fix commit within minutes of it; these two do not, and this
# is the difference between them.
remote '
if pgrep -f "/Applications/Forge Modular.app/Contents/MacOS" >/dev/null 2>&1; then
    echo "  the app is running — quitting it before replacing it"
    osascript -e '"'"'tell application "Forge Modular" to quit'"'"' >/dev/null 2>&1
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -f "/Applications/Forge Modular.app/Contents/MacOS" >/dev/null 2>&1 || break
        sleep 1
    done
    if pgrep -f "/Applications/Forge Modular.app/Contents/MacOS" >/dev/null 2>&1; then
        echo "  IT WOULD NOT QUIT — refusing to overwrite a running binary."
        echo "  Quit Forge Modular and run this again."
        exit 3
    fi
fi
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

# A UI proof that runs ON this machine, as one command.
#
# The arrow keys can only be proven against a live window: what was wrong with
# them was WHICH VIEW the window dispatched to, and nothing in-process can see
# that. Accessibility and Screen Recording cannot be granted over SSH, so this
# cannot run from here — but leaving a human to work out the invocation is how
# a proof stops being run. It gets a one-word launcher and a straight answer
# about whether it can work yet.
step "8. the UI proof"
remote 'mkdir -p ~/bin
cat > ~/bin/forge-modular-prove <<'"'"'LAUNCH'"'"'
#!/usr/bin/env bash
# Drive the installed Forge Modular and prove the mention list answers keys.
cd "$HOME/Library/Application Support/Forge Modular/tools/rack" || exit 1
exec python3 prove_arrows.py "$@"
LAUNCH
chmod +x ~/bin/forge-modular-prove
# ~/bin is NOT on the PATH by default on macOS, and printing the bare name
# assumed it was: the first person to follow this got "command not found" and
# had to be told the real path. Say what actually works on THIS machine.
case ":$PATH:" in
    *":$HOME/bin:"*) echo "  installed: forge-modular-prove (~/bin is on your PATH)" ;;
    *) echo "  installed: ~/bin/forge-modular-prove"
       echo "             (~/bin is not on your PATH, so use the full path —"
       echo "              or add it: echo 'export PATH=\"\$HOME/bin:\$PATH\"' >> ~/.zshrc)" ;;
esac
python3 - <<'"'"'PY'"'"'
import subprocess, os
# Both permissions are per-APPLICATION (the terminal running this), so they are
# reported rather than assumed. A refusal here is about the terminal, never the
# app under test.
ok = True
r = subprocess.run(["screencapture", "-x", "-R", "0,0,4,4", "/tmp/fm-perm.png"],
                   capture_output=True)
if r.returncode != 0:
    # Over SSH it can NEVER be granted -- the permission belongs to the
    # terminal application, and there is no terminal here. Saying "not granted"
    # in that case reads as a misconfigured machine, which it is not.
    if os.environ.get("SSH_CONNECTION") or os.environ.get("SSH_TTY"):
        print("  Screen Recording: n/a over SSH — this is expected")
    else:
        print("  Screen Recording: NOT granted to this terminal")
    ok = False
else:
    print("  Screen Recording: granted")
    os.remove("/tmp/fm-perm.png")
if ok:
    print("  run it on this machine with:  ~/bin/forge-modular-prove")
elif os.environ.get("SSH_CONNECTION") or os.environ.get("SSH_TTY"):
    print("  in a Terminal ON this machine, run:  ~/bin/forge-modular-prove")
else:
    print("  grant it in System Settings > Privacy & Security, then run:")
    print("      ~/bin/forge-modular-prove")
PY'

# Does it actually START on this machine?
#
# Everything above proves the bundle is present, signed, notarized, stapled and
# indexed. None of it launches the thing. A build was signed and shipped here
# without ever being run, and it crashed on the first launch — into a
# three-day-old startup crash nobody had counted, because a crash that happens
# before a window appears leaves no trace anyone reads.
#
# So: launch it, wait, and see whether it is still alive. Then count the crash
# reports, because "it survived this time" is not the same as "it is well" for
# a fault that is intermittent.
step "9. does it start"
remote 'before=$(ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null | grep -c "^Forge Modular")
open -a /Applications/"Forge Modular.app" 2>/dev/null
alive=0
for i in $(seq 1 12); do
    sleep 1
    pgrep -f "/Applications/Forge Modular.app/Contents/MacOS" >/dev/null && alive=1 || alive=0
done
after=$(ls ~/Library/Logs/DiagnosticReports/ 2>/dev/null | grep -c "^Forge Modular")
if [ "$alive" = 1 ]; then
    echo "  it launched and was still running after 12s"
else
    echo "  IT DID NOT SURVIVE LAUNCH"
fi
if [ "$after" -gt "$before" ]; then
    echo "  and it left $((after - before)) new crash report(s) — see"
    echo "  ~/Library/Logs/DiagnosticReports"
fi
echo "  crash reports on this machine, all time: $after"
osascript -e '"'"'tell application "Forge Modular" to quit'"'"' >/dev/null 2>&1
'

step "10. signature and quarantine"
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

step "done. Step 10 proves it: CLI, the app by clicking Build, and REAPER."
