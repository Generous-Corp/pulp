# Linux Platform Guide

Linux is Pulp's third supported platform. This guide covers dependencies, building, audio/MIDI setup, packaging, and deployment.

## Requirements

- Ubuntu 22.04+ / Fedora 38+ / Arch (x86_64 or aarch64)
- GCC 12+ or Clang 15+ with C++20 support
- CMake 3.24+
- ALSA development libraries

## Dependencies

### Ubuntu / Debian

```bash
sudo apt-get install build-essential cmake git libasound2-dev
```

### Fedora / RHEL

```bash
sudo dnf install gcc-c++ cmake git alsa-lib-devel
```

### Arch

```bash
sudo pacman -S base-devel cmake git alsa-lib
```

## External SDKs

```bash
# VST3 SDK (MIT) — cloned at configure time
git clone --depth 1 --recursive --branch v3.7.12_build_20 https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
cd external/vst3sdk && git submodule update --init --recursive --depth 1

# CLAP (MIT) — fetched automatically via CMake FetchContent
# No AudioUnit SDK needed on Linux
```

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Ninja (faster builds)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Audio I/O (ALSA)

Pulp uses ALSA (Advanced Linux Sound Architecture) for audio on Linux. The implementation uses blocking `snd_pcm_writei` with a dedicated render thread.

### How It Works

- **Interleaved float32**: ALSA receives interleaved samples; Pulp deinterleaves for its callback
- **Double-buffered**: period size = requested buffer size, buffer = 2 periods
- **Blocking write**: the render thread blocks on `snd_pcm_writei`, which naturally paces output
- **Underrun recovery**: automatic recovery via `snd_pcm_recover` on EPIPE

### PulseAudio / PipeWire Compatibility

ALSA's "default" device routes through PulseAudio or PipeWire when available. No additional configuration is needed — Pulp's ALSA backend works transparently with both.

### Device Enumeration

```cpp
auto system = pulp::audio::create_audio_system();
auto devices = system->enumerate_devices();
// Includes "default" (PulseAudio/PipeWire) + hardware devices (hw:0, hw:1, ...)
```

### Low-Latency Setup

For professional audio with low latency:

1. Install JACK: `sudo apt-get install jackd2`
2. Configure real-time privileges: add your user to the `audio` group
3. Use a low-latency kernel: `sudo apt-get install linux-lowlatency`

## MIDI I/O (ALSA Raw MIDI)

Pulp uses ALSA's raw MIDI interface (`snd_rawmidi_*`) for MIDI on Linux.

### Device Enumeration

```cpp
auto system = pulp::midi::create_midi_system();
auto inputs = system->enumerate_inputs();   // Hardware MIDI inputs
auto outputs = system->enumerate_outputs();  // Hardware MIDI outputs
```

### Virtual MIDI Ports

ALSA supports virtual MIDI ports for inter-application MIDI routing. Open with port ID "virtual" to create a virtual port.

## GPU Rendering (Vulkan)

Dawn (Pulp's WebGPU implementation) supports Vulkan natively on Linux. When GPU rendering is enabled (`PULP_ENABLE_GPU=ON`), the Skia Graphite backend renders via Vulkan through Dawn.

### Vulkan Setup

```bash
# Ubuntu
sudo apt-get install libvulkan-dev vulkan-tools mesa-vulkan-drivers

# Verify
vulkaninfo --summary
```

Note: GPU rendering in QEMU/KVM virtual machines may be limited. If Vulkan is unavailable, Dawn will report an error at surface creation time. This does not affect audio processing.

## Plugin Formats

### VST3

VST3 plugins are built as `.so` shared libraries. Install locations:

- **System-wide**: `/usr/lib/vst3/` or `/usr/local/lib/vst3/`
- **Per-user**: `~/.vst3/`

### CLAP

CLAP plugins are built as `.so` files. Install locations:

- **System-wide**: `/usr/lib/clap/` or `/usr/local/lib/clap/`
- **Per-user**: `~/.clap/`

### LV2

LV2 plugins are built as `.lv2/` bundles when the LV2 headers are available
at configure time. Install locations:

- **System-wide**: `/usr/lib/lv2/`
- **Per-user**: `~/.lv2/`

### No AU on Linux

Audio Units are macOS/iOS only. Linux builds produce VST3, CLAP, and LV2
formats when their SDK/header dependencies are enabled.

## Packaging

### Linux Packages

After `pulp build`, `pulp ship package --version <version>` packages the built
VST3, CLAP, and LV2 bundles from `build/{VST3,CLAP,LV2}` as a `.deb` in
`artifacts/` using `dpkg-deb`. If `dpkg-deb` is not available, the command
falls back to a `.tar.gz` archive of the available plugin format directories.

If no VST3, CLAP, or LV2 bundles are present, the Linux package path fails
instead of creating an empty artifact summary.

### AppImage

Standalone Linux apps can be wrapped as AppImage bundles by passing the built
executable explicitly:

```bash
pulp ship package --version 1.0.0 --format appimage \
  --binary build/examples/myapp/MyApp_Standalone
```

The AppImage path requires `appimagetool` on `PATH`. It packages standalone
executables only; plugins ship through the `.deb` or `.tar.gz` package path.

## CI/CD (GitHub Actions)

The build workflow includes a Linux matrix entry:

```yaml
- os: ubuntu-latest
  name: Linux (x64)
```

Linux CI:
- Installs `libasound2-dev`
- Clones VST3 SDK
- Configures with CMake (GCC)
- Builds in Release mode
- Runs cross-platform unit tests
- Uploads VST3 and CLAP `.so` artifacts

## Troubleshooting

### "ALSA: could not open device"

Check that ALSA is working:
```bash
aplay -l   # List playback devices
speaker-test -t sine -f 440   # Test audio output
```

If using PipeWire, ensure the ALSA compatibility layer is installed:
```bash
sudo apt-get install pipewire-alsa
```

### No MIDI devices

Ensure MIDI hardware is connected and ALSA sees it:
```bash
amidi -l   # List raw MIDI devices
aconnect -l   # List MIDI connections
```

### Build fails: "ALSA not found"

Install the ALSA development package:
```bash
sudo apt-get install libasound2-dev   # Ubuntu/Debian
sudo dnf install alsa-lib-devel       # Fedora
```

### Vulkan not available in VM

GPU rendering requires real Vulkan hardware support. In QEMU/KVM VMs without GPU passthrough, Vulkan may not be available. This is expected — audio plugins work without GPU rendering.
