# Pulp Web Demos

Pulp audio plugins compiled to WebAssembly and running in the browser.

## Quick Start

```bash
# 1. Build the WASM plugins (requires Emscripten)
cd examples/web-demos/wasm-build
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 2. Copy outputs to browser host
cp build/Pulp*.{js,wasm} ../../tools/browser-host/plugins/

# 3. Serve and open
cd ../../tools/browser-host
python3 -m http.server 8080
# Open http://localhost:8080
```

## What's Here

| Plugin | Type | Description | Parameters |
|--------|------|-------------|------------|
| **PulpGain** | Effect | Stereo gain with bypass | Input Gain, Output Gain, Bypass |
| **PulpPluck** | Instrument | Karplus-Strong plucked string | Decay, Brightness, Volume |
| **PulpChorus** | Effect | Stereo chorus with LFO | Rate, Depth, Mix |

## Prerequisites

### Emscripten (recommended)

```bash
# macOS
brew install emscripten

# Or manual install
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source emsdk_env.sh
```

### WASI SDK (experimental, for project-specific WebCLAP builds)

```bash
# Download from https://github.com/WebAssembly/wasi-sdk/releases
export WASI_SDK_PREFIX=/opt/wasi-sdk

# Build a project that includes tools/cmake/PulpWclap.cmake and calls
# pulp_add_wclap(...). The checked-in web demos currently ship WAMv2
# Emscripten targets, not a ready-made WebCLAP target.
cmake -S ../.. -B build-wclap \
  -DCMAKE_TOOLCHAIN_FILE=../../tools/cmake/wasi-toolchain.cmake \
  -DPULP_BUILD_WCLAP=ON
```

## How It Works

Each plugin is compiled to WebAssembly using Emscripten. The WASM module exports C functions that the browser host calls from an `AudioWorkletProcessor`:

```
Browser (JS)                    WASM Module (C++)
─────────────                   ─────────────────
AudioWorkletProcessor  ──────►  wam_init(sampleRate, blockSize)
  .process()           ──────►  wam_process(input, output, ch, frames)
  parameterChange      ──────►  wam_set_param(id, value)
  MIDI message         ──────►  wam_midi(status, d1, d2, offset)
  getDescriptor        ──────►  wam_descriptor() → JSON
```

The C++ side uses `WamProcessorBridge` which wraps a standard Pulp `Processor`. PulpPluck reuses the native example processor source that runs as VST3, AU v2, or CLAP on the desktop; PulpGain and PulpChorus use the same adapter shape for their web-demo processors.

## File Structure

```
web-demos/
├── pulp_chorus.hpp         # PulpChorus Processor (stereo chorus)
├── web_demos.cpp           # Registry include
├── CMakeLists.txt          # Native build (for testing)
├── README.md               # This file
└── wasm-build/
    ├── CMakeLists.txt      # Emscripten WASM build
    ├── pulp_gain_wasm.cpp  # PulpGain WASM entry point
    ├── pulp_pluck_wasm.cpp # PulpPluck WASM entry point
    └── pulp_chorus_wasm.cpp # PulpChorus WASM entry point

../pulp-pluck/
└── pulp_pluck.hpp          # Shared PulpPluck processor source
```

## Running Tests

The web demo plugins have native tests (no Emscripten needed):

```bash
# From repo root
cmake --build build --target pulp-test-web-demos
./build/test/pulp-test-web-demos
# → 8 test cases, 269 assertions
```

## Cross-Platform Story

The same Pulp Processor runs as:

| Format | Platform | Build Tool |
|--------|----------|-----------|
| VST3 | macOS, Windows, Linux | CMake (native) |
| AU v2 | macOS | CMake (native) |
| CLAP | macOS, Windows, Linux | CMake (native) |
| **WAMv2** | **Browser** | **Emscripten** |
| **WebCLAP** | **Native + Browser** | **WASI SDK helper; no checked-in demo target yet** |

## Acknowledgments

- [WebCLAP](https://github.com/WebCLAP) — portable audio plugins with WebAssembly
- [WAMv2](https://www.webaudiomodules.com) — Web Audio Modules standard
- [signalsmith-clap-cpp](https://github.com/geraintluff/signalsmith-clap-cpp) — reference WCLAP build pipeline
- [CLAP](https://github.com/free-audio/clap) — the plugin API standard
- [Emscripten](https://emscripten.org) — C++ to WebAssembly compiler
