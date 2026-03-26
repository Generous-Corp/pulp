# macOS Platform Guide

macOS is Pulp's primary development platform. This guide covers signing, notarization, entitlements, auval validation, and deployment specifics.

## Requirements

- macOS 13+ (ARM64 or x86_64)
- Xcode 15+ command-line tools
- CMake 3.24+
- C++20 compiler (Apple Clang 15+)

## External SDKs

```bash
# VST3 SDK (MIT) — cloned at configure time
git clone --depth 1 --branch v3.7.12 https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
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

## Universal Binaries

To build for both ARM64 and x86_64:

```bash
cmake -B build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build
```

## Sandbox Considerations

- AU v2 plugins run in the host's process (no sandbox)
- AUv3 plugins run in an app extension sandbox (future — not yet implemented)
- Standalone apps should request entitlements appropriate to their needs

## Debugging in DAWs

1. Build in Debug mode: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
2. Install the plugin to the appropriate folder
3. Attach Xcode debugger to the DAW process: Debug → Attach to Process
4. Set breakpoints in your Processor code
