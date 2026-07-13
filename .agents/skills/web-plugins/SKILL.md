---
name: web-plugins
description: Pulp in the browser — the WAM v2 and WebCLAP adapters, the wasm runtime, the Skia/WebGL2 browser window host, and the WebGPU (emdawnwebgpu) GPU-audio lane. Covers what does and does not compile to wasm, the worklet-thread constraints (no std::thread, no fetch, no navigator.gpu), the non-realtime tick, memory growth, how to PROVE a GPU lane actually ran rather than silently falling back to CPU, and the silent-failure traps that make a wasm build "work" while producing no audio or no pixels.
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

- **The UI is not Graphite/Dawn.** The published Skia wasm slice is **Ganesh on
  WebGL2** and ships zero `wgpu` symbols (see the `skia-gpu-build` skill's wasm
  section). WebGL2 has **no compute shaders**, so the UI's render path cannot
  carry GPU DSP — the two lanes are unrelated (see below).
- File-backed loaders and native editors are compiled out (`PULP_WASM` /
  `PULP_HEADLESS`). A plugin that needs an asset on the web must carry it in the
  binary or fetch it on the main thread — an `AudioWorkletGlobalScope` cannot
  fetch, which is also why the WAM worklet build must be `SINGLE_FILE` (the wasm
  embedded in the .js).

## GPU audio in the browser: it exists, and it is a THIRD lane

`examples/web-demos/gpu-audio/` runs SuperConvolver's convolution as a **WGSL
compute shader on the browser's real WebGPU device**. Do not repeat the old line
that this is impossible or unstarted. But be equally precise about its shape,
because three separate constraints force it and each one is a trap:

- **The DSP links `emdawnwebgpu`, not Skia.** Dawn's Emscripten port implements
  `webgpu.h` over `navigator.gpu` and is **completely independent of Skia** — the
  UI's Ganesh/WebGL2 slice is irrelevant here and must not be dragged in.
  `tools/cmake/PulpGpuWasm.cmake` (`--use-port=emdawnwebgpu`) is deliberately
  Skia-free.
- **An `AudioWorklet` cannot touch `navigator.gpu`. At all.** So the compute
  CANNOT live where the audio lives. It runs in a **DedicatedWorker**, and hands
  finished blocks to the worklet over **SharedArrayBuffer rings** — the same shape
  as native `GpuAudioTransport`: fixed latency primed as N blocks, lock-free ring,
  a `MissPolicy` the audio thread applies when a block is late. (WebCLAP already
  needs COOP/COEP, so the SAB is free; a WAM-only page would have to earn it.)
- **The native blocking readback is FATAL in a browser.** `MapAsync` + a spin on
  `ProcessEvents` works natively and **deadlocks on the web**: the spin starves the
  very JS event loop that would resolve the map. The browser arm must be genuinely
  async — submit block N+1 while N's map is still in flight, bound the in-flight
  depth, and route a blown deadline to `MissPolicy` instead of hanging.

**Never claim it is faster than the CPU.** It is not, and the plugin does not
default to it. The 2026-06-29 spike measured a competent real-FFT CPU convolver
beating or tying the GPU at every musically plausible setting. This is a
**capability** result — convolution reverb running as a compute shader in a
browser tab — and the CPU remains the default and the always-available fallback.

### Proving a GPU lane ran is the whole problem

A GPU path that silently falls back to the CPU is **indistinguishable from one that
works**. "It sounds right", "the device and pipelines exist", and "no errors" all
pass falsely when nothing ever dispatched. Two assertions carry real weight, and
`browser-test/validate-gpu.mjs` is the worked example:

- **Kill the safety net.** Run with the CPU fallback disabled: a GPU that never
  produced a block then yields **silence**, so the run cannot be quietly covered.
- **Tamper with the shader and watch the samples move.** Read the **actual WGSL
  text back out of the shipped module**, rewrite it (scale the output store by
  0.5), push it back before init, and require the audible samples to come out
  exactly 0.5× — bit-for-bit. A JS or wasm impostor doing the arithmetic elsewhere
  is *completely unaffected* by editing a shader it never runs. Reading the source
  out of the module rather than pasting a copy into the test also makes it
  drift-proof.

**Never assert on `timestamp-query`.** Chrome quantizes it: a 512-point FFT block
measures **0 ns**. A zero there is not evidence the GPU idled, and asserting either
way manufactures false evidence. Report it; never gate on it.

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

### "Keep the work bounded" is a hard requirement, not advice

Coalescing an expensive rebuild to once-per-render-turn lowers how *often* it
fires; it does **not** make it safe. SuperConvolver's IR rebuild (synthesis +
a peak-response FFT over the whole IR + a partitioned FFT re-plan for both
channels) measured **15.0 ms in one render callback against a 2.667 ms quantum
budget** (128 frames @ 48 kHz) — a dropout on every Size change, even coalesced.

The fix is to **time-slice** the work, not to shrink it. The pattern
(`examples/super-convolver/sliced_ir_rebuild.hpp`) generalizes:

- Express each phase as a **stream of fixed-cost items**; the tick consumes at
  most `budget` of them and returns. Budget is a **constant**, never a function
  of the input size — that is the whole point.
- Keep the OLD state rendering audio until the new one is complete, then publish
  it through the existing lock-free handoff (`signal::ConvolverIrSwapper` /
  `runtime::Handoff`). The audio thread's only job stays `try_swap_ir()`.
- A new request mid-job must **supersede** (restart) it, not queue behind it — a
  knob drag delivers a stream of values and only the last one is ever heard.
- **`non_realtime_tick_pending()` must stay `true` while a job is in flight**, not
  just when one is *needed*. Both web hosts stop pumping the tick the moment it
  says false — WAM skips `service_non_realtime`, and WebCLAP stops calling
  `request_callback` — so a job that only advertises "work needed" strands itself
  half-built.
- Pick the budget by **measuring the slowest phase on the scalar path the browser
  actually runs** (no vDSP/Accelerate — that is Apple-native only, and it makes a
  large FFT ~3× faster than what wasm will do). SuperConvolver uses 32768 items ⇒
  worst render callback 0.78 ms across a full Size drag, vs 15.0 ms before.
- The cost you pay is **latency, not glitches**: a 4 s IR needs ~120 render turns
  (~300 ms) to rebuild. Crossfade the swap (`PartitionedConvolver::set_crossfade`)
  so the change is heard as continuous rather than as a switch being thrown, and
  remember that any test which measures an IR right after a swap will otherwise
  measure the *blend*.
- An FFT over the whole input is the one thing that will not chunk. Decompose it:
  a truncated Gentleman–Sande (DIF) split down to 1024-point leaves turns one
  indivisible N-point transform into a stream of butterflies plus a stream of leaf
  FFTs, exact to float precision. See `superconvolver::PeakResponseScan`.

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

## Landmine: CLAP has no parameter `unit` — only `value_to_text`

The WAM ABI reports a parameter's display unit directly (`wam_adapter.cpp`
emits `{"unit":"%"}`). CLAP deliberately does **not**: `clap_param_info` has
no unit field, and a host is expected to *display whatever
`clap_plugin_params.value_to_text()` renders* ("35.00 %"). A WebCLAP host that
marshals only the info struct therefore produces unitless parameters, and the
shared player — same page code, same plugin — renders "1.50" on the WebCLAP
demo and "1.50 s" on the WAM one. That was a real, shipped divergence.

Both WebCLAP hosts (`packages/…/vendor/pulp-wasm/wclap-processor.js` worklet
and `core/format/src/wasm/wclap-host.mjs` offline) therefore call
`value_to_text` at **two probe values** per parameter and report the raw
strings as `textProbes`; `deriveDisplayUnit()` in `wclap-abi.mjs` recovers the
suffix (and returns `""` rather than inventing one when a plugin uses a custom
`to_string`, e.g. enum labels). If you add a field the Pulp web UI needs and
CLAP has no struct slot for it, this is the pattern — go through the plugin's
own display call, don't hardcode a default in the adapter.

## Handing a plugin BINARY data from the page (samples, IRs, wavetables)

A browser has no filesystem, so a plugin whose native build loads a file
(`vw::FileChooser` → `set_ir_path`) needs a web equivalent. **Do not add a
per-ABI entry point for it.** Go through the plugin's own state:

- Both ABIs already expose the plugin's opaque state behind ONE `HostAdapter`
  call — WAM through `wam_state_size`/`wam_read_state`/`wam_write_state`, WebCLAP
  through the `clap.state` extension — and both produce the *same* `PLST` blob the
  native VST3/AU/CLAP builds write.
- So: read the live state, swap its **plugin-owned blob** for a record carrying the
  payload, write it back (`packages/pulp-web-player/src/state/plugin-state.js` —
  `parseContainer` / `buildContainer`, exported from the package). The plugin's
  `deserialize_plugin_state()` is the receiver. Preserving the `params` half of
  the container is what stops a load from resetting the user's knobs.
- Three things fall out for free, which is why this is the seam: it is **identical
  on WAM and WebCLAP** (nothing to keep in sync), the payload **survives a state
  save/restore** because it *is* the state, and "revert to the built-in" is the
  same call with a different tag.
- Decode with the demo's **own `AudioContext`** (`decodeAudioData` resamples to the
  context rate), so the PCM arrives at the session rate and the plugin's resampler
  — which is not chunkable, unlike everything else in the rebuild — is skipped.
- Worked example: `examples/web-demos/super-convolver-ui/ir-source.js` (the SCv2
  record + the drop-zone) and the `onReady` seam in the shared shell, which is the
  hook for **plugin-specific page chrome** that needs the live adapter and the
  AudioContext. `customUi` is the wrong hook for this: it *replaces* the parameter
  grid and falls back to it on failure.

## Landmine: a WebCLAP host must READ parameters back, not mirror them

A host that remembers what it last *sent* (`values.set(id, v)`) goes stale the
moment the plugin rewrites its own parameters — which it does on every state load
and preset change. `WebClapPlugin.paramValue()` calls
`clap_plugin_params.get_value()`; use it. The WAM lane has the same trap and
solves it with `wam_param_epoch` + `wam_read_param_values`.

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

### Landmine: `pulp_add_wclap(Foo)` declares the target as `Foo-wclap`

Not `Foo`. `cmake --build … --target Foo` is a hard **"No rule to make target"**,
not a fallback — so a workflow step with the bare name fails before it reaches
whatever it was supposed to prove. Shipped that way once in `web-plugins.yml`.

### Landmine: a plugin is not time-invariant while its IR is still rebuilding

Any fixture that **measures an impulse response and then convolves with it** (the
standard way to check a convolver against an oracle) is assuming the plugin is
linear and **time-invariant** across the capture. SuperConvolver is only that once
its IR has stopped moving, and it does not start that way: a Size change is
**time-sliced** across many `process()` calls and crossfaded
(`superconvolver::SlicedIrRebuild`), exactly so the render callback stays in budget
instead of spiking (see "Keep the work bounded").

Set a parameter and capture immediately and you probe `h` **mid-crossfade** — a
blend of two IRs, a kernel that describes no instant of the run. Everything
downstream is then judged against it, and the failure **frames the ENGINE as broken
when the fixture is what is wrong** (measured: CPU vs oracle relative RMS **3.4** —
not a drift, a different signal).

Two things fix it, and you want both:

1. **Pre-roll of silence** before the marker impulse. The graph is live and the
   plugin drains its sliced rebuild while nothing is being measured.
2. **A second impulse after the analysis window**, whose response must equal the
   first's. A pre-roll alone is just an assumption that convergence fits inside it,
   and it rots the moment the slice budget or the IR length changes. The second
   impulse turns it into a **checked claim** about the exact property the oracle
   depends on, and it fails loudly with a named reason *before* the bad kernel can
   poison anything.

## Demo pages

Publishing or updating a browser demo (shared player, cache-busting, OG images,
COOP/COEP) is the `screenshot-sync` skill plus the personal `pulp-web-demo`
standard. The one rule worth repeating here because it silently kills audio:
**never cache-bust the worklet processor URL** — the registered processor name is
derived from it, so a `?v=` forks the name and the node never constructs.
