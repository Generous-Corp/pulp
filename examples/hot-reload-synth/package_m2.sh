#!/usr/bin/env bash
# Build the signed M2 installer: the hot-reload SYNTH (instrument) — AU/VST3/CLAP/
# standalone — plus its sine/saw logic variants + a swap.sh, seeding the sine
# variant to the watched path on install. Reproducible replacement for the M1
# ad-hoc pkg build. Requires the built artifacts + signing creds.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$root/build"
out="${1:-$here/dist}"
: "${PULP_SECRETS:=$HOME/.config/pulp/secrets}"
# shellcheck disable=SC1091
source "$PULP_SECRETS/keychain.env"
security unlock-keychain -p "$PULP_SIGN_KEYCHAIN_PW" "$PULP_SIGN_KEYCHAIN" 2>/dev/null || true
APP="$PULP_SIGN_IDENTITY_HASH"; INST="$PULP_SIGN_INSTALLER_HASH"; KC="$PULP_SIGN_KEYCHAIN"
sign() { codesign --force --timestamp --options runtime --sign "$APP" --keychain "$KC" "$1"; }

AU="$build/AU/Pulp Hot-Reload Synth.component"
VST3="$build/VST3/Pulp Hot-Reload Synth.vst3"
CLAP="$build/CLAP/Pulp Hot-Reload Synth.clap"
APPB="$build/examples/hot-reload-synth/Pulp Hot-Reload Synth.app"
SINE="$build/examples/hot-reload-synth/logic.dylib"
SAW="$build/examples/hot-reload-synth/logic-saw.dylib"
for f in "$AU" "$VST3" "$CLAP" "$APPB" "$SINE" "$SAW"; do [ -e "$f" ] || { echo "MISSING: $f"; exit 1; }; done

echo "== sign artifacts =="
sign "$SINE"; sign "$SAW"
sign "$AU"; sign "$VST3"; sign "$CLAP"; sign "$APPB"

echo "== assemble pkgroot =="
rm -rf "$out"; pkgroot="$out/root"; scripts="$out/scripts"
mkdir -p "$pkgroot/Library/Audio/Plug-Ins/Components" \
         "$pkgroot/Library/Audio/Plug-Ins/VST3" \
         "$pkgroot/Library/Audio/Plug-Ins/CLAP" \
         "$pkgroot/Applications" \
         "$pkgroot/Library/Application Support/PulpHotReloadM2" "$scripts"
cp -R "$AU"  "$pkgroot/Library/Audio/Plug-Ins/Components/"
cp -R "$VST3" "$pkgroot/Library/Audio/Plug-Ins/VST3/"
cp -R "$CLAP" "$pkgroot/Library/Audio/Plug-Ins/CLAP/"
cp -R "$APPB" "$pkgroot/Applications/"
cp "$SINE" "$pkgroot/Library/Application Support/PulpHotReloadM2/logic-sine.dylib"
cp "$SAW"  "$pkgroot/Library/Application Support/PulpHotReloadM2/logic-saw.dylib"
cat > "$pkgroot/Library/Application Support/PulpHotReloadM2/swap.sh" <<'SW'
#!/bin/bash
# Hot-swap the running synth's oscillator with no reopen: ./swap.sh sine|saw
v="${1:-}"; case "$v" in sine|saw);; *) echo "usage: $0 sine|saw"; exit 2;; esac
dest="$HOME/.pulp/hot-reload-synth/logic.dylib"; mkdir -p "$(dirname "$dest")"
cp "/Library/Application Support/PulpHotReloadM2/logic-$v.dylib" "$dest" && touch "$dest"
echo "swapped -> $v (hold a chord in the synth and you'll hear the timbre morph)"
SW
chmod +x "$pkgroot/Library/Application Support/PulpHotReloadM2/swap.sh"

cat > "$scripts/postinstall" <<'PI'
#!/bin/bash
# Seed the sine variant to the console user's watched path so the synth sounds
# on first load (the AU watches $HOME/.pulp/hot-reload-synth/logic.dylib).
u=$(stat -f%Su /dev/console); h=$(dscl . -read "/Users/$u" NFSHomeDirectory | awk '{print $2}')
d="$h/.pulp/hot-reload-synth"; mkdir -p "$d"
cp "/Library/Application Support/PulpHotReloadM2/logic-sine.dylib" "$d/logic.dylib"
chown -R "$u" "$h/.pulp"
exit 0
PI
chmod +x "$scripts/postinstall"

echo "== pkgbuild + productbuild (signed) =="
mkdir -p "$out"
pkgbuild --root "$pkgroot" --scripts "$scripts" \
    --identifier com.pulp.hot-reload-synth.m2 --version 1.0.0 \
    --install-location / "$out/component.pkg"
productbuild --package "$out/component.pkg" --sign "$INST" --keychain "$KC" \
    "$out/Pulp-Hot-Reload-M2.pkg"
echo "== done: $out/Pulp-Hot-Reload-M2.pkg =="
pkgutil --check-signature "$out/Pulp-Hot-Reload-M2.pkg" 2>&1 | head -3
