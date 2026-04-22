---
name: ship
description: Sign, notarize, package, and distribute Pulp plugins and apps across macOS, Windows, and Android
triggers:
  - ship
  - sign
  - notarize
  - package
  - appcast
  - keystore
  - code signing
  - Play Store
  - release
  - distribute
  - installer
---

# Ship Skill — Signing, Packaging, and Distribution

## Overview

The `pulp ship` command handles the full distribution pipeline: code signing, Apple notarization, platform-specific packaging, update feed generation, and Android APK/AAB builds.

## Pre-flight: plugin ↔ CLI skew check

Before running `pulp ship ...`, source the shared skew-check helper so
a user on an outdated CLI sees a one-line hint (stderr, once per
session) when the installed CLI is older than the plugin's declared
`min_cli_version`:

```bash
source "$(git rev-parse --show-toplevel)/tools/scripts/cli_version_check.sh"
pulp_cli_version_check
```

Advisory only — never blocks. Full contract + override knobs in the
`upgrade` skill. Release-discovery Slice 6 (#551).

## Subcommands

| Command | Platform | What It Does |
|---------|----------|-------------|
| `sign` | macOS, Windows, Android | Sign plugin bundles or APK/AAB |
| `notarize` | macOS only | Submit to Apple notarization, poll, staple |
| `package` | All | Create .pkg (macOS), NSIS (Windows), APK+AAB (Android) |
| `appcast` | All | Generate Sparkle-compatible XML update feed |
| `check` | All | Verify signing status of built artifacts |

## Configuration

Settings are resolved in order: **CLI flag > environment variable > ~/.pulp/config.toml**

### Setup

```bash
pulp config init                    # Create config from template
pulp config set signing.apple.identity "Developer ID Application: Name (TEAMID)"
pulp config set signing.apple.team_id "ABCDE12345"
pulp config set signing.apple.apple_id "you@example.com"
pulp config set signing.android.keystore "~/keystores/release.jks"
```

### Config file location

`~/.pulp/config.toml` (override with `$PULP_HOME`)

See `config.example.toml` in the repo root for all options with documentation.

## Interactive safety contract

Before executing any signing, notarization, or packaging action, the `/ship` command uses AskUserQuestion to:
1. Show resolved config values (identity, keystore, credentials) and their source (CLI/env/config.toml)
2. Let the user review, edit, or cancel before proceeding
3. Offer to save new values with `pulp config set`

When invoked via skill trigger (not slash command), apply the same pattern: always show what will happen and confirm before executing.

## Workflows

### macOS: Build → Sign → Notarize → Package → Appcast

```bash
pulp build                                              # Must build first
pulp ship sign                                          # Uses identity from config
pulp ship notarize                                      # Uses apple_id/team_id from config
pulp ship package --version 1.0.0                       # Creates .pkg in artifacts/
pulp ship appcast --url https://example.com/Plugin.pkg --version 1.0.0
```

### Android: Build → Package (includes Gradle build + optional signing) → Verify

```bash
pulp build --target android                             # Build native libs
pulp ship package --target android --keystore ~/key.jks # Gradle build + sign APK/AAB
pulp ship check --target android                        # Verify APK/AAB signatures
```

Note: `pulp ship package --target android` invokes Gradle which builds AND signs in one step when a keystore is provided. Use `pulp ship sign --target android` only to re-sign existing artifacts in `artifacts/`.

### Windows: Build → Sign → Package

```bash
pulp build                                              # Must build first
pulp ship sign --identity "Your Company"                # Uses signtool
pulp ship package --version 1.0.0                       # Creates NSIS .exe installer
```

## Android-Specific

### ABI Selection

```bash
--abi arm64-v8a     # Default — ARM64 phones + tablets
--abi x86_64        # Emulator / Chromebook
--abi all           # arm64-v8a + x86_64 + armeabi-v7a
```

### Tablet Support

No special flags needed. ARM64 covers phones and tablets. AAB with split APKs handles screen density automatically.

### Keystore Creation

```bash
keytool -genkey -v -keystore release.jks -keyalg RSA -keysize 2048 -validity 10000
```

### Password Security

Never store passwords in plaintext config. Use environment variable references:
```toml
store_pass = "@env:ANDROID_STORE_PASS"
```

## Common Issues

### "No signing identity specified"

Run `security find-identity -v -p codesigning` (macOS) to find your identity, then:
```bash
pulp config set signing.apple.identity "Developer ID Application: ..."
```

### "Android SDK not found"

Install Android Studio or set `ANDROID_HOME`:
```bash
export ANDROID_HOME=~/Library/Android/sdk  # macOS
```

### "Notarization failed"

- Check that your app-specific password is valid (regenerate at https://appleid.apple.com)
- Ensure the bundle is properly signed with a Developer ID certificate (not just a development cert)
- Check the notarization log: `xcrun notarytool log <UUID>`

### "Gradle build failed"

- Run `pulp doctor` to check Android SDK/NDK/Java versions
- Ensure `android/` project exists (`pulp create --targets android`)
- Check Gradle output in the terminal for specific errors

### Released CLI tarball crashes on user machines

Symptom: `dyld: Library not loaded: @rpath/libwgpu_native.dylib` or
`error while loading shared libraries: libwgpu_native.so` after the
user extracts `pulp-<platform>.tar.gz` from a GitHub Release.

Root cause: `release-cli.yml` historically uploaded the bare
`build/tools/cli/pulp` binary, which carries an `LC_RPATH` /
`DT_RUNPATH` pointing at the build runner's home directory (e.g.
`/Users/runner/Library/Caches/Pulp/...` on macOS, the
GitHub-hosted-runner workspace on Linux). That path doesn't exist on
user machines.

Fix (active since v0.20.x): `tools/scripts/package_cli.py` is invoked
by `release-cli.yml` to:
1. Copy `libwgpu_native.{dylib,so,dll}` next to the binary.
2. macOS: `install_name_tool -delete_rpath` every absolute LC_RPATH
   and add `@loader_path`.
3. Linux: `patchelf --set-rpath '$ORIGIN'`.
4. Windows: no rewrite needed — DLLs resolve from the binary's
   directory automatically.

The portable-binary smoke gate in `release-cli.yml` runs the produced
artifact on a *clean* runner that did not build it, catching the bug
class before tagging. If you change rpath logic, run the smoke job
locally first or it will fail in CI for everyone else.

### Shipyard pin drift between local tooling and release workflows

Pulp's release automation depends on the pinned Shipyard CLI in two places:

- `tools/shipyard.toml` is the source-of-truth pin for local installs and
  `shipyard pr`.
- `.github/workflows/release-cli.yml` and `.github/workflows/post-tag-sync.yml`
  carry a `SHIPYARD_VERSION` env used by the release-side workflows.

If you bump the Shipyard pin, update both workflows in the same PR and keep the
existing string format in each file (`v0.26.0` vs `0.26.0`). Otherwise local
shipping and tag-time release jobs quietly diverge onto different Shipyard
versions, which is how release-only behavior changes get missed.

## Doctor Checks

`pulp doctor` validates Android toolchain:
- Android SDK location
- NDK version (r26+ required for C++20)
- Java version (17+ required for AGP 8+)
- Build-tools availability (apksigner, zipalign)
