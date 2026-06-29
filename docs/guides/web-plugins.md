# Web Plugin Formats: WAMv2 and WebCLAP

Pulp's web-plugin work is split across two complementary web standards. This
guide explains the intended architecture, the checked-in build paths, and the
current browser-host limits.

## Browser Host Availability

The Pulp Browser Host is currently a local tool in `tools/browser-host/`. The
repo does not yet publish a canonical repo-owned Pages deployment for it, so
browser-host examples should be treated as local-run instructions for now.

**WAMv2** now loads end-to-end: the `PulpGain` canary built with
`pulp_add_wam_plugin` loads into a browser `AudioWorkletNode`, renders audio,
and exposes generated parameter controls in Chrome. This is proven by a
headless, deterministic `OfflineAudioContext` fixture
(`examples/web-demos/wasm-build/browser-test/`) and a Node runner — it is not
yet wired into a CI lane, and it targets a stereo, single-instance canary rather
than full WAM-host (`WamEnv`/`WamGroup`) conformance. **WebCLAP** builds a Pulp
Processor to a CLAP-in-WebAssembly module (the checked-in PulpGain WebCLAP) that
a pure-JS host drives through the full CLAP lifecycle in Node — rendering audio
and controlling parameters — but is not yet hosted in a browser.

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
- `core/format/src/wasm/wam-plugin.js` — experimental WAMv2 JS runtime scaffold

**Upstream references:**
- [WAMv2 API](https://github.com/webaudiomodules/api) — interface definitions
- [WAMv2 SDK](https://github.com/webaudiomodules/sdk) — base classes and utilities
- [WAMv2 Examples](https://github.com/webaudiomodules/wam-examples) — reference plugins
- [NPM packages](https://www.npmjs.com/org/webaudiomodules) — `@webaudiomodules/api`, `@webaudiomodules/sdk`

### WebCLAP — Portable Audio Plugins with WebAssembly

[WebCLAP](https://github.com/WebCLAP) compiles CLAP plugins to WebAssembly, producing a single cross-platform binary (a `.wclap`) that runs in native DAWs (via the wclap-bridge) and in browsers (via wclap-host-js).

Since Pulp already has a CLAP adapter, WebCLAP support compiles a CLAP-style
Pulp processor to `wasm32-wasi-threads`:

```
Pulp Processor → CLAP adapter → WASI SDK → module.wasm (exports clap_entry)
                                              ↓
                  Native DAWs ← wclap-bridge (Wasmtime) ← .wclap bundle
                  Browsers    ← wclap-host-js (AudioWorklet) ← .wclap bundle
                  Node        ← wclap-host.mjs (pure-JS CLAP host) ← module.wasm
```

**Status:** the checked-in PulpGain WebCLAP (`examples/web-demos/wclap-build/`)
builds with wasi-sdk and is **hosted in Node** through the full CLAP lifecycle —
`wclap_probe.mjs` proves the module is live, and `wclap_host_runner.mjs` drives
create → init → activate → process, rendering audio and controlling parameters
(`Input Gain +6 dB` raises output exactly +6 dB). Host callbacks are synthesized
from JS via `WebAssembly.Function`, so no compiled C++ host shim is needed.
**Not yet** in-browser-hosted (AudioWorklet), and there is no `.wclap` bundle
layout or CI lane yet.

**When to use WebCLAP:** You want a single CLAP-in-WebAssembly binary for WCLAP
hosts (wclap-bridge, wclap-host-js) or for Node hosting, and are prepared to
wire it into a project-specific WASI build.

**Key files:**
- `core/format/include/pulp/format/web/wclap_adapter.hpp` — `PULP_WCLAP_PLUGIN()` macro
- `tools/cmake/PulpWclap.cmake` — `pulp_add_wclap()` build function
- `tools/cmake/wasi-toolchain.cmake` — WASI SDK CMake toolchain
- `core/format/src/wasm/wclap-host.mjs` — reusable pure-JS WebCLAP host
- `core/format/src/wasm/wclap_host_runner.mjs` — Node host harness (audio + params)
- `core/format/src/wasm/wclap_probe.mjs` — module-contract probe

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
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DPULP_WAM_CHOC_INCLUDE=<dir containing choc/>   # if not auto-located
cmake --build build
```

WAM plugins are declared with `pulp_add_wam_plugin` (`tools/cmake/PulpWam.cmake`)
— each plugin is a one-line factory; the shared `wam_*` C ABI lives in
`core/format/src/wasm/wam_entry.cpp`. A target emits a `.js` + `.wasm` pair (or a
BASE64-embedded ES-module factory with `SINGLE_FILE`, required for the
AudioWorklet). See `reference/cmake.md#pulp_add_wam_plugin`.

Validate without a browser using the deterministic Node runner
(`examples/web-demos/wasm-build/wam_node_runner.mjs`), and in a browser with the
`OfflineAudioContext` fixture under `examples/web-demos/wasm-build/browser-test/`
(see its README). The runtime JS — the AudioWorklet processor, the main-thread
`WebAudioModule`, and the shared heap bridge — lives in
`core/format/src/wasm/` (`wam-processor.js`, `wam-plugin.js`, `wam-runtime.mjs`).

`tools/cmake/PulpWasm.cmake` remains a separate app/standalone WASM helper; it is
not the WAM plugin path.

### Option 2: WebCLAP (WASI SDK)

```bash
# Install WASI SDK (https://github.com/WebAssembly/wasi-sdk/releases)
export WASI_SDK_PREFIX=/opt/wasi-sdk

# Configure a project that includes tools/cmake/PulpWclap.cmake and calls
# pulp_add_wclap(...). The root Pulp tree does not currently define a checked-in
# PulpGain_WCLAP target, and there is no root PULP_BUILD_WCLAP toggle.
cmake -S . -B build-wclap \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake \
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

The Pulp Browser Host (`tools/browser-host/`) is a self-contained HTML shell for
the web-plugin runtime work. Today it can:

- Accept WAM/WASM/WCLAP URLs or files and classify them by extension
- Serve as the local static host for checked-in Emscripten outputs
- Play audio files through the Web Audio analyser path
- Show the shell UI, MIDI keyboard, and oscilloscope

The host does not yet instantiate the checked-in WAM demo outputs into plugin
`AudioWorkletNode`s, build parameter controls from those outputs, or unpack
WCLAP bundles with `wclap-host-js`.

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
| WAMv2 | Emscripten | WASM | ❌ | Experimental generated-output lane; Pulp browser-host runtime wiring not validated | HTML/CSS/JS |

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
