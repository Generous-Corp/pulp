# Web Plugin Formats: WAMv2 and WebCLAP

Pulp has experimental browser-format scaffolding through two complementary web
standards. This guide explains the current build outputs, adapter pieces, and
local browser-host scaffold.

## Browser Host Availability

The Pulp Browser Host is currently a local tool in `tools/browser-host/`. The
repo does not yet publish a canonical repo-owned Pages deployment for it, so
browser-host examples should be treated as local-run instructions for now.
The host is not yet a runtime-validated WAMv2/WebCLAP demo for the checked-in
generated plug-in outputs.

## Two Paths to the Browser

### WAMv2 — Web Audio Modules v2

[WAMv2](https://www.webaudiomodules.com) is a standard for web audio plugins and DAWs. A WAM plugin is an ES module that creates an `AudioWorkletNode` for real-time processing.

Pulp's WAMv2 adapter wraps a Processor as a `WebAudioModule`:

```
Pulp Processor (C++ → WASM via Emscripten)
  ↕ WamProcessorBridge (de-interleave, param sync, MIDI)
  ↕ AudioWorkletProcessor (audio thread)
  ↕ WamNode / AudioWorkletNode (main thread)
  ↕ WebAudioModule (host-facing API)
```

**When to use WAMv2:** Your plugin runs in a browser-based DAW or web app that supports the WAM standard (e.g., [WebDAW](https://www.webaudiomodules.com/community), custom web apps).

**Key files:**
- `core/format/include/pulp/format/web/wam_adapter.hpp` — C++ bridge
- `core/format/src/wasm/wam_adapter.cpp` — implementation
- `core/format/src/wasm/wam-plugin.js` — WAMv2 JS runtime

**Upstream references:**
- [WAMv2 API](https://github.com/webaudiomodules/api) — interface definitions
- [WAMv2 SDK](https://github.com/webaudiomodules/sdk) — base classes and utilities
- [WAMv2 Examples](https://github.com/webaudiomodules/wam-examples) — reference plugins
- [NPM packages](https://www.npmjs.com/org/webaudiomodules) — `@webaudiomodules/api`, `@webaudiomodules/sdk`

### WebCLAP — Portable Audio Plugins with WebAssembly

[WebCLAP](https://github.com/WebCLAP) compiles CLAP plugins to WebAssembly, producing a single cross-platform binary (a `.wclap`) that runs in native DAWs (via the wclap-bridge) and in browsers (via wclap-host-js).

Since Pulp already has a CLAP adapter, WebCLAP support is shaped as an
experimental helper path for compiling a CLAP-style Pulp processor to
`wasm32-wasi`:

```
Pulp Processor → CLAP adapter → WASI SDK → module.wasm
                                              ↓
                  Native DAWs ← wclap-bridge (Wasmtime) ← .wclap bundle
                  Browsers    ← wclap-host-js (AudioWorklet) ← .wclap bundle
```

**When to use WebCLAP:** You are experimenting with WCLAP hosts such as
wclap-bridge or wclap-host-js and are prepared to wire the helper into a
project-specific WASI build. The repo ships adapter macros and a CMake helper;
it does not yet ship a checked-in, browser-validated `PulpGain_WCLAP` demo.

**Key files:**
- `core/format/include/pulp/format/web/wclap_adapter.hpp` — `PULP_WCLAP_PLUGIN()` macro
- `tools/cmake/PulpWclap.cmake` — `pulp_add_wclap()` build function
- `tools/cmake/wasi-toolchain.cmake` — WASI SDK CMake toolchain

**Upstream references:**
- [WebCLAP organization](https://github.com/WebCLAP) — all repos
- [wclap-bridge](https://github.com/WebCLAP/wclap-bridge) — native CLAP/VST3 host for WCLAPs
- [wclap-host-js](https://github.com/WebCLAP/wclap-host-js) — browser host library
- [browser-test-host](https://github.com/WebCLAP/browser-test-host) — reference browser host
- [signalsmith-clap-cpp](https://github.com/geraintluff/signalsmith-clap-cpp) — example WCLAP build pipeline

### CLAP Webview Extension

CLAP v1.2.7 introduced the [draft webview extension](https://github.com/free-audio/clap/blob/main/include/clap/ext/draft/webview.h), which is the primary way WCLAPs provide a GUI. The plugin provides HTML/JS content; the host provides the webview.

Pulp supports this through `WebviewProvider`:
- Plugins return HTML content or a URL via `get_webview_content()`
- The auto-generated webview UI creates parameter sliders from the plugin's parameter definitions
- Communication uses `postMessage` between the webview and the plugin

**Key file:** `core/format/include/pulp/format/web/clap_webview.hpp`

## Building for the Web

### Option 1: WAMv2 (Emscripten)

```bash
# Install Emscripten SDK
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source emsdk_env.sh

# Configure and build the checked-in web demo lane
cd examples/web-demos/wasm-build
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The output is a `.js` + `.wasm` pair for the checked-in demo plugins. These
outputs are useful for browser-integration work, but the repo does not yet
claim end-to-end browser-host audio validation for them. The root
`tools/cmake/PulpWasm.cmake` helper is available for projects that include it
explicitly, but the root Pulp build does not currently create WAM targets from
`-DPULP_WASM=ON` alone.

### Option 2: WebCLAP (WASI SDK)

```bash
# Install WASI SDK (https://github.com/WebAssembly/wasi-sdk/releases)
export WASI_SDK_PREFIX=/opt/wasi-sdk

# Configure a project that includes tools/cmake/PulpWclap.cmake and calls
# pulp_add_wclap(...). The root Pulp tree does not currently define a checked-in
# PulpGain_WCLAP target.
cmake -S . -B build-wclap \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake \
  -DPULP_BUILD_WCLAP=ON \
  -DCMAKE_BUILD_TYPE=Release

# Build the project-defined WCLAP target
cmake --build build-wclap --target <YourPlugin>_WCLAP

# Bundle as .wclap
cd build-wclap/<YourPlugin>.wclap/
tar --exclude=".*" -czf ../<YourPlugin>.wclap.tar.gz *
```

The `.wclap.tar.gz` is intended to be:
- Loaded in native DAWs via [wclap-bridge](https://github.com/WebCLAP/wclap-bridge) (appears as `[WCLAP] <YourPlugin>`)
- Loaded in browsers via [WebCLAP browser-test-host](https://github.com/WebCLAP/browser-test-host) or another host with `wclap-host-js`

### WCLAP Requirements

A valid WCLAP module must:

1. **Export `clap_entry`** — the standard CLAP entry point
2. **Export memory allocators** — `malloc`/`free` or `cabi_realloc` (for host sandbox allocation)
3. **Export a growable function table** — for host callback registration
4. **Target wasm32-wasi** — using WASI SDK or Emscripten with WASI compatibility

The `PULP_WCLAP_PLUGIN()` macro handles all of this automatically.

### Threading Considerations

- **WASI SDK (recommended):** Full `wasi-threads` support. Use `wasi-sdk-pthread.cmake` toolchain.
- **Emscripten:** Plugins can be called from multiple threads, but cannot spawn their own threads.
- **Browser hosts:** Require [cross-origin isolation](https://web.dev/coop-coep/) for SharedArrayBuffer (needed for threads).

## Pulp Browser Host

The Pulp Browser Host (`tools/browser-host/`) is a self-contained HTML scaffold
for browser integration work. It currently provides:

- Local file/URL controls and basic file-playback or microphone routing UI
- A WAMv2 ES module import path for modules exposing `default.createInstance(audioCtx)`
- A raw `.wasm` fetch/compile path that still needs the AudioWorklet bridge file
  and generated-output wiring
- Display auto-generated parameter controls
- Provide an on-screen MIDI keyboard for instruments
- Show a real-time oscilloscope
- Share plugin state via URL (base64-encoded)

The host currently recognizes WCLAP URLs and files, but the `.wclap.tar.gz`
unpack/instantiate path is still a placeholder until `wclap-host-js` is wired.
The checked-in WAM demo outputs are Emscripten module factories with `wam_*`
exports, so they should be treated as generated-output fixtures until the host
bridge is wired and browser-validated against them.

### Publishing Status

The current docs deployment workflow publishes the generated docs site, API
reference, and installer scripts. It does not yet publish `tools/browser-host/`
as part of the Pages artifact. Repo-owned browser-host publication should be
handled as a later docs-site integration phase instead of assuming a `gh-pages`
branch flow.

### Running Locally

```bash
# Serve with any static file server (needed for AudioWorklet CORS)
cd tools/browser-host
python3 -m http.server 8080
# Open http://localhost:8080
```

## Architecture Summary

| Format | Build Tool | Runtime | Native DAWs | Browsers | GUI |
|--------|-----------|---------|-------------|----------|-----|
| CLAP | CMake | Native | ✅ Direct | ❌ | clap.gui (NSView/HWND) |
| WebCLAP | WASI SDK | WASM | Experimental via wclap-bridge | Experimental via external wclap-host-js; Pulp browser-host loading not wired yet | clap.webview |
| WAMv2 | Emscripten | WASM | ❌ | Experimental generated-output lane; Pulp browser-host runtime validation pending | HTML/CSS/JS |

## What Runs Where

```
                    ┌─────────────┐
                    │ Pulp Plugin │
                    │ (Processor) │
                    └──────┬──────┘
                           │
           ┌───────────────┼───────────────┐
           │               │               │
     ┌─────┴─────┐  ┌─────┴─────┐  ┌─────┴─────┐
     │   CLAP    │  │  WebCLAP  │  │   WAMv2   │
     │  Adapter  │  │  (WASI)   │  │(Emscripten)│
     └─────┬─────┘  └─────┬─────┘  └─────┬─────┘
           │               │               │
     ┌─────┴─────┐  ┌─────┴─────┐  ┌─────┴─────┐
     │  Native   │  │  Native + │  │  Browser   │
     │   DAWs    │  │  Browser  │  │   DAWs     │
     └───────────┘  └───────────┘  └───────────┘
```
