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
| **WebCLAP** (`.wclap`) | wasi-sdk (`wasm32-wasi-threads`) | A CLAP compiled to WebAssembly | **experimental** — scaffold |

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

## WebCLAP — scaffold (not buildable yet)

`tools/cmake/PulpWclap.cmake` + `core/format/include/pulp/format/web/wclap_adapter.hpp`
are an experimental scaffold. There is no checked-in `.wclap` target yet. A WCLAP
build requires the **threaded** wasi target (`wasm32-wasi-threads`) because the
state store transitively needs `std::thread`, plus a portable event backend and
an exceptions decision. Status and the build plan: the planning submodule.

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
