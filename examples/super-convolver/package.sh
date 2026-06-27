#!/bin/bash
# Codified release packaging for SuperConvolver — ONE component-selectable,
# signed + notarized installer that bundles EVERYTHING: the standalone app, the
# AU/VST3/CLAP plugins, and (when present) the Diagnostics helper.
#
# Why this script exists: earlier releases got packaging wrong by reaching for
# `pulp ship package` (which emits a SEPARATE per-format .pkg) and `pulp ship
# share` (a SEPARATE per-app .dmg). The right shape for a user is a SINGLE .pkg
# whose Customize pane lets them pick AU / VST3 / CLAP / Standalone / Diagnostics.
# This script is the canonical recipe so that never drifts again. (The intent is
# to graduate this into `pulp ship package --combined --app … --component …`.)
#
# Inputs (env overrides; sensible defaults):
#   VER           version string                 (default: read from CMake bundle)
#   BUILD         pulp build dir                  (default: ../../build from here)
#   DIAG_APP      path to a built Diagnostics.app (optional; included if it exists)
#   APP_ID        Developer ID Application hash   (signs bundles/apps)
#   INST_ID       Developer ID Installer hash     (signs the .pkg)
#   OUT           output dir                      (default: /tmp/sc-release)
#
# Steps: deep-sign every bundle (inner dylibs first → @loader_path relocatable,
# validated by check_bundle_relocatable.py) → pkgbuild one component per artifact
# (plugins to their plug-in dirs, apps to /Applications) → productbuild a single
# component-selectable distribution → notarize + staple the one .pkg.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
CLI="$BUILD/tools/cli/pulp-cpp"
VALIDATOR="$ROOT/tools/cmake/scripts/check_bundle_relocatable.py"
VER="${VER:-1.0.3}"
APP_ID="${APP_ID:-D10A184D5A207EAA926955447DC27E2AD965DFB8}"
INST_ID="${INST_ID:-0E91CD0D8592220A75AE9D13D4031E36472EE58D}"
DIAG_APP="${DIAG_APP:-/Volumes/Workshop/Code/pulp-diagnostickit/build/SuperConvolver Diagnostics.app}"
DIAG_ENT="${DIAG_ENT:-/Volumes/Workshop/Code/pulp-diagnostickit/DiagnosticKit.entitlements}"
OUT="${OUT:-/tmp/sc-release}"
STAGE="$(mktemp -d)"; mkdir -p "$OUT" "$STAGE/comp"
source ~/.config/pulp/secrets/keychain.env 2>/dev/null || true

deep_sign() {  # $1=bundle  $2(optional)=entitlements
  local b="$1" ent="${2:-}"
  find "$b/Contents/MacOS" -name "*.dylib" -print0 2>/dev/null | while IFS= read -r -d '' d; do
    codesign --force --options runtime --timestamp -s "$APP_ID" "$d"; done
  if [ -n "$ent" ]; then codesign --force --options runtime --timestamp --entitlements "$ent" -s "$APP_ID" "$b"
  else codesign --force --options runtime --timestamp -s "$APP_ID" "$b"; fi
  codesign --verify --deep --strict "$b"
  python3 "$VALIDATOR" "$b" --strict   # guard: never ship a non-relocatable bundle
}

comp() {  # id  install-loc  src-bundle  → a component .pkg in $STAGE/comp
  pkgbuild --component "$3" --identifier "$1" --version "$VER" --install-location "$2" \
    "$STAGE/comp/$(basename "$3").pkg" >/dev/null
}
app_comp() {  # id  app-path  → component installing the app to /Applications
  local r="$STAGE/root-$(basename "$2")"; mkdir -p "$r/Applications"; cp -R "$2" "$r/Applications/"
  pkgbuild --root "$r" --identifier "$1" --version "$VER" --install-location / \
    "$STAGE/comp/$(basename "$2").pkg" >/dev/null
}

echo "== deep-sign + relocatability-validate all bundles =="
deep_sign "$BUILD/AU/SuperConvolver.component"
deep_sign "$BUILD/VST3/SuperConvolver.vst3"
deep_sign "$BUILD/CLAP/SuperConvolver.clap"
deep_sign "$BUILD/examples/super-convolver/SuperConvolver.app"
LINES=""; OUTLINE=""
comp com.pulp.superconvolver.au.pkg   /Library/Audio/Plug-Ins/Components "$BUILD/AU/SuperConvolver.component"
comp com.pulp.superconvolver.vst3.pkg /Library/Audio/Plug-Ins/VST3       "$BUILD/VST3/SuperConvolver.vst3"
comp com.pulp.superconvolver.clap.pkg /Library/Audio/Plug-Ins/CLAP       "$BUILD/CLAP/SuperConvolver.clap"
app_comp com.pulp.superconvolver.standalone.pkg "$BUILD/examples/super-convolver/SuperConvolver.app"
CHOICES='<line choice="au"/><line choice="vst3"/><line choice="clap"/><line choice="standalone"/>'
DEFS='<choice id="au" title="Audio Unit (AU)" description="Logic, GarageBand"><pkg-ref id="com.pulp.superconvolver.au.pkg"/></choice>
<choice id="vst3" title="VST3" description="Most DAWs"><pkg-ref id="com.pulp.superconvolver.vst3.pkg"/></choice>
<choice id="clap" title="CLAP" description="REAPER, Bitwig"><pkg-ref id="com.pulp.superconvolver.clap.pkg"/></choice>
<choice id="standalone" title="Standalone app" description="SuperConvolver.app → Applications"><pkg-ref id="com.pulp.superconvolver.standalone.pkg"/></choice>'
REFS='<pkg-ref id="com.pulp.superconvolver.au.pkg" version="'$VER'">SuperConvolver.component.pkg</pkg-ref>
<pkg-ref id="com.pulp.superconvolver.vst3.pkg" version="'$VER'">SuperConvolver.vst3.pkg</pkg-ref>
<pkg-ref id="com.pulp.superconvolver.clap.pkg" version="'$VER'">SuperConvolver.clap.pkg</pkg-ref>
<pkg-ref id="com.pulp.superconvolver.standalone.pkg" version="'$VER'">SuperConvolver.app.pkg</pkg-ref>'

if [ -d "$DIAG_APP" ]; then
  echo "== including Diagnostics =="
  deep_sign "$DIAG_APP" "$DIAG_ENT"
  app_comp com.pulp.superconvolver.diagnostics.pkg "$DIAG_APP"
  CHOICES="$CHOICES"'<line choice="diagnostics"/>'
  DEFS="$DEFS"'
<choice id="diagnostics" title="Diagnostics helper" description="If a plugin won'\''t load, run it → report .zip on your Desktop"><pkg-ref id="com.pulp.superconvolver.diagnostics.pkg"/></choice>'
  REFS="$REFS"'
<pkg-ref id="com.pulp.superconvolver.diagnostics.pkg" version="'$VER'">SuperConvolver Diagnostics.app.pkg</pkg-ref>'
fi

cat > "$STAGE/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>SuperConvolver $VER</title><organization>com.pulp</organization>
  <options customize="always" require-scripts="false" hostArchitectures="arm64"/>
  <choices-outline>$CHOICES</choices-outline>
  $DEFS
  $REFS
</installer-gui-script>
XML
productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/comp" \
  --sign "$INST_ID" "$OUT/SuperConvolver-$VER.pkg" >/dev/null
echo "== notarize + staple the single installer =="
"$CLI" ship notarize --path "$OUT/SuperConvolver-$VER.pkg"
xcrun stapler validate "$OUT/SuperConvolver-$VER.pkg" && echo "OK → $OUT/SuperConvolver-$VER.pkg"
rm -rf "$STAGE"
