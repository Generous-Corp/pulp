# Android Platform Guide

Android is a supported Pulp platform target. This guide covers build
prerequisites, the in-repo `android/` reference app, and the end-to-end
smoke validation path for the generator-scaffolded app (issue #337).

For deep architecture and bringup gotchas see
[.agents/skills/android/SKILL.md](../../../.agents/skills/android/SKILL.md).

## Requirements

- Android Studio (for SDK / AVD management) or standalone `sdkmanager`
- Android NDK `30.0.14904198` (`sdkmanager "ndk;30.0.14904198"`)
- Java 17 (`brew install --cask temurin@17` on macOS)
- `adb` and `emulator` on PATH (or detected via `ANDROID_HOME`)
- For GPU-accelerated rendering: Skia Graphite + Dawn Android build
  (see "Skia-Android prebuilts" below)

Run `pulp doctor android` to verify each of these in one pass.

## Building

### Host build (same as desktop)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(sysctl -n hw.ncpu)
ctest --test-dir build --output-on-failure
```

### Cross-compile libpulp.so

```bash
export ANDROID_NDK=$HOME/Library/Android/sdk/ndk/30.0.14904198
cmake -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_NATIVE_API_LEVEL=26 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android -j$(sysctl -n hw.ncpu)
```

### Build the APK

```bash
cd android
./gradlew assembleDebug
# Output: android/app/build/outputs/apk/debug/app-debug.apk
```

## Skia-Android prebuilts

Skia Graphite + Dawn is only GPU-active on Android when the Android-ABI
static libraries live under `external/skia-build/android-gpu/` (arm64)
or `external/skia-build/android-gpu-x86_64/` (x86\_64 emulator).

The repo does not ship these — build them with:

```bash
tools/build-skia-android.sh arm64
# or: tools/build-skia-android.sh all    # arm64 + x86_64
```

Expect a 30-60 minute cold build. The script clones Skia, syncs
deps, and stages the output tree FindSkia.cmake consumes. Once built,
subsequent `./gradlew assembleDebug` runs pick them up automatically.

If Skia-Android is missing, the APK still builds and runs (JNI bridge,
audio, lifecycle, permissions all work), but `GpuSurface::create_dawn()`
returns `nullptr` and the app logs:

```
E Pulp : Android GPU surface: failed to create Dawn GpuSurface
```

Use the smoke script's `--allow-no-gpu` flag to proceed past this state
when validating the non-GPU path.

## Emulator

### Start an AVD

```bash
# List available AVDs
emulator -list-avds

# CRITICAL: `-gpu host` — `swiftshader_indirect` starves the audio HAL.
# CRITICAL: `QEMU_AUDIO_DRV=coreaudio` on macOS — default audio backend
#           has a broken pipe to Core Audio speakers.
QEMU_AUDIO_DRV=coreaudio emulator -avd Medium_Phone_API_36.1 -gpu host -no-snapshot &

# Wait for boot
adb wait-for-device
adb shell getprop sys.boot_completed  # returns "1" when ready
```

Pulp's in-repo helper `android/run-emulator.sh <AVD>` wraps the above.

Recommended AVD: `Medium_Phone_API_36.1` with the `arm64-v8a` system
image on Apple Silicon hosts — matches the arm64-only APK and avoids
the HVF-unsupported path (see `.agents/skills/android/SKILL.md` § "Known
Blockers").

## End-to-end smoke

`tools/scripts/android_smoke.sh` runs the #337 acceptance path
end-to-end against a local emulator or device:

1. **Build APK** — `./gradlew assembleDebug` (skippable via `--skip-build`)
2. **Install & launch** `com.pulp.app/com.pulp.PulpActivity`
3. **Wait** for `JNI_OnLoad: Pulp native bridge initialized`
4. **Wait** for render-ready (`Android GPU surface: ANativeWindow received`
   and, in strict mode, `Dawn initialized` + Skia context marker)
5. **Wait** for audio engine (`DemoSynth: playing`)
6. **Exercise lifecycle** — `KEYCODE_HOME` → wait for `nativeOnBackground`;
   `am start` → wait for `nativeOnForeground`
7. **Exercise permissions** — `pm revoke` + `pm grant` RECORD_AUDIO;
   verify via `dumpsys package` that the runtime permission state
   flipped. (Note: `pm revoke` on a permission held by a live process
   kills that process — lifecycle is exercised first.)
8. **Clean shutdown** — `am force-stop` + scan logcat for FATAL
   exceptions

### Prerequisites

- An emulator or physical device attached and fully booted
  (`adb shell getprop sys.boot_completed` returns `1`)
- The device ABI must match the APK (arm64-v8a by default; the in-repo
  Gradle config filters to arm64-v8a only)
- `ANDROID_HOME` or `ANDROID_SDK_ROOT` set, or one of the default paths
  populated: `~/Library/Android/sdk`, `~/Android/Sdk`

### Quick start

```bash
# Full path — builds APK, installs, runs smoke
tools/scripts/android_smoke.sh

# Faster — reuse existing APK
tools/scripts/android_smoke.sh --skip-build

# Non-GPU environments (Skia-Android prebuilts not present)
tools/scripts/android_smoke.sh --skip-build --allow-no-gpu

# Target a specific device when more than one is attached
tools/scripts/android_smoke.sh --device emulator-5554
```

Full run time on a warm emulator with an existing APK: ~5 seconds.
Cold cycle (APK build + smoke): ~3 minutes on M-series Macs.

### ctest integration

The smoke is wired as the `android-smoke` ctest test, disabled by
default. Opt in per-invocation:

```bash
PULP_ANDROID_SMOKE_ENABLED=1 ctest --test-dir build -R android-smoke --output-on-failure
```

Without `PULP_ANDROID_SMOKE_ENABLED=1` the test exits with code 77
(ctest SKIP) so `ctest` runs on non-Android hosts report the test as
skipped rather than failed.

### CI posture

The cloud emulator path in `.github/workflows/android.yml` is gated
behind `vars.PULP_ANDROID_EMULATOR_ENABLED` — `macos-latest` runners
are Apple Silicon, and QEMU+HVF can't host an arm64-v8a Android guest
(`HV_UNSUPPORTED`). Until a working runner config lands (Linux arm64 +
KVM, x86\_64 APK + x86\_64 emulator, or self-hosted nested-HVF), the
smoke is a local-run validation deliverable. See issue #487 for the
path-forward discussion.

## Deploy to a device

```bash
# Plug in a phone with USB debugging enabled
adb devices                                # confirm attached
tools/scripts/android_smoke.sh --device <serial>
```

For day-to-day iteration after a Kotlin-only change, the Google
Android CLI accelerator (see
[.agents/skills/android/SKILL.md](../../../.agents/skills/android/SKILL.md))
reduces the install+launch cycle to one command:

```bash
android run --apks=android/app/build/outputs/apk/debug/app-debug.apk
```

## Logcat debugging

```bash
# Pulp-only stream (both native and Kotlin log tags)
adb logcat -s Pulp:V PulpAudio:V PulpRender:V

# Capture a single activity lifetime
adb logcat -c && adb shell am start -n com.pulp.app/com.pulp.PulpActivity
```

See `.agents/skills/android/SKILL.md` § "Critical Gotchas" for the
full list of bringup pitfalls (platform detection order, Vulkan format,
Oboe stream mode, audio lifecycle, etc.).
