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

**Proving the ENGINE is not proving the PAGE.** The engine fixture is allowed to
cheat — `validate-gpu.mjs` measures the impulse response on the CPU engine in one
run and hands it to the worker in the next. A visitor gets no such favour, so
everything between the plugin's data and the worker's kernel is UNCOVERED by it,
and each link fails *silently*: the page loads, the CPU path plays, the GPU engine
simply never appears, and nothing anywhere errors. Drive the ASSEMBLED page in a
real browser too (`page-gpu.mjs`), and assert the worker's counters ADVANCE — not
that a control exists (it can be inert), not that there are no errors (a silent
fallback is errorless by design), and not a cumulative `produced > 0` (that latches
on the first block ever made and then reads "GPU" forever, including while a lost
device misses every deadline).

### Getting the plugin's data OUT to the page (and the trap in it)

A plugin whose DSP can also run OUTSIDE the worklet — a GPU worker, which an
`AudioWorkletGlobalScope` cannot even reach — has to tell that somewhere what to
work with. For a convolver that means the IR, and it must be the IR the plugin
ACTUALLY ENDED UP WITH, after its own normalize/window pass. Handing the page a raw
IR to give to both sides is **not** equivalent: the plugin transforms what it is
given, so the two engines would convolve with different kernels and the CPU
fallback would quietly stop being a substitute for a missed GPU block.

The seam that works: plain **optional wasm exports** (`pulp_ir_generation` /
`pulp_ir_snapshot` / `pulp_ir_data`) → the worklet polls → `postMessage` → the
adapter latches and re-emits (`onIrChanged`) → the page forwards it. Not a new CLAP
extension: an extension is permanent ABI invented for one demo, carried forever by
every host that is not this page. An export is opt-in and invisible — the shared
worklet checks whether the module has it and does nothing when it does not.

Two things that WILL bite:

- **Do not gate the poll on the non-realtime tick.** It looks obviously right — the
  tick is what rebuilds the IR — and it silently never fires for the FIRST one. The
  plugin builds its initial IR while ACTIVATING, so by the time audio is running it
  has nothing pending, never asks the host for a callback, and a tick-gated poll
  waits forever for a rebuild that already happened. Poll the generation counter
  every quantum; it is one wasm call returning a `uint32`.
- **Latch it in the adapter.** The first value is published while the plugin
  activates — before the page's handler exists. Without a latch (and a replay on
  subscribe) that first publish is lost, and the consumer waits for a change that
  already happened.

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

## Landmine: iOS Safari ignores `.click()` on a `display:none` file input

The page's IR/sample picker is a `<input type=file>` the plugin's Source affordance
triggers (editor → `load_ir_path({})` → `Module.onRequestIr` → `filePicker.click()`).
Hiding that input with `hidden` / `display:none` works on **desktop** Safari and
Chrome but **iOS Safari silently drops the `.click()`** — the tap does nothing and it
reads as a broken control. Keep the input in the render tree and hide it visually
instead (`position:fixed; width:1px; height:1px; opacity:0; pointer-events:none`).
The click must also still be **synchronous inside the user gesture** — the whole
canvas-pointer → wasm → `EM_JS` → `click()` path is synchronous, so that holds; do not
defer it behind a promise/`setTimeout`.

## Landmine: a control-surface canvas eats page scroll — opt into `pan-y`, and it's TWO paths

`web_input.cpp` sets `canvas.style.touchAction = 'none'` and `preventDefault`s BOTH
pointerdown AND wheel so a knob drag / wheel-over-knob never pans the page — correct for a
standalone plugin. But an editor **embedded in a scrollable page** (only horizontal
sliders, no vertical drags) then swallows every gesture and the page cannot be scrolled
from on top of the plugin. Opt in by setting `canvas.style.touchAction = 'pan-y'` after
mount (see `pulp-ui.js`); the input layer checks the live `touch-action` and, on a
pannable canvas:

- **touch** (mobile): skips pointer capture + `preventDefault` on pointerdown, so a
  VERTICAL drag scrolls the page while a HORIZONTAL drag or tap still drives a slider
  (`pan-y` claims only the vertical axis).
- **wheel** (DESKTOP trackpad/mouse — a SEPARATE path, easy to forget): still forwards the
  wheel to the view but does NOT `preventDefault`, so the page scrolls. **The touch fix
  alone leaves desktop broken** — the "big screen, can't scroll over the plugin" bug is the
  wheel handler, not touch-action.

Do NOT flip the default to `pan-y` — a knob-heavy plugin needs both axes for its own
drags. VERIFY IN WEBKIT, not just Chromium: `playwright-core`'s `webkit` IS Safari's
engine (`Version/…Safari/605`), and `page.mouse.wheel(0, 600)` over the canvas must move
`window.scrollY` (measured 0→209 on the SuperConvolver editor). Chromium-only "it scrolls"
is not proof for the browser the user is actually on.

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

## Landmine: the OG bake is a TWO-PASS assemble — the second pass needs the same args

`gen-og-images.mjs` shoots each page, then `assemble-gallery.mjs` runs **again** to bake the
`og:image` / twitter block into the HTML — a page's tags are emitted only when its `og.png`
already exists on disk, which is only true on that second pass.

Run the second pass **bare** (`node assemble-gallery.mjs` with no `--wam-build` / `--ui-build`
/ `--gpu-build`) and the assembler cannot find the build trees, so it **skips** every page that
needs one. It skips them quietly — a one-line note — and therefore never rewrites their HTML,
which is the only thing that pass exists to do. `/super-convolver-gpu/`'s og.png was shot
perfectly and then never referenced: the page shipped with **no `og:image` at all** and
unfurled bare. Pass the same trees to both passes.

## The page owns the engine readout, not the view tree

A status line *inside* the plugin canvas that the page refreshes on a timer is a layout event
on a timer: every time a counter gains a digit the label re-measures, and at phone width it
sheared straight through the knob labels above it. It was also duplicate chrome — the page
already renders an Engine `<select>` directly under the canvas, which both names the engine and
is the control that changes it.

Put live metrics in **DOM slots** beside that control, each with a fixed width and
`font-variant-numeric: tabular-nums`, so a changing number never moves the layout. The view
tree carries the controls; the page carries the readout.

## Landmine: an offered engine is not a running engine — gate the WORK, not the output

A GPU (or any offload) lane fed by a SharedArrayBuffer ring keeps receiving blocks in **both**
engines: the plugin pushes on every block regardless, because that is what advances the shared
block timeline and keeps the two paths sample-aligned. A worker that convolves whatever it is
handed therefore keeps the GPU **fully busy while the user is on CPU** — measured on the
shipped SuperConvolver page at ~100 queue submits per second, a whole wet stream produced and
discarded. The user sees it: *"in CPU mode I still see spikes on my GPU meter."* Selecting CPU
must make the GPU **idle**, not merely ignored.

Gate it with a ring FLAG the worker reads once per tick (not a message — the worker's job is to
never stall). While it is clear: drain the input ring into plain memory, submit nothing.

**But idling is only half of it.** A partitioned convolver's tail comes from a frequency-domain
delay line of recent input spectra, and **updating that line IS the GPU work** — the forward FFT
is a dispatch. So an idle GPU is one whose memory of the recent past goes stale, and resuming
with a stale line smears a **ghost of pre-flip audio** under the new material. Buffer the raw
input blocks while idle (a memcpy) and **replay them on the flip**, oldest first with outputs
discarded, to rebuild exactly the line the GPU would have had. One priming burst, then live.
Keep the history exactly as long as the convolver's memory (`ceil(irFrames / block) + 1`);
replaying more just overwrites the same partitions.

Assert BOTH halves in the browser fixture, from the worker's OWN counters in the SAB (never a
label the page prints): submits do **not** advance while on CPU, and the priming counter is
non-zero after the flip. Mutation-check the first one — an always-on worker must make it fail.

## Landmine: the adapter's `values` array is POSITIONAL, not keyed by parameter id

`onParamsChanged(values, infos)` hands you `values` built as `infos.map(p => value(p.id))` — so
`values[i]` belongs to `infos[i]`. Reading `values[param.id]` "works" only when ids happen to
equal indices; on SuperConvolver, Engine's id is 5, so `values[e.id]` silently read the SIXTH
parameter. Find the INDEX (`infos.findIndex(...)`) and read `values[index]`. This bug hid for a
long time because the page's `<select>` handler also set the same state directly — the broken
path only ran for preset/host writes.

## Landmine: the wasm UI build hand-lists its sources — a core refactor silently breaks it

`tools/cmake/PulpWebUi.cmake` builds Pulp's own view+canvas render stack to wasm by
**explicitly listing every TU** (it can't glob core/ the way the native build's CMakeLists do —
it deliberately excludes Dawn/Graphite/scripting/design-import TUs). So when a core refactor
**splits a file** — `skia_canvas.cpp` → `skia_canvas.cpp` + `skia_canvas_path.cpp`, or the text
editor into `text_edit_model` / `text_editor` / `_clipboard` / `_ime` / `_paint` — the native
build (which lists or globs them) keeps working, and this hand-list silently goes stale. The
failure is a wall of `undefined symbol:` at wasm-ld link time, and it lands far from the PR that
caused it.

It merges green because the web-build lanes (`web-plugins.yml`, `wclap-cloudflare.yml`) are
**path-filtered and advisory** — a PR touching only `core/canvas/**` or `core/view/**` never
triggers them. So a core-only refactor breaks the wasm UI build with nothing red. (Making the
WebCLAP lane a required check is the durable fix; until then, assume this can be broken on main.)

When you hit it: don't chase symbols one build at a time. Read the symbol's namespace, find the
defining TU (`git grep -l 'Thing::method' -- core/.../src`), and **mirror what the native build
compiles** (`core/view/CMakeLists.txt`, `core/canvas/…`) rather than adding files piecemeal —
one added TU pulls in its own new references (a Label opening a TextEditor cascades into the
editor's model + clipboard + context-menu TUs, and finally into `pulp::platform::Clipboard`,
which had no web impl at all). Build locally against `origin/main` before trusting it — the
cascade is real, and your local linker matches CI (verify the first layer agrees before chasing
the next).

## Landmine: an animating web editor recurses paint→repaint unless the render loop is armed

A view that calls `request_repaint()` from inside `paint()` — any continuously animating editor
(SuperConvolver's living field) — routes through `WindowHost::mark_dirty()` →
`schedule_repaint()`. That method only requests an async rAF frame when `PULP_VIEW_HAS_RENDER_LOOP`
is defined; otherwise it falls back to a SYNCHRONOUS `repaint()`, which re-enters `paint()` from
within `paint()` and recurses until the JS stack overflows. Two traps compound it:

1. **The macro is native-only by default.** `PULP_VIEW_HAS_RENDER_LOOP=1` is set on the native
   `pulp-view-core` target. The wasm UI build (`PulpWebUi.cmake`) hand-lists its sources and does
   NOT inherit it — even though it DOES compile and arm the rAF `RenderLoop`. Define it in
   `PulpWebUi.cmake` or every animating editor deadlocks on first paint.
2. **`run_event_loop()` marks dirty before the loop exists.** It calls `show()` → `mark_dirty()`
   BEFORE creating the render loop, so the first paint is synchronous even with the macro. The
   browser `WindowHost::render_frame()` carries a re-entrancy guard (`rendering_` flag): a repaint
   requested during paint is deferred to the next frame, never nested.

The crash is a red herring generator: the stack overflows in whatever draw is executing when it
tips (a gradient FP, a texture upload), so the symbolized top frames point AWAY from the cause.
Count the repeating frame — it was 662× `View::paint_all` — to find the real cycle. The static
generated grid never animates, so this stays hidden until a real animating editor mounts. **A
new custom editor must be exercised by the browser fixture (it mounts and must not overflow).**

## The full-canvas editor pattern (a plugin's REAL Skia UI on the web)

**Two modes, a per-plugin choice — full-canvas is ADDITIVE, not a replacement.** A demo
page runs in one of two modes, selected by the `customUi` seam:

- **Default (grid) mode** — the shared player renders the plugin's parameters as a knob
  grid, generated declaratively. Zero bespoke UI code. This is the norm and the right
  choice for a simple utility (a gain, a basic filter).
- **Full-canvas mode** — the page supplies a `customUi` (a compiled Skia UI module) that
  paints the plugin's whole bespoke editor onto ONE canvas filling the panel. Opt-in, more
  work, native-quality. Pick it when the plugin's identity is a custom visual (an EQ's
  frequency-response curve + draggable bands, a convolver's IR/field, a synth's scope).

No `customUi` → grid; `customUi` present → full-canvas — **and the full-canvas mode FALLS
BACK to the grid if the module fails to mount**, so adding it never risks the baseline.
Everything below is the full-canvas path.

A **full-canvas editor** paints the plugin's whole bespoke UI onto ONE Skia canvas that
fills the panel edge to edge — the same native editor the DAW shows, running in the
browser. `super-convolver-gpu` is the reference implementation; **copy it, do not reinvent
it.** Where to look:

- `examples/super-convolver/super_convolver_ui.hpp` — the editor itself (a `vw::View`
  drawn with the canvas API). This compiles into BOTH the native plugin and the wasm UI
  module — **one source, two builds.**
- `examples/web-demos/super-convolver-ui/` — the web mount: `ui_entry.cpp` (the
  `EMSCRIPTEN_KEEPALIVE` seam), `super_convolver_web_host.hpp` (a browser shim
  implementing the editor's host interface), `pulp-ui.js` (`mountPulpUi`), `CMakeLists.txt`.
- `tools/cmake/PulpWebUi.cmake` — builds the editor + Pulp's view/canvas/Skia stack to wasm.
- The GPU page in `examples/web-demos/wclap-build/cloudflare/assemble-gallery.mjs`
  (`customUi` + the CSS that widens `#panel.pulp` and pins the canvas height).
- Docs: `docs/guides/web-plugins.md` ("Native Editors on the Web"), and the size/tier
  design in `planning/2026-07-15-web-editor-architecture-and-size.md`.

**The decoupling contract (what lets one editor serve native + web).** The editor talks
ONLY to a small host interface (`SuperConvolverUiHost`: gpu_status / ir_path /
load_ir_path / impulse_response_snapshot) + `StateStore` + a data bus — NEVER the DSP
header. Native host = the processor; web host = the browser shim. A new plugin defines
its own `<Plugin>UiHost` interface and two implementors.

**Params cross the DSP, not the editor.** The editor reads its parameter list from the
DSP descriptor (adapter → `pulp-ui.js` → `g_store`). So a param the editor draws must be
DECLARED BY THE DSP — declare the SAME set the native build declares (the web build once
dropped `Rooms` and the editor showed four sliders where native shows five). Rebuild BOTH
the UI module (`build-webui`) AND the DSP wasm when params change; the UI module alone is
not enough.

**Full-width layout.** Strip the page chrome the editor already draws (engine dropdown,
CPU/GPU blurbs, a separate load-file area) — it duplicates what the canvas paints. Widen
`#panel.pulp` (`max-width: min(1200px,94vw)`) and give the canvas a FIXED height
(`clamp(420px,60vh,680px)`), not an aspect ratio (aspect makes the box a function of width
and collapses on a phone).

### Responsive + iOS is NOT optional — the layout is DRAWN, so CSS won't save you

A full-canvas editor lays itself out in code, so a phone-width bug (overlapping header,
sheared slider labels) never surfaces in a unit test — only in a browser, at that width.
Every full-canvas editor MUST handle these, all learned the hard way on SuperConvolver:

- **Narrow breakpoint.** The UI `scale()` keys off HEIGHT, so a tall skinny phone canvas
  has a LARGE scale and a SMALL width — a centered header (mode tabs) then collides with
  the wordmark on the left and the status chip on the right, and a quarter-width slider
  cell cannot fit "MIX" and "35 %" on one line. Add a `narrow_` mode (`W < ~680*scale`):
  drop the tabs to their own row, give secondary chrome its own row, and STACK each slider
  as label-over-value-over-track. Keep desktop untouched.
- **The info/help overlay must reflow too** — a fixed two-column card overflows a phone;
  give it a full-width, stacked, clipped variant.
- **iOS file picker:** the page's `<input type=file>` must NOT be `display:none`/`hidden`
  (iOS Safari drops `.click()` on it) — hide it visually instead. See its own landmine above.
- **Page scroll over the canvas:** opt into `touch-action: pan-y` after mount so a vertical
  drag scrolls the page while a horizontal drag/tap still works the controls. See its landmine.
- **Live readouts:** reserve their height (two lines if they can wrap) and WRAP rather than
  truncate, so flipping state never bumps the page; debounce fast-toggling values (~0.8s
  hysteresis) so a single dropped poll doesn't strobe the text.

### Verifying it: a browser pass at three sizes is PART OF THE JOB, not a favor to ask for

Do NOT ship a full-canvas editor and let the user discover overlapping text. Before calling
it done, run the bundled audit and LOOK at every size — this is the workflow, not an extra:

```sh
node examples/web-demos/tools/responsive-audit.mjs \
  --url https://<preview>/<plugin>/ --out /tmp/audit
# then READ /tmp/audit/{desktop,tablet,phone}-editor.png with your own eyes
```

It screenshots the page + editor at desktop (1440w), tablet (834w), and phone (390w),
prints the editor's CSS size and computed `touch-action`, and lists what to check
(header collisions, slider label/value overlap, the info card, even fill, and — on phone —
the two-line readout, the file picker, and scroll). Drive real interactions with CDP
`Input.dispatchTouchEvent` (a REAL touch), not synthetic `new TouchEvent` (which does not
drive scrolling and gives a false negative). Fix what the screenshots show, redeploy, re-run.
