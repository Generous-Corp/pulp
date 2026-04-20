# Android Synth Example

A JS-scripted subtractive synthesizer running on Android with GPU rendering, demonstrating:

- **Dawn/Vulkan** GPU rendering via Skia Graphite
- **QuickJS** scripted UI (the entire widget layout is defined in JavaScript)
- **Oboe** audio synthesis with lock-free parameter sync
- **30+ interactive widgets**: knobs, faders, toggles, XY pad, meter
- **AChoreographer** VSYNC render loop on a dedicated render thread

## Architecture

```
JavaScript (QuickJS)          C++ (Pulp core)
├── synth_ui.js              ├── gpu_surface_android.cpp
│   ├── createKnob()         │   ├── Dawn/Vulkan init
│   ├── createFader()        │   ├── Skia Graphite rendering
│   ├── createToggle()       │   ├── AChoreographer render loop
│   └── createXYPad()        │   └── Touch → View routing
│                            │
│   WidgetBridge ──────────→ ├── demo_synth.cpp
│   (JS → C++ widgets)      │   ├── Oboe audio stream
│                            │   ├── Saw/square oscillators
│                            │   ├── One-pole resonant filter
│                            │   └── ADSR envelope
│
Kotlin                       Shared Atomics
├── PulpActivity             ├── SynthParams
├── PulpSurfaceView          │   ├── osc_pitch, osc_detune...
│   ├── Render thread        │   ├── filter_cutoff, filter_reso...
│   │   └── Looper           │   └── env_attack, env_decay...
│   └── Touch dispatch       │
└── PulpAudioFocus           └── UI writes → Audio reads (relaxed)
```

## Key Files

| File | Purpose |
|------|---------|
| `synth_ui.js.h` | Embedded JS UI script (the entire synth layout) |
| `gpu_surface_android.cpp` | Dawn/Vulkan surface, render loop, touch routing, label drawing |
| `demo_synth.cpp` | Oboe audio: oscillators, filter, envelope, peak metering |
| `demo_synth.hpp` | Shared atomic parameters (lock-free UI → audio sync) |
| `PulpSurfaceView.kt` | SurfaceView with render thread, safe area insets |
| `PulpActivity.kt` | Activity lifecycle, audio focus management |

## The JS UI

The synth UI is defined in ~150 lines of JavaScript using the WidgetBridge API:

```javascript
// Create a knob row
const oscRow = createRow("osc-row", "root");
setFlex("osc-row", "justify_content", "space_evenly");

createKnob("pitch", "osc-row");
setFlex("pitch", "width", 48);
setValue("pitch", 0.5);

createFader("master", "horizontal", "root");
setValue("master", 0.8);

createToggle("osc1", "toggle-row");
setValue("osc1", 1.0);  // ON
```

QuickJS executes this on Android. If JS fails, the app falls back to an equivalent C++ widget hierarchy.

## Controls

| Control | What It Does |
|---------|-------------|
| **Pitch** knob | Oscillator frequency (C2 to C7) |
| **Detune** knob | Second oscillator detuning |
| **Mix** knob | Saw ↔ square waveform blend |
| **Level** knob | Oscillator output level |
| **OSC 1-4** toggles | Enable base pitch, detuned, octave up, sub octave |
| **XY Pad** | X = filter cutoff, Y = filter resonance |
| **Cutoff/Reso/Env** knobs | Filter parameters |
| **A/D/S/R** knobs | Amplitude envelope |
| **CH 1-4** faders | Channel mixer levels |
| **Master** fader | Output volume |
| **Meter** | Real-time peak level (green/yellow/red segments) |

## Building

```bash
# Prerequisites: Android NDK 30, Android SDK with API 36 emulator

# Cross-compile
export ANDROID_NDK=$HOME/Library/Android/sdk/ndk/30.0.14904198
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_NATIVE_API_LEVEL=26 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-android -j$(nproc)

# Build APK
cd android && ./gradlew assembleDebug

# Launch emulator (MUST use -gpu host for stable audio)
QEMU_AUDIO_DRV=coreaudio emulator -avd <avd> -gpu host -no-snapshot &

# Install and run
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.pulp.app/com.pulp.PulpActivity
```

## Emulator Notes

- **Always use `-gpu host`** (not `swiftshader_indirect`). SwiftShader is a CPU rasterizer that starves the audio HAL.
- **`QEMU_AUDIO_DRV=coreaudio`** is required on macOS for the audio pipe to work.
- Dawn shader compilation takes ~2s on device, ~15s on emulator. The render thread with Looper prevents ANR.

## Runtime Permissions

This synth is output-only so it doesn't need `RECORD_AUDIO`, but any plugin
that adds mic input or Bluetooth MIDI should use the cross-platform
`pulp::platform::Permissions` API (same code path on iOS, Android, and
desktop — see `examples/ios-auv3-synth` for the iOS side):

```cpp
#include <pulp/platform/permissions.hpp>

using namespace pulp::platform;

if (query(Permission::Microphone) != PermissionState::Granted) {
    request(Permission::Microphone, [](PermissionState s) {
        if (s == PermissionState::Granted) {
            // Safe to start Oboe input stream.
        }
    });
}
```

On Android the prompt UI is surfaced by the Kotlin host
(`ActivityResultContracts.RequestPermission`); the native callback fires
once the user taps Allow or Deny. Tests can short-circuit with
`PermissionsOverride` — no real prompt, no JNI round-trip.
