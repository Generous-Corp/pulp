---
name: ship
description: "Sign, package, notarize, and distribute Pulp plugins and apps across macOS, Windows, and Android. Handles code signing (codesign, signtool, apksigner), Apple notarization, installers (.pkg, NSIS, APK, AAB), Sparkle appcast feeds, and signing status checks."
---

Ship a Pulp plugin or app — sign, notarize, package, and generate update feeds.

## Before running any ship command

Check prerequisites for the target platform:

**macOS:** Run `security find-identity -v -p codesigning` to verify signing identity exists. Check `pulp config show` for saved credentials.

**Android:** Verify `ANDROID_HOME` is set (`pulp doctor` checks this). Verify keystore file exists. Verify `android/` Gradle project exists.

**Windows:** Verify signing certificate is installed. Verify NSIS is on PATH for packaging.

If credentials are missing, the CLI shows rich error messages with setup guidance. Credentials can be saved globally via `pulp config set` so they carry across projects.

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
- `pulp ship sign --target android --keystore key.jks --key-alias mykey --store-pass @env:PASS`

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

Run the appropriate subcommand based on $ARGUMENTS. If no arguments, show signing status first with `pulp ship check`, then suggest the next step in the workflow.

For full CI-driven shipping (PR + validate + merge + release), use the `ci` skill instead: say "ship this" or use `/ci ship`.
