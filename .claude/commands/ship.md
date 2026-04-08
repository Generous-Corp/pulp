---
name: ship
description: Sign, package, notarize, and distribute a Pulp plugin or app
---

Ship a Pulp plugin or app — sign, notarize, package, and generate update feeds.

Available subcommands:

**Signing:**
- `pulp ship sign --identity "Developer ID Application: ..."` — macOS/Windows code sign
- `pulp ship sign --target android --keystore key.jks --key-alias mykey` — Android APK/AAB signing

**Packaging:**
- `pulp ship package --version 1.0.0` — create installer (.pkg on macOS, NSIS on Windows)
- `pulp ship package --target android --keystore key.jks` — build Android APK + AAB via Gradle
- `pulp ship package --target android --abi all` — build for all ABIs (arm64-v8a, x86_64, armeabi-v7a)
- `pulp ship package --target android --aab-only` — AAB only (for Play Store)

**Notarization (macOS):**
- `pulp ship notarize --apple-id you@example.com --team-id ABCDE12345` — submit + staple
- `pulp ship notarize --staple` — staple only (already submitted)

**Update feeds:**
- `pulp ship appcast --url https://example.com/Plugin-1.0.pkg --version 1.0.0 --notes "..."` — Sparkle XML

**Status:**
- `pulp ship check` — desktop plugin signing status
- `pulp ship check --target android` — Android APK/AAB signing status

Run the appropriate subcommand based on $ARGUMENTS. If no arguments, show signing status first with `pulp ship check`.

For full CI-driven shipping (PR + validate + merge + release), use the `ci` skill instead: say "ship this" or use `/ci ship`.
