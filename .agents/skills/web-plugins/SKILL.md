---
name: web-plugins
description: Pulp in the browser — the WAM v2 and WebCLAP adapters, the wasm runtime, and the Skia/WebGL2 browser window host. Covers what does and does not compile to wasm, the worklet-thread constraints (no std::thread, no fetch), the non-realtime tick, memory growth, and the silent-failure traps that make a wasm build "work" while producing no audio or no pixels.
---

# Pulp on the web (WAM v2 / WebCLAP / browser UI)

Pulp targets the browser through two audio ABIs and one UI host:

| Piece | Where | What it is |
|-------|-------|------------|
| WAM v2 adapter | `core/format/src/wasm/wam_adapter.cpp`, `wam-runtime.mjs` | The whole module lives inside one `AudioWorklet` |
| WebCLAP adapter | `core/format/src/wasm/` + `examples/web-demos/wclap-build/` | Real CLAP, wasm-hosted; needs COOP/COEP for threaded shared memory |
| Browser window host | `core/view/platform/web/` | `core/view`'s widget tree painted by **Skia Ganesh on WebGL2** |

The audio side and the UI side are **independent**. The browser UI module is
DSP-free and talks to audio only through the web player's `HostAdapter` seam, so
the *same* wasm UI module mounts against both a WAM and a WebCLAP demo. Build it
once; if it looks different across the two ABIs, that is a shared-player bug, not
a per-demo tweak.

## What is NOT in the browser

Be precise about this; it is easy to overclaim and the claims get quoted.

- **GPU audio is not in the browser.** No GPU-compute code compiles to wasm.
  The published Skia wasm slice is Ganesh on WebGL2, and **WebGL2 has no compute
  shaders**. A GPU-DSP lane would need WebGPU (emdawnwebgpu) in a dedicated
  worker; that is unstarted. The browser lane is a **GPU-rendered UI over CPU
  DSP** — say exactly that.
- **Not Graphite/Dawn.** See the `skia-gpu-build` skill's wasm section; the slice
  ships zero `wgpu` symbols.
- File-backed loaders and native editors are compiled out (`PULP_WASM` /
  `PULP_HEADLESS`). A plugin that needs an asset on the web must carry it in the
  binary or fetch it on the main thread — an `AudioWorkletGlobalScope` cannot
  fetch, which is also why the WAM worklet build must be `SINGLE_FILE` (the wasm
  embedded in the .js).

## The worklet has no second thread

A WAM module runs entirely inside the audio worklet: **there is no `std::thread`
and no control thread.** A processor whose control changes need work `process()`
must never do (decode, resample, FFT-plan, allocate) therefore has nowhere to do
it — which is what `Processor::on_non_realtime_tick()` /
`non_realtime_tick_pending()` exist for.

- The WAM adapter marks the processor dirty on a control write and services it
  **once per render turn**, right after the render call (`WamStage::service_non_realtime`
  / `mark_non_realtime_dirty`). A knob drag delivers many control messages in one
  turn; they collapse into a single pass over the **latest** values. Do not rely
  on one tick per parameter write — rely on "eventually, with the latest values."
- It is **not** an audio-thread callback and never runs inside `process()` — but
  in a worklet-only host it does run on the same OS thread, just outside the
  render call. **A long tick still makes the next quantum late.** Keep the work
  bounded and proportional to what actually changed.
- CLAP reaches the same hook a different way (`request_callback` →
  `on_main_thread`), and **native** CLAP gets it too. See the `clap` skill.

## Landmine: `emscripten_resize_heap` stubbed to `false` = a bare abort past 16 MB

The WAM runtime used to hard-stub `emscripten_resize_heap` to return `false`
while every module links `-sALLOW_MEMORY_GROWTH=1`. The two together mean: the
first allocation past the **16 MB initial heap** fails, and the module aborts
with **no diagnostic** — no OOM message, no exception, just a dead plugin. It
reproduces only on inputs large enough to grow the heap, so a small demo passes
and a real one dies.

`wam-runtime.mjs` now implements real geometric growth, capped at the wasm32
ceiling. If you write or vendor another JS runtime shim for a Pulp wasm module,
`emscripten_resize_heap` **must actually grow the memory** — a `false` stub is
only correct if the module is also linked with `ALLOW_MEMORY_GROWTH=0`, and none
of them are.

## Landmines that make a *rendering* build fail silently

Both live in the `skia-gpu-build` skill's wasm section; know they exist:

- **No `SK_TRIVIAL_ABI`** → `wasm-ld` links a *trapping stub* for cross-boundary
  `sk_sp` calls and the first frame dies with a bare `RuntimeError: unreachable`.
- **Emscripten's `SkFontMgr_New_Custom_Empty`** returns a non-null, glyph-less
  fontmgr reporting 1 family / 1 face, so null-checks *and* family-count guards
  both pass while every string measures at **zero width**. Probe font usability
  by drawing a glyph (`unicharToGlyph('A') != 0`), never by counting families.

## Browser-host rules

- **Probe for WebGL2; a browser without it is a shipping configuration.**
  `pulp::view::web::browser_host_gpu_available()` is the web analogue of
  `decide_gpu_host`. The mount path must fail loudly and asynchronously so the
  host page can fall back (the demo pages restore the player's generated
  parameter grid). A module that fails asynchronously and reports nothing leaves
  an empty panel, which reads as a render bug.
- **WebGL context loss is a normal event.** Lost → the surface reports
  unavailable and the rAF loop keeps pumping; restored → Ganesh is rebuilt and
  repaints. Any GPU resource cached above the surface must survive that cycle.
- The render loop is `requestAnimationFrame`-driven
  (`core/render/src/render_loop_emscripten.cpp`); DOM pointer/key events are
  translated in `core/view/include/pulp/view/web/web_event_translate.hpp`.

## Testing — the assertions a native test cannot reach

The `WAMv2 + WebCLAP (Linux, headless Chrome)` lane
(`.github/workflows/web-plugins.yml`) is the gate. Two conventions:

- **Drive both ABIs from ONE runner.** `superconvolver_runner.mjs` asserts the
  same audio / parameter / latency / state behavior against the WAM module *and*
  the WebCLAP module, so a divergence between the two fails in CI rather than in
  a demo page. Do this for any plugin that ships in both.
- **Real pixels + a real gesture.** The browser fixture asserts a WebGL2 context,
  GPU and raster content floors, text ink, a synthesized drag producing a
  bracketed `gestureBegin → setParameterValue → gestureEnd`, a host→UI repaint,
  and the context loss/restore cycle. Nothing here is reachable from a native
  unit test; don't substitute one.

The lane pins emsdk (never `latest`) and fetches the Skia wasm slice from
`tools/deps/manifest.json` — see the `ci` skill's `web-plugins.yml` section
before touching either.

## Demo pages

Publishing or updating a browser demo (shared player, cache-busting, OG images,
COOP/COEP) is the `screenshot-sync` skill plus the personal `pulp-web-demo`
standard. The one rule worth repeating here because it silently kills audio:
**never cache-bust the worklet processor URL** — the registered processor name is
derived from it, so a `?v=` forks the name and the node never constructs.
