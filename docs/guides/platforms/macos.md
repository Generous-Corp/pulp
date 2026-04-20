# macOS Platform Guide

macOS is Pulp's primary development platform. This guide covers signing, notarization, entitlements, auval validation, and deployment specifics.

## Requirements

- macOS 13+ on Apple Silicon (ARM64). Intel x86_64 is **not supported** — Rosetta 2 translates x86_64 → ARM64 on Apple Silicon, not the reverse, so Intel Macs cannot run Pulp's ARM64 release binaries. Intel users must build from source via `./setup.sh`, and no lane of Pulp's CI / release pipeline / development targets that configuration
- Xcode 15+ command-line tools
- CMake 3.24+
- C++20 compiler (Apple Clang 15+)

## External SDKs

```bash
# VST3 SDK (MIT) — cloned at configure time
git clone --depth 1 --recursive --branch v3.7.12_build_20 https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
cd external/vst3sdk && git submodule update --init --recursive --depth 1

# AudioUnit SDK (Apache 2.0) — cloned at configure time
git clone --depth 1 https://github.com/apple/AudioUnitSDK.git external/AudioUnitSDK

# CLAP (MIT) — fetched automatically via CMake FetchContent
```

## Code Signing

### Developer ID Certificate

You need a "Developer ID Application" certificate from Apple. Check available identities:

```bash
security find-identity -v -p codesigning
# or
pulp ship check
```

### Signing Plugins

```bash
pulp ship sign --identity "Developer ID Application: Your Name (TEAMID)"
```

This signs all plugin bundles with hardened runtime and timestamp. The default entitlements (`ship/templates/entitlements.plist`) grant audio input and network client access.

### Entitlements

Audio plugins need specific entitlements for the hardened runtime:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "...">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
    <key>com.apple.security.network.client</key>
    <true/>
</dict>
</plist>
```

Without `device.audio-input`, standalone apps can't access the microphone. Without `network.client`, update checks fail.

## Notarization

Apple requires notarization for distribution outside the App Store.

```bash
# Via notarytool (Xcode 14+)
xcrun notarytool submit MyPlugin.dmg \
    --apple-id "your@apple.id" \
    --team-id "TEAMID" \
    --password "xxxx-xxxx-xxxx-xxxx" \
    --wait

# Staple the ticket
xcrun stapler staple MyPlugin.dmg
```

The CI workflow automates this on tag pushes.

## Audio Unit Validation (auval)

AU plugins must pass `auval` before installation:

```bash
# Install the plugin first
cp -r build/AU/MyPlugin.component ~/Library/Audio/Plug-Ins/Components/

# Run auval (effect)
auval -v aufx MyPl Mnfr

# Run auval (instrument)
auval -v aumu MyPl Mnfr

# Run auval (MIDI effect)
auval -v aumi MyPl Mnfr
```

`auval` verifies:
- Component loads and instantiates
- Parameters are enumerable and within range
- Audio renders without errors (NaN, Inf)
- State save/load round-trips correctly
- No memory leaks during instantiation/destruction

### auval Tips

- The 4-character codes come from your `PluginDescriptor` (PLUGIN_CODE and MANUFACTURER_CODE in CMake)
- AU components must be installed to `~/Library/Audio/Plug-Ins/Components/` before auval can find them
- If auval fails, check Console.app for crash logs

## Plugin Install Locations

| Format | Path | Notes |
|--------|------|-------|
| VST3 | `~/Library/Audio/Plug-Ins/VST3/` | Per-user, DAW-scannable |
| AU | `~/Library/Audio/Plug-Ins/Components/` | Per-user, auval requires this |
| CLAP | `~/Library/Audio/Plug-Ins/CLAP/` | Per-user |
| Standalone | `~/Applications/` or `/Applications/` | DMG drag-to-install |

## Architectures

Pulp officially supports **Apple Silicon (ARM64) only** on macOS. Released
binaries (`pulp-darwin-arm64.tar.gz`, `pulp-sdk-darwin-arm64.tar.gz`) target
ARM64. Intel Macs cannot run these binaries (Rosetta 2 translates x86_64 →
ARM64, not the reverse) and `tools/install/install.sh` declines to install
on x86_64 hosts without building from source.

Building a universal (arm64+x86_64) plugin from source is possible in
principle via `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`, but this is an
**unsupported source-build experiment** — Pulp's CI lanes, release pipeline,
and ongoing development assume Apple Silicon. Don't expect parity with the
ARM64 build.

## Sandbox Considerations

- **AU v2** plugins run in the host's process (no sandbox)
- **AU v3** plugins run in an app extension (`.appex`) and therefore in a
  sandbox — this is Apple's platform model for AUv3; there is no
  non-sandboxed AUv3 deployment form. AUv3 is shipped today via
  `pulp_add_plugin(... FORMATS AUv3 ...)` (see
  `docs/guides/ios-auv3-guidance.md` for the iOS path + entitlement story)
- **VST3** plugins run in the host's process (no sandbox)
- **CLAP** plugins run in the host's process (no sandbox)
- **Standalone apps** should request entitlements appropriate to their needs

## Debugging in DAWs

1. Build in Debug mode: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
2. Install the plugin to the appropriate folder
3. Attach Xcode debugger to the DAW process: Debug → Attach to Process
4. Set breakpoints in your Processor code
