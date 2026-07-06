#!/usr/bin/env bash
# Build the signed M2 installer: TWO hot-reload demos, both AU/VST3/CLAP/standalone,
# each with its logic variants + a swap script, seeding the initial variant on
# install. Reproducible replacement for the M1 ad-hoc pkg build.
#   SYNTH  (instrument, aumu) — swap.sh  sine|saw    (hear the oscillator morph)
#   MORPH  (effect, aufx)     — morph.sh warm|harsh  (SEE editor blue/red + HEAR sine/square)
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

# synth artifacts
S_AU="$build/AU/Pulp Hot-Reload Synth.component"
S_VST3="$build/VST3/Pulp Hot-Reload Synth.vst3"
S_CLAP="$build/CLAP/Pulp Hot-Reload Synth.clap"
S_APP="$build/examples/hot-reload-synth/Pulp Hot-Reload Synth.app"
S_SINE="$build/examples/hot-reload-synth/logic.dylib"
S_SAW="$build/examples/hot-reload-synth/logic-saw.dylib"
# morph artifacts
M_AU="$build/AU/Pulp Hot-Reload Morph.component"
M_VST3="$build/VST3/Pulp Hot-Reload Morph.vst3"
M_CLAP="$build/CLAP/Pulp Hot-Reload Morph.clap"
M_APP="$build/examples/hot-reload-morph/Pulp Hot-Reload Morph.app"
M_WARM="$build/examples/hot-reload-morph/logic.dylib"
M_HARSH="$build/examples/hot-reload-morph/logic-harsh.dylib"
for f in "$S_AU" "$S_VST3" "$S_CLAP" "$S_APP" "$S_SINE" "$S_SAW" \
         "$M_AU" "$M_VST3" "$M_CLAP" "$M_APP" "$M_WARM" "$M_HARSH"; do
    [ -e "$f" ] || { echo "MISSING: $f"; exit 1; }
done

echo "== sign artifacts =="
for f in "$S_SINE" "$S_SAW" "$M_WARM" "$M_HARSH" \
         "$S_AU" "$S_VST3" "$S_CLAP" "$S_APP" "$M_AU" "$M_VST3" "$M_CLAP" "$M_APP"; do
    sign "$f"
done

echo "== assemble pkgroot =="
rm -rf "$out"; pkgroot="$out/root"; scripts="$out/scripts"; sup="$pkgroot/Library/Application Support/PulpHotReloadM2"
mkdir -p "$pkgroot/Library/Audio/Plug-Ins/Components" \
         "$pkgroot/Library/Audio/Plug-Ins/VST3" \
         "$pkgroot/Library/Audio/Plug-Ins/CLAP" \
         "$pkgroot/Applications" "$sup" "$scripts"
cp -R "$S_AU" "$M_AU"     "$pkgroot/Library/Audio/Plug-Ins/Components/"
cp -R "$S_VST3" "$M_VST3" "$pkgroot/Library/Audio/Plug-Ins/VST3/"
cp -R "$S_CLAP" "$M_CLAP" "$pkgroot/Library/Audio/Plug-Ins/CLAP/"
cp -R "$S_APP" "$M_APP"   "$pkgroot/Applications/"
cp "$S_SINE" "$sup/synth-sine.dylib"; cp "$S_SAW" "$sup/synth-saw.dylib"
cp "$M_WARM" "$sup/morph-warm.dylib"; cp "$M_HARSH" "$sup/morph-harsh.dylib"

cat > "$sup/swap.sh" <<'SW'
#!/bin/bash
# Hot-swap the running SYNTH oscillator (no reopen): ./swap.sh sine|saw
v="${1:-}"; case "$v" in sine|saw);; *) echo "usage: $0 sine|saw"; exit 2;; esac
dest="$HOME/.pulp/hot-reload-synth/logic.dylib"; mkdir -p "$(dirname "$dest")"
cp "/Library/Application Support/PulpHotReloadM2/synth-$v.dylib" "$dest" && touch "$dest"
echo "synth -> $v (hold a chord; the timbre morphs live)"
SW
cat > "$sup/morph.sh" <<'MW'
#!/bin/bash
# Hot-swap the running MORPH DSP+editor (no reopen): ./morph.sh warm|harsh
v="${1:-}"; case "$v" in warm|harsh);; *) echo "usage: $0 warm|harsh"; exit 2;; esac
dest="$HOME/.pulp/hot-reload-morph/logic.dylib"; mkdir -p "$(dirname "$dest")"
cp "/Library/Application Support/PulpHotReloadM2/morph-$v.dylib" "$dest" && touch "$dest"
echo "morph -> $v (the editor flips blue WARM/red HARSH + the tremolo sine/square)"
MW
chmod +x "$sup/swap.sh" "$sup/morph.sh"

cat > "$scripts/postinstall" <<'PI'
#!/bin/bash
# Seed each demo's initial variant to the console user's watched path so the
# plugins sound + look right on first load.
u=$(stat -f%Su /dev/console); h=$(dscl . -read "/Users/$u" NFSHomeDirectory | awk '{print $2}')
sup="/Library/Application Support/PulpHotReloadM2"
mkdir -p "$h/.pulp/hot-reload-synth" "$h/.pulp/hot-reload-morph"
cp "$sup/synth-sine.dylib" "$h/.pulp/hot-reload-synth/logic.dylib"
cp "$sup/morph-warm.dylib" "$h/.pulp/hot-reload-morph/logic.dylib"
chown -R "$u" "$h/.pulp"
exit 0
PI
chmod +x "$scripts/postinstall"

echo "== pkgbuild + productbuild (signed) =="
mkdir -p "$out"
pkgbuild --root "$pkgroot" --scripts "$scripts" \
    --identifier com.pulp.hot-reload.m2 --version 1.0.0 \
    --install-location / "$out/component.pkg"
productbuild --package "$out/component.pkg" --sign "$INST" --keychain "$KC" \
    "$out/Pulp-Hot-Reload-M2.pkg"
echo "== done: $out/Pulp-Hot-Reload-M2.pkg =="
pkgutil --check-signature "$out/Pulp-Hot-Reload-M2.pkg" 2>&1 | head -3
