# Windows Platform Guide

Windows is Pulp's second supported platform. This guide covers building, audio/MIDI setup, signing, and deployment.

## Requirements

- Windows 10 version 1903+ (x64 or ARM64)
- Visual Studio 2022 (17.x) with C++ desktop workload
- Git for Windows (the supported wrapper uses its bundled Git Bash)
- CMake 3.24+
- C++20 compiler (MSVC v143+)

## First-Time Bootstrap

Use the PowerShell wrapper, not raw `./setup.sh`, as the supported Windows entrypoint:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```

`setup.ps1` imports the Visual Studio C++ environment, resolves Git Bash from Git for Windows, and temporarily maps the checkout to a short drive alias so nested dependency bootstrap does not trip Windows path-length limits.

## External SDKs

```powershell
# Bootstrap pinned shared dependencies and SDKs
powershell -ExecutionPolicy Bypass -File .\setup.ps1 --deps-only

# VST3 SDK (MIT) is then provided under external/vst3sdk from Pulp's shared SDK cache.
# CLAP (MIT) is fetched automatically via CMake FetchContent.
# AudioUnit SDK is not used on Windows.
```

## Building

### Command Line (Developer PowerShell / Developer Command Prompt)

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1 --deps-only
cmake -S . -B C:\pulp-build -G "Visual Studio 17 2022" -A x64
cmake --build C:\pulp-build --config Release
ctest --test-dir C:\pulp-build --output-on-failure -C Release
```

On Windows on ARM, use `-A ARM64` instead. If you have both Visual Studio Community and Build Tools installed, you can force the full IDE instance with `-DCMAKE_GENERATOR_INSTANCE="C:/Program Files/Microsoft Visual Studio/2022/Community"`.
If you bypass `setup.ps1`, keep your checkout/build paths short. Nested FetchContent paths under `_deps/` can otherwise exceed the classic 260-character limit during MSBuild configure steps.

### Visual Studio IDE

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
# Open build/pulp.sln in Visual Studio
```

### Ninja (faster builds)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Audio I/O (WASAPI)

Pulp uses WASAPI (Windows Audio Session API) for low-latency audio on Windows. The implementation defaults to shared mode with event-driven buffering, and also supports exclusive mode and a shared low-latency path.

### How It Works

- **Shared mode** (default): audio is mixed with other applications (no exclusive device lock)
- **Exclusive mode**: `DeviceConfig::share_mode = ShareMode::exclusive` takes sole ownership of the endpoint and drives it at the device's minimum period for the lowest latency. If the endpoint disallows exclusive access or the mix format is unsupported there, `open()` honest-fails so the caller can fall back to shared.
- **Shared low-latency**: `DeviceConfig::low_latency = true` (shared mode only) uses `IAudioClient3::InitializeSharedAudioStream` at the engine's minimum period. If `IAudioClient3` is unavailable or the call fails, Pulp degrades to the standard shared `Initialize` at the requested buffer size — never a half-open stream.
- **Event-driven**: a background thread waits on buffer events, minimizing latency
- **Float32 non-interleaved**: Pulp's callback receives per-channel buffers; interleaving to/from WASAPI's format is handled internally
- **Input capture**: opening a capture endpoint delivers non-empty input buffers and an empty output buffer. Full-duplex apps open separate render and capture devices and synchronize them at the host layer.
- **Thread priority**: the render thread runs at `THREAD_PRIORITY_TIME_CRITICAL`
- **Device-invalidation recovery**: if the endpoint is removed or its sample rate / mix format changes (`AUDCLNT_E_DEVICE_INVALIDATED`), the I/O thread stops the stream cleanly and fires the `AudioSystem` device-change notification so the host can re-open at the new format. Pulp does not attempt a transparent in-place reopen.

### Device Enumeration

```cpp
auto system = pulp::audio::create_audio_system();
auto devices = system->enumerate_devices();
for (const auto& dev : devices) {
    // dev.name, dev.id, dev.max_output_channels, dev.sample_rates
}
```

### Standalone Apps

The standalone host (`pulp::format::StandaloneHost`) automatically uses WASAPI on Windows. No additional configuration needed — it uses the default output device at the system's native sample rate.

## MIDI I/O (Win32 MIDI)

Pulp uses the Win32 MIDI API (`midiIn*`/`midiOut*`) for MIDI on Windows.

### Device Enumeration

```cpp
auto system = pulp::midi::create_midi_system();
auto inputs = system->enumerate_inputs();
auto outputs = system->enumerate_outputs();
```

### Callback-Based Input

MIDI input uses the Win32 `CALLBACK_FUNCTION` mechanism. Messages are dispatched to Pulp's `MidiInputCallback` with status byte, data bytes, and millisecond timestamps.

## GPU Rendering (D3D12)

Dawn (Pulp's WebGPU implementation) supports D3D12 natively on Windows. When GPU rendering is enabled (`PULP_ENABLE_GPU=ON`), the Skia Graphite backend renders via D3D12 through Dawn.

No additional setup is needed — Dawn selects the D3D12 backend automatically on Windows. On Windows on ARM, the pinned WebGPU dependency also has an `aarch64` prebuilt runtime, so normal GPU-enabled smoke validation can stay on the standard dependency path.

## Plugin Formats

### VST3

VST3 plugins are built as `.vst3` bundles. Default install locations:

- **System-wide**: `C:\Program Files\Common Files\VST3\`
- **Per-user**: `%LOCALAPPDATA%\Programs\Common\VST3\`

### CLAP

CLAP plugins are built as `.clap` files. Default install locations:

- **System-wide**: `C:\Program Files\Common Files\CLAP\`
- **Per-user**: `%LOCALAPPDATA%\Programs\Common\CLAP\`

### No AU on Windows

Audio Units are macOS/iOS only. Windows builds produce VST3 and CLAP formats.

## Code Signing (Authenticode)

### Using signtool

Windows code signing uses `signtool.exe` from the Windows SDK:

```bash
# Sign with a certificate by name
signtool sign /n "Your Publisher Name" /t http://timestamp.digicert.com /fd sha256 MyPlugin.vst3

# Verify signing
signtool verify /pa MyPlugin.vst3
```

### Azure Trusted Signing

For cloud-based EV certificates (no hardware token required):

```bash
# Install Azure SignTool
dotnet tool install --global AzureSignTool

# Sign with Azure Trusted Signing
AzureSignTool sign -kvu "https://your-vault.vault.azure.net" \
  -kvc "your-cert-name" \
  -tr http://timestamp.digicert.com -td sha256 \
  MyPlugin.vst3
```

### Via Pulp CLI

```bash
pulp ship sign --identity "Your Publisher Name"
pulp ship check  # verify signing status
```

## Windows Installer (NSIS)

Pulp can generate NSIS installers for distributing plugins on Windows.

### Requirements

Install NSIS: https://nsis.sourceforge.io/ — `makensis` must be on PATH.

### Creating an Installer

```bash
pulp ship package --version 1.0.0
```

The installer:
- Installs VST3 to `%COMMONFILES%\VST3\`
- Installs CLAP to `%COMMONFILES%\CLAP\`
- Creates an uninstaller; plugin-only installers do not create Start Menu shortcuts
- Registers with Windows Add/Remove Programs

### Per-User Installation

For installs that don't require administrator privileges:

```bash
pulp ship package --version 1.0.0 --per-user
```

This installs to `%LOCALAPPDATA%\Programs\Common\` instead of `%COMMONFILES%\`.

## CI/CD (GitHub Actions)

The build workflow includes a Windows matrix entry:

```yaml
- os: windows-latest
  name: Windows (x64)
```

Windows CI:
- Clones VST3 SDK
- Configures with CMake (MSVC)
- Builds in Release mode
- Runs cross-platform unit tests
- Uploads VST3 and CLAP artifacts

## Troubleshooting

### WASAPI: "could not activate audio client"

The audio device may be in use exclusively by another application, or the device may have been removed. Check Windows Sound settings to verify the default output device is available.

### MIDI devices not appearing

Ensure MIDI devices are connected before enumerating. The Win32 MIDI API does not support hot-plugging — restart the application after connecting new MIDI hardware.

### Build fails with MSVC errors

Ensure you're using Visual Studio 2022 (v143 toolset). Older MSVC versions may not fully support C++20 features used by Pulp.

### Plugin not found by DAW

Verify the plugin is installed to the correct system directory. Most DAWs scan `C:\Program Files\Common Files\VST3\` — check your DAW's plugin scan path settings.
