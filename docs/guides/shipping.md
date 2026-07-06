# Shipping Guide

End-to-end workflow for signing, packaging, and distributing Pulp plugins on macOS.

## Overview

The shipping pipeline follows this order:

```
Build → Validate → Sign → Notarize → Package → Distribute
```

Never install a plugin to system folders without passing validation first.

Shipping a plugin that hot-reloads its DSP/UI to end users? Sign the reload packs
too — see [reload-trust.md](reload-trust.md) for `pulp ship swap-pack`, the trust
model, and where signing keys live.

## Prerequisites

- Apple Developer ID certificate (Developer ID Application)
- Either an App Store Connect API key (`.p8`, preferred) or an Apple ID with
  app-specific password for notarization. Store ASC creds in
  `~/.config/pulp/secrets/notary.env` — see Step 4.
- Xcode command-line tools installed

## Step 1: Build

```bash
pulp build
# or
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Output appears in `build/VST3/`, `build/CLAP/`, `build/AU/`.

## Step 2: Validate

```bash
pulp validate
```

This runs:
- `clap-validator` on `.clap` bundles (if installed)
- `dlopen` test on `.clap` bundles (fallback)
- `auval` on `.component` bundles (macOS only, requires installation)
- `ctest` for format-specific test labels

## Step 3: Sign

```bash
pulp ship sign --identity "Developer ID Application: Your Name (TEAMID)"
```

Signs all plugin bundles in `build/` with:
- `--force` (re-signs if already signed)
- `--timestamp` (Apple timestamp server)
- `--options runtime` (hardened runtime, required for notarization)
- Default entitlements from `ship/templates/entitlements.plist`

Custom entitlements:

```bash
pulp ship sign --identity "..." --entitlements path/to/custom.plist
```

### Default Entitlements

The default entitlements enable:
- Audio input (microphone access)
- Network client (for update checks)

### Check Signing Status

```bash
pulp ship check
```

Runs `codesign --verify --deep --strict` on each bundle.

### Non-Interactive Keychain Access

If `codesign` can list the Developer ID identity but signing fails with
`errSecInternalComponent`, the private key is installed but the current process
is not allowed to use it. GUI Terminal sessions may prompt and succeed while
agents, CI jobs, launchd services, or other non-interactive shells fail.

The durable setup is a dedicated signing keychain for automation. Export the
Developer ID Application certificate and private key as a `.p12` from Keychain
Access, then run this once from zsh:

```bash
KC="$HOME/Library/Keychains/pulp-signing.keychain-db"

read -rs "KC_PW?New signing keychain password: "; echo
security create-keychain -p "$KC_PW" "$KC"
security set-keychain-settings -lut 21600 "$KC"
security unlock-keychain -p "$KC_PW" "$KC"
security list-keychains -d user -s "$KC" "$HOME/Library/Keychains/login.keychain-db"

read -rs "P12_PW?Developer ID .p12 password: "; echo
security import "/path/to/DeveloperIDApplication.p12" \
  -k "$KC" \
  -P "$P12_PW" \
  -T /usr/bin/codesign \
  -T /usr/bin/productsign \
  -T /usr/bin/pkgbuild \
  -T /usr/bin/productbuild

security set-key-partition-list -S apple-tool:,apple: -s -k "$KC_PW" "$KC"
unset KC_PW P12_PW
security find-identity -v -p codesigning "$KC"
```

Before a non-interactive signing run, unlock that keychain and make sure it is
in the user search list:

```bash
KC="$HOME/Library/Keychains/pulp-signing.keychain-db"
read -rs "KC_PW?Signing keychain password: "; echo
security unlock-keychain -p "$KC_PW" "$KC"
security list-keychains -d user -s "$KC" "$HOME/Library/Keychains/login.keychain-db"
unset KC_PW
```

For a personal machine that already has the Developer ID private key in the
login keychain, this narrower repair is usually enough:

```bash
KC="$HOME/Library/Keychains/login.keychain-db"
read -rs "LOGIN_PW?Mac login password: "; echo
security unlock-keychain -p "$LOGIN_PW" "$KC"
security set-key-partition-list -S apple-tool:,apple: -s -k "$LOGIN_PW" "$KC"
unset LOGIN_PW
```

## Step 4: Notarize Packaged Artifacts

Use this after `pulp ship package`, or use `pulp ship release` to run the
sign/package/notarize stages in one command. The preferred CLI path uses an App
Store Connect API key (`.p8`) and points at the packaged `.pkg`, `.dmg`, or
`.zip` artifact:

```bash
pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg \
                   --api-key ~/.config/pulp/secrets/AuthKey_XXX.p8 \
                   --api-key-id XXX \
                   --api-issuer 5e8f0b95-3e2f-48e7-b7c2-52e7c220502a
```

Stash the credentials once in `~/.config/pulp/secrets/notary.env` and the
CLI resolves them automatically:

```bash
# ~/.config/pulp/secrets/notary.env  (chmod 600)
PULP_NOTARY_KEY_PATH="$HOME/.config/pulp/secrets/AuthKey_XXX.p8"
PULP_NOTARY_KEY_ID="XXX"
PULP_NOTARY_ISSUER_ID="5e8f0b95-3e2f-48e7-b7c2-52e7c220502a"
```

Then:

```bash
pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg
pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg --dry-run
```

The legacy Apple-ID + app-specific-password lane still works:

```bash
pulp ship notarize --path artifacts/MyPlugin-1.0.0.pkg \
                   --apple-id you@example.com --team-id ABCDE12345
# password defaults to @keychain:AC_PASSWORD — store via
#   security add-generic-password -s AC_PASSWORD -a you@example.com -w
```

Programmatic API:

```cpp
#include <pulp/ship/codesign.hpp>

// Submit the distributable archive you will ship, such as the .pkg or .dmg
// produced by packaging. Do not submit a raw .clap/.vst3/.component directory.
const char* archive = "artifacts/MyPlugin-1.0.0.pkg";

// App Store Connect API key (preferred)
auto uuid = pulp::ship::notarize_submit_asc(
    archive,
    "/path/to/AuthKey_XXX.p8", "XXX", "5e8f0b95-...");

// Legacy Apple-ID flow
// auto uuid = pulp::ship::notarize_submit(
//     archive, "you@apple.id", "TEAMID", "@keychain:AC_PASSWORD");

auto status = pulp::ship::notarize_check_asc(
    *uuid, "/path/to/AuthKey_XXX.p8", "XXX", "5e8f0b95-...");
pulp::ship::notarize_staple(archive);
```

The CI workflow (`sign-and-release.yml`) automates this on tag pushes.

## Step 5: Package

### App Icons

Standalone targets and app targets can attach an icon source directly from
CMake:

```cmake
pulp_app_icon(MyPlugin_Standalone
    SOURCE assets/app-icon.png
)
```

If you never call `pulp_app_icon(...)`, the target builds with its default icon
behavior. Removing the call cleanly removes the custom-icon pipeline again.

Current behavior:
- macOS bundles a generated `AppIcon.icns`
- Windows links a generated `.ico`
- Android scaffolds generated launcher PNGs under `android/app/src/main/res-generated/`
- iOS records the selected source for downstream packaging and warns that
  asset-catalog emission is not implemented yet

The source image must be a PNG that is at least 1024×1024.

Per-platform overrides and debug/release variants are optional:

```cmake
pulp_app_icon(MyPlugin_Standalone
    SOURCE       assets/app-icon.png
    WINDOWS      assets/app-icon-win.png
    DEBUG_ICON   assets/app-icon-dev.png
    RELEASE_ICON assets/app-icon.png
)
```

### PKG Installer

```bash
pulp ship package --version 1.0.0
```

Creates `.pkg` installers in `artifacts/` for each plugin bundle, with correct install locations:
- VST3 → `/Library/Audio/Plug-Ins/VST3/`
- CLAP → `/Library/Audio/Plug-Ins/CLAP/`
- AU → `~/Library/Audio/Plug-Ins/Components/`

### DMG Disk Image

```cpp
pulp::ship::create_dmg("build/Standalone/MyPlugin.app",
                        "artifacts/MyPlugin-1.0.0.dmg",
                        "MyPlugin 1.0.0");
```

Creates a DMG with an Applications alias for drag-to-install.

### Combined Multi-Format Installer

```cpp
#include <pulp/ship/codesign.hpp>
#include <vector>

std::vector<pulp::ship::InstallComponent> components = {
    {"build/VST3/MyPlugin.vst3", "/Library/Audio/Plug-Ins/VST3", "VST3"},
    {"build/CLAP/MyPlugin.clap", "/Library/Audio/Plug-Ins/CLAP", "CLAP"},
    {"build/AU/MyPlugin.component",
     "/Library/Audio/Plug-Ins/Components", "Audio Unit (AU)"},
};

pulp::ship::create_combined_pkg(
    components,
    "artifacts/MyPlugin-1.0.0-All.pkg",
    "com.mycompany.myplugin",
    "1.0.0"
);
```

## Step 6: Distribute

### Appcast for Auto-Updates

Generate a Sparkle-compatible appcast:

```cpp
#include <pulp/ship/appcast.hpp>
#include <stdexcept>

pulp::ship::AppcastItem item;
item.version      = "1.0.1";
item.download_url = "https://example.com/MyPlugin-1.0.1.dmg";
item.description  = "Bug fixes and performance improvements.";

// Ed25519 signing accepts a Sparkle-style base64 private key: either
// a 32-byte seed or a 64-byte secret key.
auto sig = pulp::ship::sign_file_ed25519(local_file_path, private_key_b64);
if (!sig) {
    throw std::runtime_error("Ed25519 signing failed");
}
item.ed_signature = *sig;

pulp::ship::Appcast feed;
feed.items.push_back(item);
auto xml = feed.to_xml();
```

### CI Release Pipeline

The `sign-and-release.yml` workflow runs on version tags (`v*`):

1. Builds all formats (Release config)
2. Signs with Developer ID from GitHub Secrets
3. Notarizes via `notarytool`
4. Creates PKG installers
5. Generates appcast.xml, including Ed25519 signatures when a signing key is provided
6. Creates GitHub Release with artifacts

## Plugin Install Locations (macOS)

| Format | System (all users) | User |
|--------|-------------------|------|
| VST3 | `/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/VST3/` |
| AU | — | `~/Library/Audio/Plug-Ins/Components/` |
| CLAP | `/Library/Audio/Plug-Ins/CLAP/` | `~/Library/Audio/Plug-Ins/CLAP/` |
| Standalone | `/Applications/` | `~/Applications/` |
