# Deploying a Pulp AUv3 Plug-in on iOS

End-to-end recipe for shipping a Pulp AUv3 plug-in from `cmake --build`
to the iOS Simulator and to a physical iPad with GarageBand iOS as the
real-world host. Pairs with the `ios` skill
(`.agents/skills/ios/SKILL.md`) for the per-subsystem gotchas, and with
`tools/cmake/PulpIosHostApp.cmake` for the CMake plumbing this guide
calls into.

> **Status (2026-05-26):** Phase iOS-B scaffolding (this recipe + the
> `pulp_add_ios_host_app` CMake helper). Phase iOS-C is "scaffolded,
> awaiting user device run" — the steps below are the recipe; the
> matrix in [Validation Checklist](#garageband-ios-validation-checklist)
> tracks which acceptance criteria a real-device session must confirm.

## Prerequisites

- **macOS** with Xcode 15+ (or 26.x) and the iOS / iOS Simulator SDKs.
- A Pulp project that already declares its AUv3 extension via
  `pulp_add_ios_auv3(...)`. The shipped example
  `examples/ios-auv3-synth/` is the reference.
- For physical-iPad deployment: Apple Developer Program membership +
  device paired in Xcode → Window → Devices and Simulators.

Pulp ships secrets via `~/.config/pulp/secrets/notary.env`. The relevant
keys for iOS:

```bash
PULP_SIGN_IDENTITY_SHA="..."       # signing-identity SHA-1
PULP_TEAM_ID="95CX6P84C4"          # Apple Developer Team ID
```

Source the env when you need codesign + provisioning info:

```bash
source ~/.config/pulp/secrets/notary.env
```

## CMake recipe

`pulp_add_ios_host_app(...)` wraps the `.appex` from
`pulp_add_ios_auv3(...)` into a SwiftUI HostApp `.app` bundle.

```cmake
# examples/ios-auv3-synth/CMakeLists.txt
pulp_add_ios_auv3(
    NAME            PulpSineSynth
    BUNDLE_ID       com.pulp.examples.sinesynth.host.PulpSineSynth
    MANUFACTURER    Pulp
    MANUFACTURER_CODE  Pulp
    SUBTYPE_CODE    PsSn
    AU_TYPE         aumu
    VERSION         0.1.0
    SOURCES         src/sine_synth.cpp src/au_v3_entry.cpp
)

pulp_add_ios_host_app(PulpSineSynth_HostApp
    AUV3_EXTENSION    PulpSineSynth_AUv3        # must match the .appex target above
    BUNDLE_ID         com.pulp.examples.sinesynth.host
    NAME              "PulpSineSynth"
    VERSION           0.1.0
    DEPLOYMENT_TARGET 16.4
)
```

The helper:

- Creates `${target}` as a SwiftUI iOS `.app` bundle from
  `templates/ios-auv3/HostApp/PulpHostApp.swift` +
  `ContentView.swift` (override with `SOURCES ...` for a custom UI).
- Generates `Info.plist` whose `AudioComponents` entry mirrors the
  AUv3 extension's manufacturer / subtype / type / version — drift
  between HostApp and extension descriptors silently breaks
  `AVAudioUnitComponentManager` filters.
- Embeds the built `.appex` into `${target}.app/PlugIns/` via a
  CMake sentinel target so a `.appex` rebuild forces a re-embed
  even when the HostApp itself didn't relink.

Configure + build:

```bash
cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=OFF \
  -DPULP_BUILD_TESTS=OFF

cmake --build build-ios-sim \
  --target PulpSineSynth_HostApp \
  --config Release -- -sdk iphonesimulator
```

The HostApp `.app` lands in `build-ios-sim/AUv3/Release-iphonesimulator/PulpSineSynth.app`
(or the equivalent for your build configuration).

## Deploy: iOS Simulator

```bash
# Pick a Simulator. iPad Pro 13-inch is the layout-validation default;
# iPhone 17 Pro is fastest on Apple Silicon for repeat builds.
xcrun simctl list devices available | grep -E "iPad|iPhone"

# Boot the chosen device.
xcrun simctl boot "iPad Pro 13-inch (M5)"   # or its UDID

# Install + launch the HostApp.
xcrun simctl install booted \
  build-ios-sim/AUv3/Release-iphonesimulator/PulpSineSynth.app
xcrun simctl launch  --console booted com.pulp.examples.sinesynth.host

# Confirm PluginKit registered the .appex inside the HostApp.
xcrun simctl spawn booted pluginkit -mAvvv -p com.apple.AudioUnit-UI \
  | grep -i pulpsinesynth
```

The HostApp's `ContentView` calls `AVAudioUnit.instantiate(with:options:)`
on launch. A list of parameter sliders appearing means the AUv3 loaded
end-to-end. Tap **Play** to start the AVAudioEngine and verify audio.

## Deploy: physical iPad

### 1. Pair the iPad in Xcode

1. Plug the iPad in via USB (or pair wirelessly).
2. Open Xcode → Window → Devices and Simulators → Devices tab.
3. Trust the computer on the iPad (Settings → Privacy → "Trust This Computer?").
4. Wait for Xcode to finish "Preparing iPad" (downloads symbols on first pair).

### 2. Configure CMake with signing identity

```bash
source ~/.config/pulp/secrets/notary.env

cmake -S . -B build-ios-device -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
  -DPULP_ENABLE_GPU=OFF \
  -DPULP_BUILD_TESTS=OFF \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY="Apple Development" \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${PULP_TEAM_ID}" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE="Automatic"

cmake --build build-ios-device \
  --target PulpSineSynth_HostApp \
  --config Release -- -sdk iphoneos
```

> **Provisioning profile gotcha**: Automatic signing requires the
> bundle identifier (`com.pulp.examples.sinesynth.host` and its
> `.appex` sibling `com.pulp.examples.sinesynth.host.PulpSineSynth.appex`) to already exist
> in [Apple Developer → Identifiers](https://developer.apple.com/account/resources/identifiers/list).
> If Xcode reports "No matching profiles found", open the generated
> `Pulp.xcodeproj` once in Xcode and let it register the IDs.

### 3. Install on device

Two paths — pick whichever is more convenient.

**A. `xcrun devicectl` (Xcode 15+, no Xcode UI):**

```bash
# List paired devices.
xcrun devicectl list devices

# Install the HostApp .app. <DEVICE_ID> is the long UDID from the list above.
xcrun devicectl device install app \
  --device <DEVICE_ID> \
  build-ios-device/AUv3/Release-iphoneos/PulpSineSynth.app

# Launch it.
xcrun devicectl device process launch \
  --device <DEVICE_ID> \
  com.pulp.examples.sinesynth.host
```

**B. Open in Xcode → Run on device:**

```bash
open build-ios-device/Pulp.xcodeproj
```

Select the `PulpSineSynth_HostApp` scheme + the paired iPad as the
destination, hit ⌘R. Xcode handles install + launch + log streaming.

### 4. Launch the HostApp once on device

iOS only registers an AUv3 extension with `AVAudioUnitComponentManager`
after the containing app launches at least once. Tap the HostApp's icon
on the iPad home screen, accept any first-launch microphone prompt
(see `Info.plist`'s `NSMicrophoneUsageDescription`), then close it. The
extension is now visible to GarageBand iOS and any other AUv3 host.

## GarageBand iOS validation checklist

The user runs this on their paired iPad. Tracks Phase iOS-C acceptance.

| # | Step | Expected | Pass / Notes |
|---|------|----------|--------------|
| 1 | Open GarageBand iOS → New Project → Audio Recorder → Tap the keyboard icon → Audio Unit Extensions | "Pulp" appears as a manufacturer | |
| 2 | Tap **Pulp → PulpSineSynth** | The plug-in's editor opens. (Default AU view — PulpSineSynth doesn't override `create_view()`.) | |
| 3 | Tap a note on the on-screen keyboard | Sine tone plays | |
| 4 | Drag the editor's parameter slider (if visible) | Parameter updates audibly in real time | |
| 5 | Rotate iPad portrait ↔ landscape | Editor reflows / does not crash | |
| 6 | Use Split View / Slide Over (iPad multitasking) | Editor reflows / does not crash | |
| 7 | Background then foreground GarageBand | Plug-in resumes without crash | |

If steps 4–7 reveal issues, capture the symptom + reproducer + iPad
console log (`Console.app` → connect device) and file under the
Phase iOS-C tracker.

### Optional: AUM

If AUM is installed, repeat steps 2–7 with AUM as the host (Instrument
slot → Pulp → PulpSineSynth) to validate the third-party-host routing
that GarageBand abstracts away.

## Troubleshooting

- **`xcrun simctl install` succeeds but the HostApp doesn't appear in Springboard.** The Simulator caches PluginKit registrations aggressively; try `xcrun simctl shutdown booted && xcrun simctl boot booted` and re-install.
- **AVAudioEngine throws `kAudioUnitErr_FailedInitialization` on first play.** Confirm `NSMicrophoneUsageDescription` is present in the HostApp's `Info.plist` (the template includes it; custom HostApps must preserve it).
- **GarageBand doesn't see the plug-in after install.** Force-quit GarageBand and re-launch — `AVAudioUnitComponentManager` only re-scans on app launch on iOS. If still missing, delete the HostApp + re-install to force a PluginKit re-register.
- **`-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM` not honoured.** Inherited team is set on every target by the Xcode generator only when the property is set before `add_executable`. The shipped `pulp_add_ios_host_app(...)` honours this; custom HostApp targets must wire the same XCODE_ATTRIBUTE.

## Cross-references

- `tools/cmake/PulpIosHostApp.cmake` — implementation of
  `pulp_add_ios_host_app(...)`.
- `tools/cmake/PulpAuv3.cmake` — `_pulp_add_auv3_ios(...)`; stashes the
  AudioComponentDescription on the `.appex` target as properties this
  helper reads back.
- `templates/ios-auv3/HostApp/` — SwiftUI HostApp template + Info.plist
  template + entitlements template.
- `.agents/skills/ios/SKILL.md` — iOS subsystem gotchas (touch dispatch,
  Skia gating, `dealloc` order, etc.).
- `planning/2026-05-24-auv3-ios-validation.md` — Phase iOS-A / B / C / D
  status tracker.
