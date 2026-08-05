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
#     [--app-scripts "Title" DIR]...      (repeatable; a pkgbuild --scripts
#                                          directory for the app of that title)
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
declare -a S_TITLE S_DIR         # app scripts: app title + pkgbuild --scripts directory
declare -a C_TITLE C_DESC C_DEST C_SRC  # content: title + description + install dest + source dir

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
    --app-scripts) S_TITLE+=("$2"); S_DIR+=("$3"); shift 3;;
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
# A choice with a real payload. $1=unique choice-id  $2=title  $3=desc  $4=pkgfile
add_ref() {
  local title desc lock; title="$(xml_escape "$2")"; desc="$(xml_escape "$3")"
  # $5="required" locks the choice ON and greys it out.
  #
  # The app is not one option among four. It carries the Rack plug-in, the
  # uninstaller and the first-run SDK fetch, so deselecting it leaves the
  # plug-in formats with nothing to generate patches, no way to remove the
  # software, and no route to a compiler. An installer that can reach a state
  # like that and then complain is worse than one that cannot reach it.
  lock=""
  [ "${5:-}" = "required" ] && lock=' enabled="false" selected="true"'
  DEFS="$DEFS<choice id=\"$1\" title=\"$title\" description=\"$desc\"$lock><pkg-ref id=\"com.pulp.$NAME.$1.pkg\"/></choice>"
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

# Outline: one expandable group per plugin when there is more than one; a flat
# list of formats when there is only one (nothing to disambiguate).
NPLUG="${#PLUGIN_NAMES[@]}"
if [[ "$NPLUG" -le 1 ]]; then
  CHOICES="$CHOICES$(printf '%s' "$PLUGIN_ENTRIES" | awk -F'\t' 'NF{printf "<line choice=\"%s\"/>",$3}')"
else
  for ((j=0; j<NPLUG; j++)); do
    pn="${PLUGIN_NAMES[$j]}"
    gid="plugin-$j"
    DEFS="$DEFS<choice id=\"$gid\" title=\"$(xml_escape "$pn")\" description=\"\" selected=\"true\"></choice>"
    inner="$(printf '%s' "$PLUGIN_ENTRIES" | awk -F'\t' -v p="$j" 'NF && $1==p{printf "<line choice=\"%s\"/>",$3}')"
    CHOICES="$CHOICES<line choice=\"$gid\">$inner</line>"
  done
fi

echo "== apps → /Applications =="
ANY_SCRIPTS=0
for ((i=0; i<${#A_TITLE[@]}; i++)); do
  t="${A_TITLE[$i]}"; p="${A_PATH[$i]}"; ent="${A_ENT[$i]}"; [[ -d "$p" ]] || { echo "missing: $p" >&2; exit 2; }
  deep_sign "$p" "$ent"
  id="$(echo "$t" | tr ' A-Z' '-a-z' | tr -cd 'a-z0-9-')"
  r="$STAGE/root-$id"; mkdir -p "$r/Applications"; cp -R "$p" "$r/Applications/"
  f="$(basename "$p").pkg"
  # An installer can only write absolute paths, as root. Anything that has to
  # land in the logged-in person's home directory -- a Rack plug-in folder, a
  # preference seed -- needs a script, so a component may carry one. macOS ships
  # bash 3.2 and has no associative arrays, so the title is looked up by scan.
  SCRIPT_ARGS=()
  for ((s=0; s<${#S_TITLE[@]}; s++)); do
    [[ "${S_TITLE[$s]}" == "$t" ]] || continue
    [[ -d "${S_DIR[$s]}" ]] || { echo "missing scripts dir: ${S_DIR[$s]}" >&2; exit 2; }
    # pkgbuild accepts a --scripts directory whose postinstall is not
    # executable and silently ships one that never runs, which is the same
    # nothing as having no script at all.
    for hook in preinstall postinstall; do
      if [[ -f "${S_DIR[$s]}/$hook" && ! -x "${S_DIR[$s]}/$hook" ]]; then
        echo "scripts/$hook is not executable — it would ship and never run" >&2
        exit 2
      fi
    done
    SCRIPT_ARGS=(--scripts "${S_DIR[$s]}")
    ANY_SCRIPTS=1
    echo "   scripts: ${S_DIR[$s]}"
    break
  done
  pkgbuild --root "$r" --identifier "com.pulp.$NAME.$id.pkg" --version "$VERSION" \
    "${SCRIPT_ARGS[@]+"${SCRIPT_ARGS[@]}"}" \
    --install-location / "$STAGE/comp/$f" >/dev/null
  add_ref "$id" "$t" "Required. Includes the Rack plug-in and uninstaller." "$f" required
  CHOICES="$CHOICES<line choice=\"$id\"/>"   # apps sit at the top level
done

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

# A consent pane, when the caller supplies one.
#
# Apple's own agree-to-install screen: the user reads it and clicks Agree
# before anything is written. That is where consent belongs -- they are already
# deciding to install, and it is the only moment a person reliably reads.
# Forge Modular uses it to say that the VCV Rack SDK is VCV's, is GPLv3, and is
# downloaded from them rather than shipped by us.
LICENSE_ARGS=()
if [ -n "${PKG_LICENSE_FILE:-}" ] && [ -f "$PKG_LICENSE_FILE" ]; then
  mkdir -p "$STAGE/resources"
  cp "$PKG_LICENSE_FILE" "$STAGE/resources/license.txt"
  LICENSE_LINE='<license file="license.txt"/>'
  LICENSE_ARGS=(--resources "$STAGE/resources")
else
  LICENSE_LINE=""
fi

# require-scripts tells Installer that a component's scripts are part of the
# product rather than optional decoration. Left "false" while a component
# carries a postinstall, an install that skipped scripts would report success
# having placed nothing -- the failure this whole mechanism exists to prevent.
REQUIRE_SCRIPTS=false
[[ "$ANY_SCRIPTS" == 1 ]] && REQUIRE_SCRIPTS=true

cat > "$STAGE/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>$NAME $VERSION</title><organization>com.pulp</organization>
  <options customize="always" require-scripts="$REQUIRE_SCRIPTS" hostArchitectures="arm64"/>
  $LICENSE_LINE
  <choices-outline>$CHOICES</choices-outline>
  $DEFS
  $REFS
</installer-gui-script>
XML
PKG="$OUT/$NAME-$VERSION.pkg"
# `${LICENSE_ARGS[@]+...}` rather than a bare `"${LICENSE_ARGS[@]}"`: macOS ships
# bash 3.2, where expanding an EMPTY array under `set -u` is an unbound-variable
# error, not an empty list. Every install without a license file -- the default --
# died here before productbuild ran, so the script exited 1 having produced no
# package at all. It only ever worked on a machine that set PKG_LICENSE_FILE.
productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE/comp" ${LICENSE_ARGS[@]+"${LICENSE_ARGS[@]}"} --sign "$INST_ID" "$PKG" >/dev/null
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
