#!/bin/bash
# Build ONE component-selectable, signed + notarized macOS installer (.pkg) for a
# Pulp plugin: the standalone app, the AU/VST3/CLAP plugins, and any extra apps
# (e.g. a Diagnostics helper) — all selectable in the installer's Customize pane.
#
# This is the canonical Pulp packaging recipe. Earlier ad-hoc releases got it
# wrong by reaching for `pulp ship package` (a SEPARATE per-format .pkg) and
# `pulp ship share` (a SEPARATE per-app .dmg) piecemeal; the right shape for a
# user is a single installer. Use this instead. (Intended to graduate into
# `pulp ship package --combined`.)
#
# Each bundle is deep-signed (inner dylibs first → @loader_path relocatable) and
# validated with check_bundle_relocatable.py before packaging, so a build that
# only works on the build machine never ships.
#
# Usage:
#   build_combined_installer.sh \
#     --name NAME --version X.Y.Z \
#     --sign-identity <Developer ID Application hash> \
#     --installer-identity <Developer ID Installer hash> \
#     --out DIR \
#     [--plugin au|vst3|clap PATH]...     (repeatable)
#     [--app "Title" PATH [ENTITLEMENTS]]...  (repeatable; installs to /Applications)
#     [--no-notarize]
#
# Example (see examples/super-convolver/package.sh for a real invocation).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VALIDATOR="$ROOT/tools/cmake/scripts/check_bundle_relocatable.py"
CLI="${PULP_CPP:-$ROOT/build/tools/cli/pulp-cpp}"

NAME=""; VERSION=""; APP_ID=""; INST_ID=""; OUT=""; NOTARIZE=1
# Parallel arrays of components.
declare -a P_KIND P_PATH      # plugins: kind + bundle path
declare -a A_TITLE A_PATH A_ENT  # apps: choice title + bundle path + entitlements (or "")

while [[ $# -gt 0 ]]; do
  case "$1" in
    --name) NAME="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --sign-identity) APP_ID="$2"; shift 2;;
    --installer-identity) INST_ID="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --no-notarize) NOTARIZE=0; shift;;
    --plugin) P_KIND+=("$2"); P_PATH+=("$3"); shift 3;;
    --app)
      A_TITLE+=("$2"); A_PATH+=("$3")
      if [[ "${4:-}" == --* || -z "${4:-}" ]]; then A_ENT+=(""); shift 3; else A_ENT+=("$4"); shift 4; fi;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[[ -n "$NAME" && -n "$VERSION" && -n "$APP_ID" && -n "$INST_ID" && -n "$OUT" ]] || {
  echo "missing required args (--name --version --sign-identity --installer-identity --out)" >&2; exit 2; }

STAGE="$(mktemp -d)"; mkdir -p "$OUT" "$STAGE/comp"
source ~/.config/pulp/secrets/keychain.env 2>/dev/null || true

deep_sign() {  # $1=bundle  $2=entitlements(optional)
  local b="$1" ent="${2:-}"
  find "$b/Contents/MacOS" -name "*.dylib" -print0 2>/dev/null | while IFS= read -r -d '' d; do
    codesign --force --options runtime --timestamp -s "$APP_ID" "$d"; done
  if [[ -n "$ent" ]]; then codesign --force --options runtime --timestamp --entitlements "$ent" -s "$APP_ID" "$b"
  else codesign --force --options runtime --timestamp -s "$APP_ID" "$b"; fi
  codesign --verify --deep --strict "$b"
  [[ -f "$VALIDATOR" ]] && python3 "$VALIDATOR" "$b" --strict
}

plugin_dir() { case "$1" in
    au) echo /Library/Audio/Plug-Ins/Components;; vst3) echo /Library/Audio/Plug-Ins/VST3;;
    clap) echo /Library/Audio/Plug-Ins/CLAP;; *) echo "bad plugin kind: $1" >&2; exit 2;; esac; }

CHOICES=""; DEFS=""; REFS=""
add_ref() {  # $1=choice-id  $2=title  $3=desc  $4=pkgfile
  CHOICES="$CHOICES<line choice=\"$1\"/>"
  DEFS="$DEFS<choice id=\"$1\" title=\"$2\" description=\"$3\"><pkg-ref id=\"com.pulp.$NAME.$1.pkg\"/></choice>"
  REFS="$REFS<pkg-ref id=\"com.pulp.$NAME.$1.pkg\" version=\"$VERSION\">$4</pkg-ref>"
}

echo "== plugins =="
for i in "${!P_KIND[@]:-}"; do
  k="${P_KIND[$i]}"; p="${P_PATH[$i]}"; [[ -d "$p" ]] || { echo "missing: $p" >&2; exit 2; }
  deep_sign "$p"
  f="$(basename "$p").pkg"
  pkgbuild --component "$p" --identifier "com.pulp.$NAME.$k.pkg" --version "$VERSION" \
    --install-location "$(plugin_dir "$k")" "$STAGE/comp/$f" >/dev/null
  case "$k" in au) d="Logic, GarageBand";; vst3) d="Most DAWs";; clap) d="REAPER, Bitwig";; esac
  add_ref "$k" "$(echo "$k" | tr a-z A-Z)" "$d" "$f"
done

echo "== apps → /Applications =="
for i in "${!A_TITLE[@]:-}"; do
  t="${A_TITLE[$i]}"; p="${A_PATH[$i]}"; ent="${A_ENT[$i]}"; [[ -d "$p" ]] || { echo "missing: $p" >&2; exit 2; }
  deep_sign "$p" "$ent"
  id="$(echo "$t" | tr ' A-Z' '-a-z' | tr -cd 'a-z0-9-')"
  r="$STAGE/root-$id"; mkdir -p "$r/Applications"; cp -R "$p" "$r/Applications/"
  f="$(basename "$p").pkg"
  pkgbuild --root "$r" --identifier "com.pulp.$NAME.$id.pkg" --version "$VERSION" \
    --install-location / "$STAGE/comp/$f" >/dev/null
  add_ref "$id" "$t" "$t" "$f"
done

cat > "$STAGE/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>$NAME $VERSION</title><organization>com.pulp</organization>
  <options customize="always" require-scripts="false" hostArchitectures="arm64"/>
  <choices-outline>$CHOICES</choices-outline>
  $DEFS
  $REFS
</installer-gui-script>
XML
PKG="$OUT/$NAME-$VERSION.pkg"
productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/comp" --sign "$INST_ID" "$PKG" >/dev/null
if [[ "$NOTARIZE" == 1 ]]; then
  "$CLI" ship notarize --path "$PKG"
  xcrun stapler validate "$PKG"
fi
echo "OK → $PKG"
rm -rf "$STAGE"
