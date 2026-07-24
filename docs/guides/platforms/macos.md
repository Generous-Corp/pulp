# macOS Platform Guide

macOS is Pulp's primary development platform. This guide covers signing, notarization, entitlements, auval validation, and deployment specifics.

## Requirements

- macOS 13.4+ on Apple Silicon (ARM64) or Intel (x86_64). Apple Silicon is the primary development platform; Intel is supported for the CLI and SDK via native `darwin-x64` release builds (see [Architectures](#architectures)). The floor is 13.4 (not 13.0) because the macOS 15.4 SDK's libc++ gates the floating-point `std::to_chars` overloads reached via `std::format` in Pulp's logging at 13.4; it is arch-independent and pinned by `tools/cmake/PulpMinOs.cmake`.
- Xcode 15+ command-line tools
- CMake 3.24+
- C++20 compiler (Apple Clang 15+)

## External SDKs

```bash
# VST3 SDK (MIT) — cloned at configure time
git clone --depth 1 --recursive --branch v3.8.0_build_66 https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
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
```

`pulp ship check` verifies built plugin signatures after signing; it does not list
available certificates.

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

Pulp supports both **Apple Silicon (ARM64)** and **Intel (x86_64)** on macOS.
Apple Silicon is the primary platform; Intel is a supported, natively-built
second architecture.

The release pipeline publishes per-architecture binaries for the CLI and SDK:

| Architecture | CLI | SDK |
|--------------|-----|-----|
| Apple Silicon | `pulp-darwin-arm64.tar.gz` | `pulp-sdk-darwin-arm64.tar.gz` |
| Intel | `pulp-darwin-x64.tar.gz` | `pulp-sdk-darwin-x64.tar.gz` |

`tools/install/install.sh` detects the host architecture (`uname -m`) and
installs the matching artifact automatically. The Intel builds are compiled
**natively** on a GitHub-hosted `macos-15-intel` runner (not cross-compiled),
so wgpu-native and the GPU render path use genuine x86_64 binaries, and the
release smoke lane runs each artifact natively. Both target macOS 13.4+
(the arch-independent floor pinned by `tools/cmake/PulpMinOs.cmake`).

Plugins you build with Pulp can target Intel or a universal binary from source
via `-DCMAKE_OSX_ARCHITECTURES=x86_64` or `"arm64;x86_64"` — the Skia, Dawn,
and wgpu-native dependencies all publish x86_64 and universal slices.

The Intel CLI/SDK leg is native-built and native-smoked, but it is **advisory**
in the release pipeline: because the `macos-15-intel` runner is occasionally
flaky, a failed Intel leg does not block the arm64/linux/windows release — that
release simply ships without the Intel slice, and the installer falls back to a
source build on Intel. So most releases carry the Intel artifacts, but a given
release may not. The broader Intel plugin/GPU story is **experimental** because
two things can't be exercised in CI: Metal on a real Intel/AMD discrete GPU, and
plugin-in-DAW hosting on real Intel hardware. Pulp verifies Intel portability on
a tiered cadence rather than on every PR — see
[macOS Intel (x86_64) support](intel-support.md) for the full tiering and the
honest catch/miss list.

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
