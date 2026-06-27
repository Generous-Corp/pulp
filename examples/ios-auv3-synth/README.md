# iOS AUv3 Example — Pulp Sine Synth

Minimal AUv3 instrument for iOS + iPadOS. A single sine oscillator with a
frequency and level parameter plus a gate opened by MIDI note-on events.
The point of the example is end-to-end packaging (App Extension + host
container) rather than a musically useful synth.

**Tracks:** docs/guides/ios-auv3-guidance.md

## Build (iOS Simulator)

```bash
cmake -S . -B build-ios-sim -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4

xcodebuild -project build-ios-sim/Pulp.xcodeproj \
  -target PulpSineSynth -configuration Debug \
  -sdk iphonesimulator -arch arm64 \
  IPHONEOS_DEPLOYMENT_TARGET=16.4 build
```

## Validate

Run in AUM, GarageBand, or Cubasis on an iPad. For the simulator, launch
the host app and confirm it loads its own extension.

## Structure

```
examples/ios-auv3-synth/
├── CMakeLists.txt          # pulp_add_ios_auv3() wiring
├── README.md               # this file
└── src/
    ├── sine_synth.cpp
    └── sine_synth.hpp
```

The Info.plist and entitlements come from `templates/ios-auv3/`.

## Runtime Permissions

Plugins that add mic input, Bluetooth MIDI, or notifications should use the
cross-platform `pulp::platform::Permissions` API — identical source on iOS,
Android, and desktop:

```cpp
#include <pulp/platform/permissions.hpp>

using namespace pulp::platform;

request(Permission::Microphone, [](PermissionState s) {
    // iOS delivers this on the main thread; safe to touch UIKit.
    if (s == PermissionState::Granted) {
        start_recording();
    }
});
```

On iOS the backend maps onto `AVAudioSession.requestRecordPermission`,
`AVCaptureDevice.requestAccessForMediaType:`, `CBManager.authorization`,
and `UNUserNotificationCenter.requestAuthorizationWithOptions:`. Tests can
redirect the whole surface with `PermissionsOverride` — no real prompt.

## Known follow-ups

- `pulp_add_ios_auv3()` builds the `.appex` today; host-app target
  generation is follow-on. Copy `templates/ios-auv3/HostApp/`
  (SwiftUI `ContentView.swift` + `PulpHostApp.swift`) into your Xcode
  project as the App Store container.
- No presets yet — the `state::PresetManager` hook is wired but the
  example doesn't ship presets.
- Metal GPU path is still gated by `PULP_HAS_SKIA` on iOS; the render-loop
  and on-device validation work are tracked separately.
