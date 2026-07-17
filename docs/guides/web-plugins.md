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
- `core/format/src/wasm/wam_build_report.mjs` — web-build report generator

**Web-build report — what happens to a plugin's UI on the web:** `pulp_add_wam_plugin`
emits a `<Name>.web-build.json` next to the module (post-build) documenting the
UI strategy of the web build, so the "what happened to my editor / my imported
design" question is answered at build time rather than discovered in the browser:

- **Generated controls.** A headless WAM build has no view layer, so the web host
  renders generated controls. The report lists the **parameter binding targets**
  (`id`, `label`, range) the generated controls are built from. A design-import
  UI binds a knob/slider to a parameter by `id`, and the generated control for
  that `id` is the same target — so a binding to parameter `id` N routes to the
  generated control for parameter `id` N. The report makes that contract explicit
  and testable (`wam_build_report.test.mjs`).
- **Native-editor fallback.** Declare `NATIVE_EDITOR` on `pulp_add_wam_plugin`
  for a plugin whose native build has a `Processor::create_view()` editor; the
  report then records that the native editor cannot run in a headless web build
  and is replaced by generated controls. (A WAM build is headless, so this is a
  developer declaration — the build has no view to introspect.)

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
cmake --build build-wclap --target <YourPlugin>-wclap

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
4. **Target wasm32-wasi-threads** — using WASI SDK or Emscripten with WASI compatibility

The `PULP_WCLAP_PLUGIN()` macro handles all of this automatically.

### Threading Considerations

- **WASI SDK (recommended):** Full `wasi-threads` support. Use `wasi-toolchain.cmake` toolchain.
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

## Native Editors on the Web (custom UI modules)

A Pulp plugin's **actual native editor** — the same C++ view tree, drawn by Skia — can run in
the browser, compiled to WebAssembly and painted into a `<canvas>` by Skia Ganesh on WebGL2.
SuperConvolver ships this: its hero impulse-response field, log-frequency spectrum, source
chip, and Mix/Size/Gain sliders are the real editor, animating in real time on the web. No
re-implementation in HTML/JS — one editor source, native and web.

### Pay-for-what-you-use: three tiers

The shared player stays small; the heavy pieces load on demand, so a plugin with no custom
editor never downloads a byte of them.

| Tier | What | Loaded by |
|------|------|-----------|
| **0 — the player** | `@danielraffel/web-player`, the main-thread shell (overlay, adapters, scope/meter, state, theming). Small. | every demo page |
| **1 — the DSP module** | the plugin compiled to wasm (WAM or WebCLAP). | pages that mount that plugin |
| **2 — a custom UI module** | the plugin's own editor (Skia + view). ~8.5 MB, **~3.5 MB brotli on the wire** (Cloudflare compresses `application/wasm`). | **only** pages that declare a custom editor |

A page with no custom editor renders the player's generated parameter grid and ships zero Skia.
See [`docs/reference/web-plugin-support.md`](../reference/web-plugin-support.md) for the format
support matrix and [`docs/reference/layout-model.md`](../reference/layout-model.md) for the
flex+grid layout the editor uses.

### The contract: author the editor DECOUPLED from the DSP

The one rule that makes a native editor web-portable: **the editor talks to a small host
interface, never the `Processor` directly.** SuperConvolver's is `SuperConvolverUiHost` — four
calls (`gpu_status`, `ir_path`, `load_ir_path`, `impulse_response_snapshot`) plus the shared
`StateStore` and a data bus (its spectrum). The editor `#include`s the host header, never the
DSP header.

- **Natively**, the host IS the processor: it implements the interface (its four members
  already existed), and `create_view()` passes `*this`. Zero cost.
- **On the web**, the host is a browser shim (`SuperConvolverWebHost`) that answers the same
  four calls against the page — file dialogs, the DSP's `pulp_ir_*` exports over the worklet
  boundary, GPU status from the SharedArrayBuffer.

Because both worlds share ONE editor source, a change to the editor ships to native and web at
once. Authoring the editor decoupled from day one turns "port to web" into a 3-line
`processor& → host&` swap instead of a large reconciliation later.

### How a page mounts it

The `pulp-web-demo` generator (the `pulp-web-demo` skill) wires the mount: it forwards pointer
input, handles DPR/resize sizing, bridges the file dialog, and swaps the custom editor in over
the generated grid — falling back to the grid if the module fails to load or the device has no
WebGL2 (honest degradation, never a blank panel). The core requirement that makes a
continuously-animating editor work on the web (a view that calls `request_repaint()` during
paint) is `PULP_VIEW_HAS_RENDER_LOOP` plus the `render_frame()` re-entrancy guard in the
browser `WindowHost` — without them, paint re-enters paint and the JS stack overflows.

Full design, size budget, and the plan to generalize this for other plugin authors:
`planning/2026-07-15-web-editor-architecture-and-size.md` (private submodule).

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
