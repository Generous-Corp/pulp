# Shipping Guide

End-to-end workflow for signing, packaging, and distributing Pulp plugins on macOS.

## Overview

The shipping pipeline follows this order:

```
Build → Validate → Sign → Notarize → Package → Distribute
```

Never install a plugin to system folders without passing validation first.

## Prerequisites

- Apple Developer ID certificate (Developer ID Application)
- Apple ID with app-specific password for notarization
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

## Step 4: Notarize

Notarization is handled via the `pulp::ship` API:

```cpp
#include <pulp/ship/codesign.hpp>

// Submit for notarization
auto uuid = pulp::ship::notarize_submit(
    "build/CLAP/MyPlugin.clap",
    "your@apple.id",
    "TEAMID",
    "xxxx-xxxx-xxxx-xxxx"  // app-specific password
);

// Check status (poll until complete)
auto status = pulp::ship::notarize_check(*uuid);

// Staple the ticket
pulp::ship::notarize_staple("build/CLAP/MyPlugin.clap");
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
pulp::ship::create_combined_pkg(
    {"build/VST3/MyPlugin.vst3", "build/CLAP/MyPlugin.clap", "build/AU/MyPlugin.component"},
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

pulp::ship::AppcastItem item;
item.version      = "1.0.1";
item.download_url = "https://example.com/MyPlugin-1.0.1.dmg";
item.description  = "Bug fixes and performance improvements.";

// Ed25519 signing: API present but implementation is planned (#295).
// sign_file_ed25519 returns std::nullopt today, and the CLI refuses
// to emit `edSignature=""` into an appcast rather than produce a
// silently-unsigned feed. Until the real impl lands, leave
// item.ed_signature unset (unsigned appcast) or sign out-of-band.
if (auto sig = pulp::ship::sign_file_ed25519(local_file_path, private_key_b64)) {
    item.ed_signature = *sig;
}

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
5. Generates appcast.xml (Ed25519 signatures planned — #295)
6. Creates GitHub Release with artifacts

## Plugin Install Locations (macOS)

| Format | System (all users) | User |
|--------|-------------------|------|
| VST3 | `/Library/Audio/Plug-Ins/VST3/` | `~/Library/Audio/Plug-Ins/VST3/` |
| AU | — | `~/Library/Audio/Plug-Ins/Components/` |
| CLAP | `/Library/Audio/Plug-Ins/CLAP/` | `~/Library/Audio/Plug-Ins/CLAP/` |
| Standalone | `/Applications/` | `~/Applications/` |
