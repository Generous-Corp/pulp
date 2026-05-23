#!/usr/bin/env bash
# sign-notarize-auv3-mac.sh — sign + notarize a macOS AU v3 container app.
#
# Required environment variables:
#   APP_PATH      Absolute path to the .app (e.g. build/AUv3/ChainerSynth.app)
#   APPLE_ID      Apple ID email for notarization
#   TEAM_ID       10-character Apple Developer team identifier
#   APP_CERT      Code-signing identity (Developer ID Application "...")
#                 OR a SHA-1 fingerprint to disambiguate duplicate certs
#   APP_PWD       App-specific password (not your regular Apple ID password)
#
# Optional:
#   INSTALL_TO    Destination .app path (default: /Applications/<name>.app)
#   APPEX_ENTITLEMENTS  Absolute path to .appex entitlements plist
#   HOST_ENTITLEMENTS   Absolute path to host .app entitlements plist
#
# This implements the macOS AU v3 packaging recipe from
# planning/2026-05-23-auv3-macos-ui-phase35.md:
#   1. Sign nested dylibs (libwgpu_native.dylib, etc.) FIRST
#   2. Sign framework with hardened runtime
#   3. Sign .appex with hardened runtime + sandbox entitlements
#   4. Sign container .app with hardened runtime + entitlements
#   5. Zip + submit to notary (waits inline; usually 1-5 minutes)
#   6. Staple the notary ticket onto the .app
#   7. Verify with spctl
#   8. Optionally install to /Applications + lsregister + open

set -euo pipefail

: "${APP_PATH:?APP_PATH is required (path to .app)}"
: "${APPLE_ID:?APPLE_ID is required}"
: "${TEAM_ID:?TEAM_ID is required}"
: "${APP_CERT:?APP_CERT is required (Developer ID Application or SHA-1)}"
: "${APP_PWD:?APP_PWD is required (app-specific password)}"

APP_PATH="$(cd "$(dirname "$APP_PATH")" && pwd)/$(basename "$APP_PATH")"
APP_NAME="$(basename "$APP_PATH" .app)"
INSTALL_TO="${INSTALL_TO:-/Applications/${APP_NAME}.app}"

# Auto-discover the embedded .appex and framework.
APPEX="$(find "$APP_PATH/Contents/PlugIns" -maxdepth 1 -name '*.appex' | head -1)"
FRAMEWORK="$(find "$APP_PATH/Contents/Frameworks" -maxdepth 1 -name '*.framework' | head -1)"

[ -d "$APPEX" ]     || { echo "❌ No .appex found in $APP_PATH/Contents/PlugIns/" >&2; exit 1; }
[ -d "$FRAMEWORK" ] || { echo "❌ No .framework found in $APP_PATH/Contents/Frameworks/" >&2; exit 1; }

# Re-sync embedded framework + appex from their source build dirs. CMake's
# POST_BUILD that does the cp -RP only fires when the host target relinks,
# which doesn't happen on framework-only source changes. Without this, the
# embedded copies can be stale and signing+notarization succeed against
# the OLD binary while the user thinks they're testing the new one.
APP_PARENT="$(dirname "$APP_PATH")"
FW_NAME="$(basename "$FRAMEWORK")"
APPEX_NAME="$(basename "$APPEX")"
FW_SRC="$APP_PARENT/$FW_NAME"
APPEX_SRC="$APP_PARENT/$APPEX_NAME"
if [ -d "$FW_SRC" ]; then
    echo "🔁 Re-syncing embedded framework from $FW_SRC"
    rm -rf "$FRAMEWORK"
    cp -RP "$FW_SRC" "$APP_PATH/Contents/Frameworks/"
fi
if [ -d "$APPEX_SRC" ]; then
    echo "🔁 Re-syncing embedded appex from $APPEX_SRC"
    rm -rf "$APPEX"
    cp -RP "$APPEX_SRC" "$APP_PATH/Contents/PlugIns/"
fi

echo "🔐 Signing pipeline for AU v3:"
echo "   .app       = $APP_PATH"
echo "   .appex     = $APPEX"
echo "   .framework = $FRAMEWORK"
echo "   cert       = $APP_CERT"

# Default entitlements — match the templates shipped with Phase 3.5.
APPEX_ENTITLEMENTS="${APPEX_ENTITLEMENTS:-}"
HOST_ENTITLEMENTS="${HOST_ENTITLEMENTS:-}"

# Step 1: Sign nested dylibs first (innermost out).
# Walk framework Versions/A explicitly — `find Versions/Current` doesn't
# traverse the relative symlink. Without proper dylib signing here, notary
# rejects with "binary is not signed with a valid Developer ID certificate"
# (the dylibs ship with CMake/Skia's linker-only ad-hoc signature).
echo "🔐 1/5 Signing embedded dylibs..."
DYLIBS="$(find "$APPEX/Contents" -name '*.dylib' 2>/dev/null; \
          find "$FRAMEWORK/Versions" -type f \( -name '*.dylib' -o -name '*.framework' \) 2>/dev/null)"
if [ -z "$DYLIBS" ]; then
    echo "   (no nested dylibs found)"
fi
for dylib in $DYLIBS; do
    echo "   signing $dylib"
    codesign --force --sign "$APP_CERT" --timestamp --options runtime "$dylib"
done

# Step 2: Sign the framework.
echo "🔐 2/5 Signing framework..."
codesign --force --sign "$APP_CERT" --timestamp --options runtime "$FRAMEWORK"

# Step 3: Sign the .appex (with sandbox entitlements).
echo "🔐 3/5 Signing .appex..."
if [ -n "$APPEX_ENTITLEMENTS" ]; then
    codesign --force --sign "$APP_CERT" --timestamp --options runtime \
        --entitlements "$APPEX_ENTITLEMENTS" "$APPEX"
else
    codesign --force --sign "$APP_CERT" --timestamp --options runtime "$APPEX"
fi

# Step 4: Sign the container app.
echo "🔐 4/5 Signing container .app..."
if [ -n "$HOST_ENTITLEMENTS" ]; then
    codesign --force --sign "$APP_CERT" --timestamp --options runtime \
        --entitlements "$HOST_ENTITLEMENTS" "$APP_PATH"
else
    codesign --force --sign "$APP_CERT" --timestamp --options runtime "$APP_PATH"
fi

echo "🔐 5/5 Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP_PATH" 2>&1 | tail -5

# Step 6: Notarize + staple.
echo "📤 Submitting to Apple notary (1-5 min)..."
ZIP="${TMPDIR:-/tmp}/${APP_NAME}-notarize.zip"
rm -f "$ZIP"
ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$ZIP"
xcrun notarytool submit "$ZIP" \
    --apple-id "$APPLE_ID" \
    --team-id "$TEAM_ID" \
    --password "$APP_PWD" \
    --wait
rm -f "$ZIP"

echo "📎 Stapling notary ticket..."
xcrun stapler staple "$APP_PATH"

echo "✅ Verifying with spctl..."
spctl --assess --type execute --verbose "$APP_PATH"

# Step 7: Install + register with Launch Services.
echo "📦 Installing to $INSTALL_TO..."
rm -rf "$INSTALL_TO"
cp -R "$APP_PATH" "$INSTALL_TO"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f -R "$INSTALL_TO"

echo ""
echo "🎉 Done. Next steps:"
echo "   1. Open $INSTALL_TO once so Launch Services scans it."
echo "   2. Quit Logic / MainStage, relaunch — it will rescan AU components."
echo "   3. Look under AU Instruments / Effects → ${APP_NAME}."
echo ""
echo "   Verify Pluginkit registration with:"
echo "     pluginkit -mAvvv -p com.apple.AudioUnit-UI | grep ${APP_NAME}"
