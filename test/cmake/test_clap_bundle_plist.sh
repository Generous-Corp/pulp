#!/usr/bin/env bash
# CLAP bundles must carry a real Info.plist.
#
# CMake's default bundle plist leaves CFBundleIdentifier, CFBundleName and
# CFBundleShortVersionString EMPTY and stamps CFBundlePackageType as APPL rather
# than BNDL. codesign then falls back to a synthesised `<name>-<hash>`
# identifier: tolerable for adhoc, but a Developer ID signature and
# notarisation want a real bundle id, and the signed artifact is otherwise
# unidentifiable. Every shipped Pulp CLAP had this until the template landed.
#
# Configure-only and hermetic: this asserts the template exists, renders the
# required keys, AND that _pulp_add_clap actually wires it — a template nothing
# references would pass a content-only check while shipping the broken default.
set -uo pipefail

ROOT="${1:?usage: test_clap_bundle_plist.sh <source-root>}"
TEMPLATE="$ROOT/tools/cmake/PulpInfoPlist.clap.in"
FORMATS="$ROOT/tools/cmake/PulpPluginFormats.cmake"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

[ -f "$TEMPLATE" ] || { bad "missing $TEMPLATE"; echo "clap-bundle-plist: FAILED" >&2; exit 1; }

# 1. Every key codesign/notarisation needs is present and non-placeholder-empty.
for key in CFBundleIdentifier CFBundleName CFBundleExecutable \
           CFBundleShortVersionString CFBundleVersion CFBundlePackageType; do
    grep -q "<key>$key</key>" "$TEMPLATE" || bad "template lacks $key"
done

# 2. Package type must be BNDL — APPL is what the broken default produced.
grep -A1 '<key>CFBundlePackageType</key>' "$TEMPLATE" | grep -q '<string>BNDL</string>' \
    || bad "CFBundlePackageType must be BNDL (APPL is the broken CMake default)"

# 3. The identifier must be substituted from the plugin's bundle id, and
#    suffixed so a CLAP cannot collide with the VST3/AU of the same plugin.
grep -A1 '<key>CFBundleIdentifier</key>' "$TEMPLATE" | grep -q '@PULP_BUNDLE_ID@\.clap' \
    || bad "CFBundleIdentifier must be @PULP_BUNDLE_ID@.clap"

# 4. Name/version must be substituted, not hardcoded or blank.
grep -A1 '<key>CFBundleName</key>' "$TEMPLATE" | grep -q '@PULP_PLUGIN_NAME@' \
    || bad "CFBundleName must be @PULP_PLUGIN_NAME@"
grep -A1 '<key>CFBundleShortVersionString</key>' "$TEMPLATE" | grep -q '@PULP_VERSION@' \
    || bad "CFBundleShortVersionString must be @PULP_VERSION@"

# 5. The wiring. Without this the template is dead and the default still ships.
grep -q 'PulpInfoPlist.clap.in' "$FORMATS" \
    || bad "_pulp_add_clap does not configure PulpInfoPlist.clap.in"
grep -q 'MACOSX_BUNDLE_INFO_PLIST.*_Info.plist.clap' "$FORMATS" \
    || bad "_pulp_add_clap does not set MACOSX_BUNDLE_INFO_PLIST for the CLAP target"

# 6. Rendering it must leave no unsubstituted @PLACEHOLDER@ behind, must yield
#    the suffixed identifier, and must contain no empty value — the exact three
#    symptoms of the CMake default this template replaces.
RENDERED="$(mktemp)"
trap 'rm -f "$RENDERED"' EXIT
sed -e 's/@PULP_PLUGIN_NAME@/TestPlug/g' \
    -e 's/@PULP_BUNDLE_ID@/com.example.testplug/g' \
    -e 's/@PULP_VERSION@/1.2.3/g' "$TEMPLATE" > "$RENDERED"

if grep -qE '@[A-Z_]+@' "$RENDERED"; then
    bad "unsubstituted placeholders: $(grep -oE '@[A-Z_]+@' "$RENDERED" | sort -u | tr '\n' ' ')"
fi
if ! grep -qF '<string>com.example.testplug.clap</string>' "$RENDERED"; then
    bad "rendered identifier is not <bundle-id>.clap"
fi
if grep -qF '<string></string>' "$RENDERED"; then
    bad "rendered plist still contains an EMPTY <string></string> value"
fi

if [ "$fail" = "0" ]; then
    echo "clap-bundle-plist: OK"
    exit 0
fi
echo "clap-bundle-plist: FAILED" >&2
exit 1
