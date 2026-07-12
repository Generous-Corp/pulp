# Pulp Web Demos

Pulp audio plugins compiled to WebAssembly and running in a browser, in both web
plugin ABIs:

- **WAMv2** (Emscripten) — a headless DSP module driven by an `AudioWorkletProcessor`.
- **WebCLAP** (wasi-sdk) — a real CLAP plugin compiled to wasm, hosted by a
  worklet-resident CLAP host. Needs cross-origin isolation (COOP/COEP), because
  the module imports a shared `WebAssembly.Memory`.

Both are built and validated in CI (`.github/workflows/web-plugins.yml`): Node
runners drive the DSP through each ABI, and headless-Chrome fixtures prove the
end-to-end AudioWorklet path in a real browser.

## Prerequisites

Nothing in this repo bootstraps either toolchain — install them yourself.

### Emscripten (WAMv2)

```bash
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install 6.0.2 && ./emsdk activate 6.0.2
source ~/emsdk/emsdk_env.sh    # every shell — `emcmake` / `emcc` are not on PATH otherwise
```

CI pins **6.0.2**. The pin is deliberate: emsdk floats, and `latest` has already
changed both the WebGPU API and how `SINGLE_FILE` embeds the wasm. Build with a
version at or above the pin.

### wasi-sdk 25 (WebCLAP)

```bash
# macOS arm64 (Linux + Intel assets are in the same release)
curl -fsSL -o /tmp/wasi-sdk.tar.gz \
  https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-arm64-macos.tar.gz
mkdir -p ~/wasi-sdk && tar -xzf /tmp/wasi-sdk.tar.gz -C ~/wasi-sdk --strip-components=1
export WASI_SDK_PREFIX=~/wasi-sdk       # or install to /opt/wasi-sdk, the default
```

`tools/cmake/wasi-toolchain.cmake` hard-fails without it.

## Build

### WAMv2

```bash
cd examples/web-demos/wasm-build
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPULP_WAM_CHOC_INCLUDE=<dir containing choc/>   # only if not auto-located
cmake --build build
```

Each plugin emits `<Name>.js` + `<Name>.wasm` (Node runners, export inspection).
The `SINGLE_FILE` variants (`PulpGainWorklet`, `SuperConvolverWorklet`) embed the
wasm in the `.js` and compile it synchronously — mandatory inside an
`AudioWorkletGlobalScope`, which has no `fetch` and no async compile.

### WebCLAP

```bash
cd examples/web-demos/wclap-build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=../../../tools/cmake/wasi-toolchain.cmake
cmake --build build
```

Emits `<Name>.wasm` plus the distributable `<Name>.wclap/` bundle and
`<Name>.wclap.tar.gz`. `PulpGain` and `SuperConvolver` always build (their
sources are in this repo); the 23-plugin demo gallery additionally needs the two
external plugin-source checkouts (see the `PULP_EXAMPLE_PLUGINS_DIR` /
`PULP_CLASSIC_EFFECTS_DIR` cache entries) and is skipped without them.

## What's Here

| Plugin | Type | ABIs | Description |
|--------|------|------|-------------|
| **PulpGain** | Effect | WAM, WebCLAP | Stereo gain with bypass |
| **PulpPluck** | Instrument | WAM | Karplus-Strong plucked string |
| **PulpChorus** | Effect | WAM | Stereo chorus with LFO |
| **PulpPluckGainRack** | Rack | WAM | Two processors chained inside one worklet module |
| **SuperConvolver** | Effect | WAM, WebCLAP | Convolution reverb (`examples/super-convolver`) |

`SuperConvolver` is the one demo whose desktop build carries a GPU engine, a
file-backed impulse-response loader, and a native GPU editor. The web build
compiles all three out (`PULP_WASM` / `PULP_HEADLESS`) and runs the CPU
`PartitionedConvolver` against the built-in synthetic IR.

Be precise about what that does and does not mean:

* The **convolution engine** in the browser is the same code the desktop runs by
  default — the CPU `PartitionedConvolver`. It is not a simplified or rewritten
  browser DSP, and the Mix / Size / Gain / Bypass parameters behave identically.
* But the browser module genuinely **is missing capability** the desktop has: no
  GPU engine (so no `Engine` / `Rooms` / `Flow` — those parameters are not even
  declared on the web lanes), and no file-backed IR loading (no `.wav` off disk;
  the built-in synthetic IRs are the only sources). Those are compiled out, not
  hidden.

So: same DSP where the two overlap, a smaller feature surface overall.

## Validate

```bash
# WAMv2 (from wasm-build/)
node wam_feature_runner.mjs build/PulpGain.wasm effect
node wam_feature_runner.mjs build/PulpPluck.wasm instrument
node wam_rack_runner.mjs    build/PulpPluckGainRack.wasm
node superconvolver_runner.mjs build/SuperConvolver.wasm

# WebCLAP (from the repo root)
node core/format/src/wasm/wclap_probe.mjs       examples/web-demos/wclap-build/build/PulpGain.wasm
node core/format/src/wasm/wclap_host_runner.mjs examples/web-demos/wclap-build/build/PulpGain.wasm --gain-db 6
node examples/web-demos/wasm-build/superconvolver_runner.mjs \
     examples/web-demos/wclap-build/build/SuperConvolver.wasm
```

`superconvolver_runner.mjs` detects the ABI from the module's exports
(`clap_entry` ⇒ WebCLAP) and runs the SAME assertions either way — wet sound from
the built-in IR with zero file I/O, Size and Mix audibly changing the output, the
fixed 512-sample (`kInternalBlock`) PDC latency, and the state round-trip — so a
behavioural divergence between the two ABIs fails CI instead of surfacing as a
"the WebCLAP demo sounds different" bug.

Native tests for the demo processors need no wasm toolchain at all:

```bash
cmake --build build --target pulp-test-web-demos && ./build/test/pulp-test-web-demos
```

## How It Works

### WAMv2

```
Browser (JS)                    WASM Module (C++)
─────────────                   ─────────────────
AudioWorkletProcessor  ──────►  wam_init(sampleRate, blockSize)
  .process()           ──────►  wam_process(input, output, ch, frames)
  parameterChange      ──────►  wam_set_param(id, value)
  MIDI message         ──────►  wam_midi(status, d1, d2, offset)
  getDescriptor        ──────►  wam_descriptor() → JSON
```

`WamProcessorBridge` (`core/format/src/wasm/wam_adapter.cpp`) wraps a standard
Pulp `Processor`; `wam-plugin.js` + `wam-processor.js` are the main-thread host
and the worklet processor. A *rack* swaps the entry point for
`wam_chain_entry.cpp` and runs N processors in one module through the identical
ABI.

### WebCLAP

The plugin is a real CLAP: `clap_entry` plus the allocator exports a WebCLAP host
sandbox calls. `core/format/src/wasm/wclap-host.mjs` is the host — it synthesizes
the host vtable with trampolines (no `WebAssembly.Function`), so it runs in plain
Node as well as in a worklet.

Build logic for both lanes lives in `tools/cmake/PulpWam.cmake` and
`tools/cmake/PulpWclap.cmake`; the two `CMakeLists.txt` here only declare which
plugins to build.

## Cross-Platform Story

The same Pulp `Processor` runs as:

| Format | Platform | Build Tool |
|--------|----------|-----------|
| VST3 | macOS, Windows, Linux | CMake (native) |
| AU v2 | macOS | CMake (native) |
| CLAP | macOS, Windows, Linux | CMake (native) |
| **WAMv2** | **Browser** | **Emscripten (`pulp_add_wam_plugin`)** |
| **WebCLAP** | **Browser + native (Wasmtime bridge)** | **wasi-sdk (`pulp_add_wclap`)** |

## File Structure

```
web-demos/
├── pulp_chorus.hpp              # PulpChorus Processor
├── web_demos.cpp                # Registry include
├── CMakeLists.txt               # Native build (for the native tests)
├── wasm-build/                  # WAMv2 (Emscripten)
│   ├── CMakeLists.txt
│   ├── pulp_gain_wasm.cpp       # one small factory TU per plugin
│   ├── pulp_pluck_wasm.cpp
│   ├── pulp_chorus_wasm.cpp
│   ├── pluck_gain_rack_wasm.cpp
│   ├── super_convolver_wasm.cpp
│   ├── wam_feature_runner.mjs   # Node validation
│   ├── wam_rack_runner.mjs
│   ├── superconvolver_runner.mjs  # dual-ABI (WAM + WebCLAP)
│   └── browser-test/            # headless-Chrome fixture
└── wclap-build/                 # WebCLAP (wasi-sdk)
    ├── CMakeLists.txt
    ├── pulp_gain_wclap.cpp
    ├── plugins/                 # one entry TU per gallery plugin
    ├── browser-host/            # headless-Chrome fixture
    └── cloudflare/              # the deployed demo site (COOP/COEP headers)
```

## Acknowledgments

- [WebCLAP](https://github.com/WebCLAP) — portable audio plugins with WebAssembly
- [WAMv2](https://www.webaudiomodules.com) — Web Audio Modules standard
- [signalsmith-clap-cpp](https://github.com/geraintluff/signalsmith-clap-cpp) — reference WCLAP build pipeline
- [CLAP](https://github.com/free-audio/clap) — the plugin API standard
- [Emscripten](https://emscripten.org) — C++ to WebAssembly compiler
