# Web Plugin Support: WAMv2 and WebCLAP

What a Pulp `Processor` can become in the browser, what is verified today, and
the exact toolchains involved. This page is the honest capability reference; the
architecture and workflow live in [../guides/web-plugins.md](../guides/web-plugins.md).

A consistency lint (`tools/scripts/check_web_plugin_docs.py`) verifies that the
helpers, files, and targets named here actually exist in the tree, so this page
cannot drift into claiming a capability the repo does not have.

## Two web lanes, two toolchains (by design)

| Lane | Toolchain | What it produces | Status |
| --- | --- | --- | --- |
| **WAMv2** (Web Audio Modules v2) | Emscripten (`emcc`) | An ES-module + AudioWorklet plugin | **partial** — working canary |
| **WebCLAP** (`.wclap`) | wasi-sdk (`wasm32-wasi-threads`) | A CLAP compiled to WebAssembly | **experimental** — building canary (not browser-hosted yet) |

The two tracks deliberately use different toolchains: WAMv2 *wants* Emscripten's
AudioWorklet/JS glue, while WebCLAP wants wasi-sdk (Clang + WASI sysroot, no JS
glue). WASM is required for both — a Pulp `Processor` is C++ DSP, so it must
compile to WebAssembly to run in a browser.

## WAMv2 — supported today

Build a `Processor` to a WAMv2 plugin with `pulp_add_wam_plugin`
(`tools/cmake/PulpWam.cmake`); see [cmake.md](cmake.md#pulp_add_wam_plugin). The
shared `wam_*` C ABI lives in `core/format/src/wasm/wam_entry.cpp`; the runtime
JS (AudioWorklet processor, main-thread `WebAudioModule`, heap bridge) lives in
`core/format/src/wasm/` (`wam-processor.js`, `wam-plugin.js`, `wam-runtime.mjs`).

**Verified `Processor` features** (effect PulpGain, rich effect PulpChorus,
instrument PulpPluck):

| Feature | Verified by |
| --- | --- |
| Audio process (effect), finite, non-silent | Node runner + browser fixture |
| dB-accurate gain, interleaved stereo, bypass | native bridge test + Node runner |
| Generated parameter controls in a browser | OfflineAudioContext browser fixture |
| MIDI note-on → sound, decay after note-off (instrument) | Node feature runner |
| MIDI delivery to the processor | native bridge test |
| Parameter read-back + state save/restore | native bridge test + Node runner |

**Not yet in scope for the WAMv2 canary:**

- Full WAM-host (`WamEnv` / `WamGroup`) conformance.
- More than stereo, or more than one instance per AudioWorkletNode.
- Broader MIDI than note/CC (pitch bend, aftertouch, program change, sysex,
  MPE).
- A CI-enforced browser lane — the browser proof is the reproducible fixture at
  `examples/web-demos/wasm-build/browser-test/`, not yet a Playwright CI job.

## WebCLAP — building canary (not browser-hosted yet)

A `Processor` now compiles to a **live WebCLAP module**. Build one with
`pulp_add_wclap` (`tools/cmake/PulpWclap.cmake`) configured with the wasi-sdk
toolchain (`tools/cmake/wasi-toolchain.cmake`); the entry point uses
`PULP_WCLAP_PLUGIN` from `core/format/include/pulp/format/web/wclap_adapter.hpp`.
The checked-in example is `examples/web-demos/wclap-build/` (PulpGain →
`PulpGain.wasm`).

The output `.wasm` exports the WebCLAP host contract: the standard CLAP
`clap_entry` global, `malloc`/`free`/`cabi_realloc`, a growable function table,
and a shared, host-imported memory. `core/format/src/wasm/wclap_probe.mjs`
instantiates the module, runs its reactor initializer, and verifies `clap_entry`
resolves to a real CLAP 1.2.2 entry struct with callable init / deinit /
get_factory.

**Why the toolchain is what it is** (these are hard constraints, not choices):

- The **threaded** wasi target (`wasm32-wasi-threads`) is required — the state
  store transitively needs `std::thread`.
- Its libc++abi ships **without an exception runtime**, so the WebCLAP source
  set builds with `-fno-exceptions`. The portable `PULP_TRY` / `PULP_CATCH_ALL`
  macros in `core/runtime/include/pulp/runtime/exceptions.hpp` keep the one
  defensive try/catch compiling in both modes, and the JSON/filesystem-bound
  native PresetManager is replaced by the headless
  `core/state/src/wasm/preset_manager_wasm.cpp` (a browser sandbox has no preset
  filesystem).

**Not yet in scope for the WebCLAP canary:**

- **Browser/DAW hosting.** The module is contract-correct and proven live, but
  nothing yet drives it through a real WebCLAP host (AudioWorklet host JS, or a
  native Wasmtime bridge). No audio has been rendered through a CLAP host — that
  is the next step.
- A `.wclap` bundle layout, a host-side UI, and a CI lane that runs the probe
  (wasi-sdk is not yet installed on CI runners).

## Pinned upstream references

These are the upstream contracts the lanes target. Pinned by branch/date for
drift control (verify before relying on a specific revision):

- WAM API v2 branch: <https://github.com/webaudiomodules/api/tree/v2>
- WAM SDK: <https://github.com/webaudiomodules/sdk>
- WebCLAP org: <https://github.com/WebCLAP>
- WebCLAP host JS: <https://github.com/WebCLAP/wclap-host-js>
- CLAP draft webview (`clap.webview/3`): <https://github.com/free-audio/clap/blob/main/include/clap/ext/draft/webview.h>
- wasi-sdk: <https://github.com/WebAssembly/wasi-sdk>

References pinned: 2026-06-29.
