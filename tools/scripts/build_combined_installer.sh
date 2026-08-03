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
#     [--product-title BUNDLE "Display Title"]...  (repeatable; renames the
#                                                   expandable group for that
#                                                   plugin, e.g. PulpDesignSynth
#                                                   -> "Kelvin (instrument)")
#     [--app "Title" PATH [ENTITLEMENTS]]...  (repeatable; installs to /Applications)
#     [--app-for BUNDLE "Title" PATH [ENTITLEMENTS]]...  (repeatable; same, but
#                                                   nests the app INSIDE that
#                                                   plugin's group and makes it
#                                                   REQUIRED — use for a
#                                                   standalone that carries the
#                                                   uninstaller, which a user
#                                                   must not be able to skip)
#     [--content "Title" "Desc" DEST SRCDIR]...  (repeatable; installs SRCDIR's
#                                                 contents to DEST, e.g. sample
#                                                 models/IRs into Application Support)
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
declare -a C_TITLE C_DESC C_DEST C_SRC  # content: title + description + install dest + source dir
declare -a A_GROUP                      # apps: plugin name to nest under ("" = top level)
declare -a PT_NAME PT_TITLE             # product display titles: bundle name -> title

while [[ $# -gt 0 ]]; do
  case "$1" in
    --name) NAME="$2"; shift 2;;
    --version) VERSION="$2"; shift 2;;
    --sign-identity) APP_ID="$2"; shift 2;;
    --installer-identity) INST_ID="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --no-notarize) NOTARIZE=0; shift;;
    --plugin) P_KIND+=("$2"); P_PATH+=("$3"); shift 3;;
    --product-title) PT_NAME+=("$2"); PT_TITLE+=("$3"); shift 3;;
    --app)
      A_TITLE+=("$2"); A_PATH+=("$3"); A_GROUP+=("")
      if [[ "${4:-}" == --* || -z "${4:-}" ]]; then A_ENT+=(""); shift 3; else A_ENT+=("$4"); shift 4; fi;;
    --app-for)
      A_GROUP+=("$2"); A_TITLE+=("$3"); A_PATH+=("$4")
      if [[ "${5:-}" == --* || -z "${5:-}" ]]; then A_ENT+=(""); shift 4; else A_ENT+=("$5"); shift 5; fi;;
    --content) C_TITLE+=("$2"); C_DESC+=("$3"); C_DEST+=("$4"); C_SRC+=("$5"); shift 5;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done
[[ -n "$NAME" && -n "$VERSION" && -n "$APP_ID" && -n "$INST_ID" && -n "$OUT" ]] || {
  echo "missing required args (--name --version --sign-identity --installer-identity --out)" >&2; exit 2; }

STAGE="$(mktemp -d)"; mkdir -p "$OUT" "$STAGE/comp"
trap 'rm -rf "$STAGE"' EXIT   # clean the staging tree on any exit (success, error, signal)
if [[ -f ~/.config/pulp/secrets/keychain.env ]]; then
  source ~/.config/pulp/secrets/keychain.env
fi

# Non-interactive signing preflight — reuse the codified `pulp ship doctor` setup
# (ensure_signing_ready.sh, the SAME script `pulp ship sign` runs) so a fresh
# agent / SSH / CI session gets the dedicated signing keychain created, unlocked,
# its inactivity auto-lock disabled, added to the search list, and authorized for
# codesign via set-key-partition-list — all from the stored secret. Without it,
# `codesign -s <hash>` falls through to the LOCKED login keychain and pops a GUI
# password dialog an unattended sign can't answer; worse, that dialog asks for the
# *dedicated* keychain's stored password, not the user's login password, so a
# login password is rejected. Single source of truth — no inline keychain juggling
# here, and it covers the fresh-machine case (keychain/.p12 not yet imported) too.
_self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${PULP_SKIP_SIGNING_PREFLIGHT:-0}" != 1 && -x "$_self_dir/ensure_signing_ready.sh" ]]; then
  "$_self_dir/ensure_signing_ready.sh" >/dev/null 2>&1 \
    && echo "[installer] signing keychain ready (pulp ship doctor preflight)" \
    || echo "[installer] WARN: ensure_signing_ready.sh returned non-zero — signing may prompt; run 'pulp ship doctor'" >&2
fi

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

xml_escape() {  # escape XML metacharacters so titles/descriptions with & < > " ' stay valid
  local s="$1"
  s="${s//&/&amp;}"; s="${s//</&lt;}"; s="${s//>/&gt;}"
  s="${s//\"/&quot;}"; s="${s//\'/&apos;}"
  printf '%s' "$s"
}

CHOICES=""; DEFS=""; REFS=""; APP_LINES=""
# Nested-app bookkeeping. Initialized because `set -u` is on and an installer
# built with no --app-for would otherwise abort on the first unset expansion.
APP_GROUP_LINES=""    # one "pluginName<TAB>choiceId" line per nested app
APP_TOP_LINES=""      # <line> elements for ungrouped apps
REQUIRED_APP_IDS=""   # choice ids to mark non-deselectable
# A choice with a real payload. $1=unique choice-id  $2=title  $3=desc  $4=pkgfile
add_ref() {
  local title desc; title="$(xml_escape "$2")"; desc="$(xml_escape "$3")"
  DEFS="$DEFS<choice id=\"$1\" title=\"$title\" description=\"$desc\"><pkg-ref id=\"com.pulp.$NAME.$1.pkg\"/></choice>"
  REFS="$REFS<pkg-ref id=\"com.pulp.$NAME.$1.pkg\" version=\"$VERSION\">$4</pkg-ref>"
}
# Plugin bundles are grouped by plugin name so the installer can nest formats
# under each plugin. Keying packages by plugin+format (not by format alone) is
# what makes a multi-plugin installer work at all -- otherwise every plugin's AU
# package shares one identifier and only the last survives. IDs use the plugin's
# first-seen index rather than a lossy name slug: "Foo-Bar" and "Foo Bar" must
# remain distinct products. macOS ships bash 3.2 (no associative arrays), so a
# small indexed-array lookup assigns each plugin its stable index.
PLUGIN_NAMES=()
PLUGIN_ENTRIES=""    # one "pluginIndex<TAB>pluginName<TAB>choiceId" line per format
echo "== plugins =="
for ((i=0; i<${#P_KIND[@]}; i++)); do
  k="${P_KIND[$i]}"; p="${P_PATH[$i]}"; [[ -d "$p" ]] || { echo "missing: $p" >&2; exit 2; }
  deep_sign "$p"
  pname="$(basename "$p")"; pname="${pname%.*}"        # e.g. VaDrum
  plugin_idx=-1
  for ((j=0; j<${#PLUGIN_NAMES[@]}; j++)); do
    [[ "${PLUGIN_NAMES[$j]}" == "$pname" ]] && { plugin_idx="$j"; break; }
  done
  if [[ "$plugin_idx" -lt 0 ]]; then
    plugin_idx="${#PLUGIN_NAMES[@]}"
    PLUGIN_NAMES+=("$pname")
  fi
  cid="plugin-${plugin_idx}-${k}"                       # unique per plugin+format
  f="${pname}.${k}.pkg"
  pkgbuild --component "$p" --identifier "com.pulp.$NAME.$cid.pkg" --version "$VERSION" \
    --install-location "$(plugin_dir "$k")" "$STAGE/comp/$f" >/dev/null
  case "$k" in au) d="Logic, GarageBand";; vst3) d="Most DAWs";; clap) d="REAPER, Bitwig";; esac
  add_ref "$cid" "$(echo "$k" | tr a-z A-Z)" "$d" "$f"
  PLUGIN_ENTRIES="${PLUGIN_ENTRIES}${plugin_idx}	${pname}	${cid}
"
done

echo "== apps → /Applications =="
for ((i=0; i<${#A_TITLE[@]}; i++)); do
  t="${A_TITLE[$i]}"; p="${A_PATH[$i]}"; ent="${A_ENT[$i]}"; [[ -d "$p" ]] || { echo "missing: $p" >&2; exit 2; }
  deep_sign "$p" "$ent"
  id="$(echo "$t" | tr ' A-Z' '-a-z' | tr -cd 'a-z0-9-')"
  r="$STAGE/root-$id"; mkdir -p "$r/Applications"; cp -R "$p" "$r/Applications/"
  # pkgbuild otherwise marks app bundles relocatable. On a development machine
  # where Launch Services already knows another copy of the same bundle ID,
  # Installer then overwrites that build-tree copy instead of installing the
  # selected app in /Applications. Pin every staged app to its package path.
  component_plist="$STAGE/$id-components.plist"
  pkgbuild --analyze --root "$r" "$component_plist" >/dev/null
  component_index=0
  while /usr/libexec/PlistBuddy -c "Print :$component_index" \
      "$component_plist" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c \
      "Set :$component_index:BundleIsRelocatable false" "$component_plist"
    component_index=$((component_index + 1))
  done
  [[ "$component_index" -gt 0 ]] || {
    echo "error: pkgbuild found no app bundles under $r" >&2
    exit 2
  }
  f="$(basename "$p").pkg"
  pkgbuild --root "$r" --component-plist "$component_plist" \
    --identifier "com.pulp.$NAME.$id.pkg" --version "$VERSION" \
    --install-location / "$STAGE/comp/$f" >/dev/null
  add_ref "$id" "$t" "$t" "$f"
  if [[ -n "${A_GROUP[$i]}" ]]; then
    # Nested under its product's group, and REQUIRED. The standalone carries
    # the uninstaller, so a user who deselects it installs plugins they cannot
    # later remove. `enabled="false" selected="true"` shows the row and refuses
    # the checkbox, which is honest about it rather than hiding the row.
    APP_GROUP_LINES="${APP_GROUP_LINES}${A_GROUP[$i]}	${id}
"
    REQUIRED_APP_IDS="$REQUIRED_APP_IDS $id"
  else
    APP_TOP_LINES="$APP_TOP_LINES<line choice=\"$id\"/>"  # ungrouped: top level
  fi
done

# Mark every nested app choice non-deselectable. Done after add_ref so it
# rewrites the choice the loop already emitted rather than racing it.
for rid in $REQUIRED_APP_IDS; do
  DEFS="${DEFS//<choice id=\"$rid\" title=/<choice id=\"$rid\" enabled=\"false\" selected=\"true\" title=}"
done

# Outline: one expandable group per product when there is more than one; a flat
# list of formats when there is only one (nothing to disambiguate).
#
# Built AFTER the app loop so a product's standalone can be nested inside its
# own group. A user thinks in products — "Kelvin, and which formats of it" —
# not in a flat list where the same plugin appears once as a format group and
# again as an app under a different name.
NPLUG="${#PLUGIN_NAMES[@]}"
if [[ "$NPLUG" -le 1 ]]; then
  CHOICES="$CHOICES$(printf '%s' "$PLUGIN_ENTRIES" | awk -F'\t' 'NF{printf "<line choice=\"%s\"/>",$3}')"
  CHOICES="$CHOICES$(printf '%s' "$APP_GROUP_LINES" | awk -F'\t' 'NF{printf "<line choice=\"%s\"/>",$2}')"
else
  for ((j=0; j<NPLUG; j++)); do
    pn="${PLUGIN_NAMES[$j]}"
    # A display title overrides the bundle name, so the group can read
    # "Kelvin (instrument)" rather than "PulpDesignSynth".
    ptitle="$pn"
    for ((k=0; k<${#PT_NAME[@]}; k++)); do
      [[ "${PT_NAME[$k]}" == "$pn" ]] && ptitle="${PT_TITLE[$k]}"
    done
    gid="plugin-$j"
    DEFS="$DEFS<choice id=\"$gid\" title=\"$(xml_escape "$ptitle")\" description=\"\" selected=\"true\"></choice>"
    inner="$(printf '%s' "$PLUGIN_ENTRIES" | awk -F'\t' -v p="$j" 'NF && $1==p{printf "<line choice=\"%s\"/>",$3}')"
    inner="$inner$(printf '%s' "$APP_GROUP_LINES" | awk -F'\t' -v n="$pn" 'NF && $1==n{printf "<line choice=\"%s\"/>",$2}')"
    CHOICES="$CHOICES<line choice=\"$gid\">$inner</line>"
  done
fi
CHOICES="$CHOICES$APP_TOP_LINES"

echo "== content =="
for ((i=0; i<${#C_TITLE[@]}; i++)); do
  t="${C_TITLE[$i]}"; desc="${C_DESC[$i]}"; dest="${C_DEST[$i]}"; src="${C_SRC[$i]}"
  [[ -d "$src" ]] || { echo "missing: $src" >&2; exit 2; }
  id="content-$(echo "$t" | tr ' A-Z' '-a-z' | tr -cd 'a-z0-9-')"
  r="$STAGE/root-$id"; mkdir -p "$r$dest"; cp -R "$src/." "$r$dest/"
  f="$id.pkg"
  pkgbuild --root "$r" --identifier "com.pulp.$NAME.$id.pkg" --version "$VERSION" \
    --install-location / "$STAGE/comp/$f" >/dev/null
  add_ref "$id" "$t" "$desc" "$f"
  CHOICES="$CHOICES<line choice=\"$id\"/>"   # content sits at the top level
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
  if [[ -x "$CLI" ]]; then
    # In-tree / top-level builds: the C++ CLI is built and drives notarize+staple.
    "$CLI" ship notarize --path "$PKG"
  else
    # Submodule / standalone consumers never build pulp-cpp (it is gated to
    # top-level Pulp builds), so fall back to notarytool directly using the
    # file-based App Store Connect key. Secrets live in ~/.config/pulp/secrets.
    if [[ -f ~/.config/pulp/secrets/notary.env ]]; then
      source ~/.config/pulp/secrets/notary.env
    fi
    : "${PULP_NOTARY_KEY_PATH:=$HOME/.config/pulp/secrets/AuthKey_${PULP_NOTARY_KEY_ID:-}.p8}"
    if [[ -z "${PULP_NOTARY_KEY_ID:-}" || -z "${PULP_NOTARY_ISSUER_ID:-}" || ! -f "$PULP_NOTARY_KEY_PATH" ]]; then
      echo "error: cannot notarize — pulp-cpp is not built and no notary key is configured." >&2
      echo "  Build the Pulp CLI (top-level build) or set PULP_NOTARY_KEY_ID / PULP_NOTARY_ISSUER_ID" >&2
      echo "  and place the .p8 in ~/.config/pulp/secrets/ (see 'pulp ship doctor'). Or pass --no-notarize." >&2
      exit 1
    fi
    xcrun notarytool submit "$PKG" \
      --key "$PULP_NOTARY_KEY_PATH" --key-id "$PULP_NOTARY_KEY_ID" \
      --issuer "$PULP_NOTARY_ISSUER_ID" --wait
    xcrun stapler staple "$PKG"
  fi
  xcrun stapler validate "$PKG"
fi
echo "OK → $PKG"
# staging tree removed by the EXIT trap
