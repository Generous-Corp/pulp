#!/bin/bash
# Run pulp-editor-open-oop-probe (see editor_open_oop_probe.mm): what a host
# window shows while an AU editor opens out of process, the way Logic hosts
# AU v2 plug-ins. Builds the probe into the build dir if it is missing.
#
#   tools/editor-open/editor_open_oop_probe.sh [--build-dir DIR] [--gui-session] -- \
#       --sub SpOt --fresh-instance --follow --no-anim --opens 3 --out /tmp/oop
#
# --gui-session  launch the probe in the logged-in user's GUI session through
#                LaunchServices. Needed over ssh: an ssh session reaches
#                neither the window server's composited image of the remote
#                view nor the user's registered Audio Units, so the probe
#                would report "invalidComponentID" or read back nothing.
#                (`launchctl asuser` needs root; `open -W --stdout` fails with
#                -10810, so the wrapper app redirects its own output.)
# The component must be installed where AUHostingService finds it
# (~/Library/Audio/Plug-Ins/Components); remove it afterwards.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build_dir="$root/build"
gui=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --gui-session) gui=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done
probe="$build_dir/tools/editor-open/pulp-editor-open-oop-probe"
if [ ! -x "$probe" ]; then
    echo "editor_open_oop_probe: building $probe" >&2
    "$root/tools/ci/governed-build.sh" cmake --build "$build_dir" --target pulp-editor-open-oop-probe >&2
fi
if [ "$gui" = 0 ]; then
    exec env PULP_AUDIO_DEVICE=null "$probe" "$@"
fi
app="${HOME}/Library/Caches/Pulp/editor-open-probe/OopProbe.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp "$probe" "$app/Contents/MacOS/probe"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>launch</string>
<key>CFBundleIdentifier</key><string>com.pulp.editor-open-oop-probe</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSUIElement</key><true/>
</dict></plist>
PLIST
cat > "$app/Contents/MacOS/launch" <<'LAUNCH'
#!/bin/sh
out="$1"; shift
export PULP_AUDIO_DEVICE=null
exec /usr/bin/caffeinate -u -d -i "$(dirname "$0")/probe" "$@" > "$out" 2>&1
LAUNCH
chmod +x "$app/Contents/MacOS/launch"
codesign --force --sign - "$app" >/dev/null 2>&1 || true
log=$(mktemp -t editor-open-oop-probe)
open -W -g -n "$app" --args "$log" "$@"
cat "$log"
if grep -q "^FAIL\|failed\|no view controller" "$log"; then exit 1; fi
