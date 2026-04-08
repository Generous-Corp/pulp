---
name: ship
description: "Sign, package, notarize, and distribute Pulp plugins and apps across macOS, Windows, and Android. Handles code signing (codesign, signtool, apksigner), Apple notarization, installers (.pkg, NSIS, APK, AAB), Sparkle appcast feeds, and signing status checks."
allowed-tools:
  - Read
  - Bash
  - Glob
  - Grep
  - AskUserQuestion
---

Ship a Pulp plugin or app — sign, notarize, package, and generate update feeds.

## Before running any ship command

1. Run `pulp config show` to check saved credentials.
2. If no config exists, OR if the config file exists but all signing fields are commented out (only section headers shown), use AskUserQuestion to offer setup:
   - "No signing config found. Would you like to set up signing credentials now?"
   - Options: "Yes, set up macOS signing" / "Yes, set up Android signing" / "Skip for now"
   - If yes, walk through `pulp config set` for each required field.

3. Check prerequisites for the target platform:
   - **macOS:** Run `security find-identity -v -p codesigning` to verify identity exists.
   - **Android:** Verify `ANDROID_HOME` is set. Verify keystore file exists. Verify `android/` Gradle project exists.
   - **Windows:** Verify signing certificate is installed. Verify NSIS is on PATH.

4. If prerequisites are missing, use AskUserQuestion to offer solutions:
   - "Android SDK not found. Would you like help setting it up?"
   - Options: "Show install instructions" / "I'll set it up myself" / "Skip Android"

## Standard workflow order

1. **Build** — `pulp build` (must complete before signing)
2. **Sign** — `pulp ship sign` (must sign before notarizing)
3. **Notarize** — `pulp ship notarize` (macOS only, after signing)
4. **Package** — `pulp ship package` (creates installer from signed bundles)
5. **Appcast** — `pulp ship appcast` (generate update feed pointing to packaged artifact)

## Subcommands

**Signing:**
- `pulp ship sign` — uses identity from `~/.pulp/config.toml` (macOS/Windows)
- `pulp ship sign --identity "Developer ID Application: ..."` — explicit identity
- `pulp ship sign --target android` — uses keystore from config
- `pulp ship sign --target android --keystore key.jks --key-alias mykey --store-pass @env:PASS --key-pass @env:KEY_PASS`

Note: `--key-pass` defaults to `--store-pass` if omitted. `sign --target android` operates on existing APK/AAB files in `artifacts/` — use `package --target android` to build from Gradle.

**Notarization (macOS only):**
- `pulp ship notarize` — uses apple_id/team_id from config
- `pulp ship notarize --apple-id you@example.com --team-id ABCDE12345 --password @keychain:AC_PASSWORD`
- `pulp ship notarize --staple` — staple only (skip submission)

**Packaging:**
- `pulp ship package --version 1.0.0` — .pkg (macOS) or NSIS .exe (Windows)
- `pulp ship package --target android --keystore key.jks` — APK + AAB via Gradle
- `pulp ship package --target android --abi all` — all ABIs (arm64-v8a, x86_64, armeabi-v7a)
- `pulp ship package --target android --aab-only` — AAB only (for Play Store)
- `pulp ship package --target android --apk-only` — APK only (for direct distribution)
- `pulp ship package --per-user` — per-user NSIS install (Windows, no admin)

**Update feeds:**
- `pulp ship appcast --url https://example.com/Plugin.pkg --version 1.0.0 --notes "Bug fixes"`
- `pulp ship appcast --sign-key ~/keys/ed25519_private.key` — EdDSA-signed for Sparkle
- `pulp ship appcast --output appcast.xml --title "My Plugin" --min-os 12.0`

**Status:**
- `pulp ship check` — desktop plugin signing status
- `pulp ship check --target android` — Android APK/AAB signing status (v2/v3 scheme, signer CN)

## Common errors

- **"No signing identity"** → Run `security find-identity -v -p codesigning` or `pulp config set signing.apple.identity "..."`
- **"No Android keystore"** → Create one: `keytool -genkey -v -keystore release.jks -keyalg RSA -keysize 2048 -validity 10000`
- **"Android SDK not found"** → Install Android Studio or `export ANDROID_HOME=~/Library/Android/sdk`
- **"Notarization failed"** → Ensure hardened runtime is enabled and using Developer ID (not development) cert. Check log: `xcrun notarytool log <UUID>`
- **"NSIS not found"** → Install NSIS and add to PATH (Windows only)
- **"Gradle build failed"** → Run `pulp doctor` to check SDK/NDK/Java versions

## Config

All signing credentials fall back to `~/.pulp/config.toml` (CLI flag > env var > config file):

```bash
pulp config init                    # Create from template
pulp config set signing.apple.identity "Developer ID Application: ..."
pulp config set signing.apple.team_id "ABCDE12345"
pulp config set signing.android.keystore "~/keystores/release.jks"
pulp config show                    # Show current values
```

## Interactive review pattern

Before executing destructive or external actions (signing, notarizing, uploading), show a summary of what will happen and use AskUserQuestion to confirm:

```
Ready to sign 3 bundles with:
  Identity: Developer ID Application: Dan Raffel (ABCDE12345)  (from: config.toml)
  Bundles:  PulpGain.vst3, PulpGain.clap, PulpGain.component

Proceed?
  - Yes, sign all
  - Edit signing identity first
  - Cancel
```

If the user chooses "Edit signing identity first", use AskUserQuestion to collect the new value (show the current value as context), then offer to save it with `pulp config set`.

Similarly for notarize:
```
Ready to notarize 3 bundles:
  Apple ID: dan@example.com  (from: config.toml)
  Team ID:  ABCDE12345       (from: config.toml)
  Password: @keychain:AC_PASSWORD

Proceed?
  - Yes, submit for notarization
  - Edit credentials first
  - Cancel
```

And for Android packaging:
```
Ready to build Android package:
  Gradle project: android/
  ABIs: arm64-v8a (phones + tablets)
  Signing: debug (no keystore configured)

Proceed?
  - Yes, build with debug signing
  - Set up release keystore first
  - Cancel
```

This "show then confirm" pattern applies to:
- `pulp ship sign` — show identity/keystore and bundles
- `pulp ship notarize` — show credentials and bundles
- `pulp ship package` — show target, version, and signing mode
- `pulp ship package --target android` — show ABIs, keystore, Gradle project

Run the appropriate subcommand based on $ARGUMENTS. If no arguments, run `pulp ship check` to show current signing status, then use AskUserQuestion to suggest next steps:
- "Sign plugin bundles" (if unsigned bundles found)
- "Set up signing credentials" (if no config)
- "Package for distribution" (if bundles are signed)
- "Generate appcast update feed" (if packages exist in artifacts/)

For full CI-driven shipping (PR + validate + merge + release), use the `ci` skill instead: say "ship this" or use `/ci ship`.
