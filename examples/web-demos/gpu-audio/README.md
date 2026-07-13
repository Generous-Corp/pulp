# GPU audio in a browser tab

SuperConvolver's FFT convolution, running as WGSL compute shaders on the
browser's WebGPU device, feeding an AudioWorklet.

**It is not faster than the CPU path.** A measured spike (2026-06-29) killed that
claim: a competent real-FFT CPU convolver matches or beats this GPU path at every
musically plausible setting. This is a **capability and headroom demonstration** —
the same convolution, running as a compute shader in a tab. The CPU engine is the
default, is always the fallback, and is never a correctness dependency on the GPU.

## Shape

An `AudioWorkletProcessor` cannot touch `navigator.gpu` and cannot spawn a
`Worker`. WebGPU is exposed on `Window` and `DedicatedWorker` only. So:

```
page (gpu-bridge.mjs)          worker (gpu-worker.mjs)         worklet (wclap-processor.js)
  probe() ─ SAB + isolation      owns the WebGPU device          owns the audio thread
  allocate + reset the ring ───▶ pulp-gpu-dsp.wasm (WGSL)  ◀───  writes input / pops wet
  startGpuLane() ── bring-up ──▶ adapter → device → shaders
  pollStats() at 10 Hz           → pipelines → self-test
```

All three map ONE `SharedArrayBuffer`, which needs cross-origin isolation
(COOP/COEP). The WebCLAP Cloudflare host already sets both, and the `/*` block in
`../wclap-build/cloudflare/_headers` covers the demo page.

### Every block carries its sequence number

The native transport (`GpuAudioTransport`) is POSITIONAL: it primes the output ring
with L blocks of silence and then trusts that exactly one wet block is published per
input block forever. In a browser that invariant does not hold, and it fails on the
two paths this design calls NORMAL:

- a full input ring **drops** a block (the worker is >= `slots` blocks behind — a
  backgrounded tab does this by design), so one input never reaches the GPU;
- a block whose deadline **expires** is never published, so one wet never lands.

Under a positional protocol each of those permanently shifts the wet stream against
the dry path by one block — silently, with no miss to observe, comb filtering
forever. So `gpu-ring.mjs` (SAB layout v2) stamps every input slot with the
worklet's monotonic block index, the worker echoes it onto the wet it produces, and
`pop()` asks for the block it is owed (`blockIndex - latencyBlocks`): an older wet is
discarded as stale, a newer one means this block's wet does not exist and the plugin
covers it from its CPU net. Drops, expiries, worker stalls and a live Engine flip all
collapse to a handful of MISSES — the one failure mode the whole design absorbs.

Two consequences worth knowing:

- **There is no silence priming.** The first L blocks MISS (the plugin's
  latency-aligned CPU wet covers them) instead of emitting L blocks of primed
  digital silence, so the fixture's headline is `296/298`, not `298/298`, and there
  is no ~21 ms dropout at the top of the stream.
- **The plugin drives the ring on EVERY block, including while Engine=CPU**
  (`fill_wet_web` in `super_convolver.hpp`). push/pop are what advance the shared
  block timeline; a ring that idled through a CPU stretch would still be holding the
  wets it produced before the flip, and the first pops after a flip back to GPU
  would emit THAT audio as if it were current. The cost is honest: the GPU worker
  keeps convolving blocks whose wet is discarded while the CPU engine is selected.

### The audio thread takes no locks

`push`/`pop` are allocation-free and lock-free, and they call neither
`Atomics.wait` (which would block the render callback) nor `Atomics.notify`. Notify
does not block — but it takes V8's shared waiter-list lock, and the audio thread
acquires nothing. The worker therefore polls on a bounded timer instead of parking
on a wake.

**A trap that cost 4 ms of the block budget:** HTML clamps a `setTimeout` scheduled
from inside a timer callback to >= 4 ms once the nesting level passes 5 — and a
polling loop built out of `setTimeout` IS a nesting chain. Measured on an M-series
Mac, that alone moved the submit → readback-resolve round trip from ~2.0 ms to
~6.0 ms of the 10.7 ms budget. `gpu-worker.mjs` hops through a `MessageChannel`
message (nesting level 0) before arming each timer, which restores the requested
interval.

## What shipped

| | |
|---|---|
| `/super-convolver-gpu/` | **Shipped.** The IR handoff is wired (see below), so the page's GPU engine actually runs — proven on the assembled page, in a real browser, by `browser-test/page-gpu.mjs`: the Engine toggle only exists once the plugin's IR has reached the worker, and with GPU selected the worker's own counters advance (measured: **0 of 192 blocks missed** in the steady-state window). `GPU_IR_HANDOFF_WIRED` in `assemble-gallery.mjs` remains the switch — set it false the moment the page could no longer honestly claim a running shader. The two shipped pages, `/super-convolver/wam/` and `/super-convolver/wclap/`, are unchanged and sha256-pinned by `gallery-smoke.mjs`. |
| Engine CPU/GPU toggle | Rendered **only** when the handshake succeeds AND the worklet really attached the ring (`descriptor.gpuLane`). Where it fails the toggle is **not rendered at all** and the page names the reason. An inert control is the "inert and misleading" failure the `PULP_WASM` compile-out already fixed once. |
| Status strip | One `Label` in Pulp's own view tree, repainted from `pulp_ui_set_gpu_status(json)` at 10 Hz on a **`setInterval`, not `requestAnimationFrame`** — rAF is throttled in a background tab, exactly when misses are most interesting. `budget_us` and `rt_percent` are derived on the page by the same arithmetic native `gpu_status()` uses, so the browser and the native build print the same numbers computed the same way. |

### The Skia carve-out is real in the wasm lane, aspirational natively

`pulp-gpu-dsp` links NO Skia — enforced, not asserted: the `pulp-gpu-dsp-skia-free`
CTest greps the object files and the generated compile/link lines
(`test/cmake/gpu_audio_web_tests.cmake`; opt in with `-DPULP_BUILD_WASM_TESTS=ON`).
That is a statement about the EMITTED MODULE. It is not yet true of the native
library: `GpuCompute` is still only built when `PULP_HAS_SKIA` is on
(`core/render/CMakeLists.txt` defines `PULP_HAS_DAWN` inside that branch), so a
native DSP-only consumer still drags in the Skia-gated render lib. Closing that is
separate work.

## What did NOT ship

- **Rooms / Flow.** The multi-IR and moving-field parameters stay native-only.
  They are a `GpuMultiConvolver` surface with no CPU safety net
  (`MissPolicy::Silence`) — in a tab, where a backgrounded worker misses deadlines
  as a matter of course, that is a silence generator, not a demo.
- **The WAM lane.** A WAM DSP module is a SINGLE_FILE worklet with no page-side
  seam to hand it a SAB and no worker to reach WebGPU through. WebCLAP already
  takes `processorOptions`, so the GPU lane rides that. WAM would need a new
  host-side hook for no additional proof.

## The proof — `browser-test/validate-gpu.mjs`

A real-time `AudioContext` in headless Chrome (measured: `state=running`,
`currentTime` advanced 1.4933 s over 1.5004 s of wall clock, sampleRate 48000,
with only `--autoplay-policy=no-user-gesture-required`). **Not**
`OfflineAudioContext`: it renders faster than wall clock and starves the async GPU
transport into permanent misses — with the CPU net killed, that is silence, i.e. a
guaranteed false FAIL.

| Run | What it proves |
|---|---|
| **A** | Engine=GPU with the CPU net **killed** ("GPU only" = 1). The audio must match `oracle.mjs` — an INDEPENDENT float64 direct time-domain convolution written in the fixture's own JS, not Pulp's convolver and not the wasm. If the GPU never produced a block, this run is SILENT and FAILS. **The backbone.** |
| **B** | **The tamper run.** `pulp_gpu_kernel_source("conv_bmul")` reads the ACTUAL WGSL the module hands to Dawn; JavaScript scales its output store by 0.5; `pulp_gpu_override_kernel` pushes it back before `pulp_gpu_init`. The audio must be Run A × 0.5 **sample-wise**, and differ from Run A. A JS/wasm impostor is unaffected by an edit to a shader it never runs. Reading the source out of the module (rather than pasting a copy into the test) makes it drift-proof. |
| **C** | Engine=CPU. Matches the oracle. It also measures the impulse response the oracle convolves with — so the kernel Run A is judged against never came from the GPU. The GPU worker keeps dispatching here (the plugin drives the ring on every block, whichever engine is selected), and that its wet was NOT emitted is what the oracle check proves: at this point the worker is still convolving with the unit impulse it was self-tested with, whose wet is the DRY signal — had any of it reached the output, the error would be enormous rather than ~2e-7. |
| **D** | The negative path: the worker script 404s. The lane must fail with a NAMED reason, no ring may attach, and the CPU convolver must still play correctly. |

### The engine is not LTI until its IR stops moving

The oracle's argument — measure `h` once, convolve, compare — assumes the plugin is
linear and TIME-INVARIANT across the capture. SuperConvolver is only time-invariant
once its IR has stopped changing, and it does not start that way: a Size change is
**time-sliced** across many `process()` calls and crossfaded
(`superconvolver::SlicedIrRebuild`), so the render callback stays inside its budget
instead of spiking. For a stretch after the fixture drives Size to its minimum, the
engine is genuinely crossfading from the default 1.5 s IR toward the 0.05 s one.

Probing `h` in that window measures a blend of two IRs — a kernel that describes no
instant of the run — and every comparison downstream is then judged against it. This
is not hypothetical: it is what happened, and the failure **frames the engine as
broken when the fixture is what is wrong** (CPU vs oracle: relative RMS 3.4 — not a
drift, a different signal).

So the source begins with a **pre-roll of silence**: the graph is live and the plugin
is draining its sliced rebuild while nothing is being measured. And because a
pre-roll is still only an assumption that convergence fits inside it — one that rots
the moment the slice budget or the IR length changes — the source **ends with a second
unit impulse**, and its response must equal the first's. That turns the assumption
into a checked claim: if the IR moved anywhere in the measured span, the two impulse
responses disagree and Run C fails loudly, before `h` can poison anything. Measured
drift on the recorded run: `0.00e+0` — bit-identical.

**Reported, never gated:** adapter info, `queueSubmits`/`mapResolves` monotonicity,
`queueSubmits >= produced`, and `gpuNsLast`. **`gpu_ns > 0` is never asserted** —
Chrome quantizes timestamp-query: measured 0 ns for a 256-element dispatch and
1,376,256 ns for a 1M-element one. A 0 for a 512-point FFT block does not mean the
GPU idled. "The device and the pipelines exist" is likewise a **diagnostic, not
proof**: it passes falsely when nothing ever dispatches and the CPU net covers.

The honest headline number the fixture prints is
`produced / (produced + miss)` over a 3 s real-time run.

### Alignment (why the oracle kernel is exact, not approximate)

The source buffer is `[unit impulse][D zeros][noise burst]`. The plugin's response
to the lone impulse IS its impulse response `h`, so the first non-silent captured
sample is a marker that removes the plugin's latency from both the kernel and the
signal window. With `D > N` the impulse's own response has left the window under
test, and by causality the first `N` samples of `x * h` depend only on `h[0..N)`.
No windowing, no tail error.

### Tolerance

`--tol` / `PULP_GPU_RMS_TOL`, **default 1e-5 — MEASURED, not guessed.**

Both engines' relative RMS error against the float64 direct-convolution oracle,
on Apple M-series / metal-3 under headless Chrome 150:

| engine | relative RMS error vs the oracle |
|---|---|
| GPU (WGSL FFT compute shaders) | **1.68e-7** |
| CPU (`PartitionedConvolver`)   | **2.03e-7** |

~2e-7 is the float32 rounding floor for an FFT convolution of this length, and the
GPU is no worse than the CPU. The gate is pinned ~50x above that floor: tight
enough that a wrong IR, a wrong scale, or an off-by-one in the overlap-add cannot
pass (those all land at 1e-2 or worse), loose enough to absorb another GPU's
rounding. The earlier 2e-3 placeholder was 10,000x the real error — it would have
passed a visibly wrong convolution.

**A trap worth naming**, because it cost a debugging cycle: the oracle's kernel is
the first `N` (8192) taps of the *measured* impulse response, so the alignment
argument above only holds while the plugin's IR is SHORTER than the analysis
geometry. At the default `Size` (1.5 s = 72,000 taps) it is not, and the CPU engine
then misses the oracle by 1.3e-2 while the GPU matches it to 1.7e-7 — which reads
like "the CPU convolver is broken" and is nothing of the sort. The GPU is exact
only because the fixture hands it that same 8192-tap kernel as its IR; the CPU
engine is honestly convolving with all 72,000 taps against an oracle that knows
8,192 of them. The fixture therefore drives `Size` to its minimum (0.05 s = 2,400
taps) in every run — see `setShortIr()`.

### Running it

```bash
source ~/emsdk/emsdk_env.sh
export WASI_SDK_PREFIX=~/wasi-sdk

# 1. the GPU-capable WebCLAP module (Engine + "GPU only" params)
cmake -S examples/web-demos/wclap-build -B build-wclap \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-wclap --target SuperConvolverGpu SuperConvolver

# 2. the Skia-free WebGPU DSP module the worker instantiates
emcmake cmake -S examples/web-demos/gpu-audio -B build-gpu-dsp -DCMAKE_BUILD_TYPE=Release
cmake --build build-gpu-dsp

# 3. the proof (exit 0 = PASS; PULP_REQUIRE_WEBGPU=1 makes a null adapter fatal)
node examples/web-demos/gpu-audio/browser-test/validate-gpu.mjs \
  --gpu-wasm build-wclap/SuperConvolverGpu.wasm --gpu-build build-gpu-dsp
```

## CI, honestly

- **Linux (`ubuntu-latest`, the existing `web-plugins` job)** — a GPU-less runner.
  The fixture **skips with a named reason** (`SKIP: webgpu-unavailable
  (no-adapter)`, exit 0). A non-gating probe step prints what `requestAdapter()`
  actually returns there, with and without `--enable-unsafe-swiftshader`, so the
  question "does Linux CI have a software WebGPU adapter?" gets answered by
  measurement instead of assumption. If it ever starts returning one, set
  `PULP_REQUIRE_WEBGPU=1` on that step and it becomes a second real gate.
- **macOS (`gpu-audio-macos`, the self-hosted runners)** — **this is the real
  gate.** Probed on the Mac Studio runner host on 2026-07-12: headless Chrome
  there reports a WebGPU adapter, `apple / metal-3`, with `timestamp-query` among
  its features. The job runs with `PULP_REQUIRE_WEBGPU=1`, so a null adapter is a
  HARD FAILURE and the fixture cannot degrade into always-skipped-always-green. It
  is skipped entirely where `PULP_LOCAL_MACOS_RUNS_ON_JSON` is unset (a fork with
  no runner pool), and it is advisory — not in branch protection — until it has run
  green for a while.
- **`--enable-unsafe-swiftshader` does NOT rescue WebGPU** on macOS: measured,
  Chrome has no software WebGPU adapter there at all
  (`--use-webgpu-adapter=swiftshader` and `forceFallbackAdapter: true` both return
  `null`). It is not passed by the GPU fixture, because it would only obscure which
  lane actually ran.
- A WebGPU probe served from `about:blank`, a `data:` URL, or `file://` reports **no
  `navigator.gpu` at all** — WebGPU needs a secure context. Every probe here serves
  over `http://localhost`. This is the first thing that makes a naive probe lie.

## The IR handoff (closed)

The GPU worker must convolve with the impulse response the PLUGIN is actually using. Not
an equivalent one, and not the raw file the user picked: the plugin NORMALIZES and WINDOWS
what it is given (`build_base_ir`, keyed off Size), so handing the page a source IR to give
to both sides would leave the two engines convolving with different kernels — and the CPU
convolver would stop being a sample-for-sample substitute for a block the GPU missed. The
"safety net" would really be a second, different reverb cutting in.

This was open for a while, and the demo page was withheld the whole time, because a page
titled "GPU engine" that serves 100 % CPU to every visitor is a capability lie — displaced
from the toggle (which correctly refuses to render) onto the chrome and onto a link someone
pastes into Slack.

It is now wired, end to end:

| Link | Where |
|---|---|
| The plugin publishes its live, post-transform IR + a generation counter | `pulp_ir_generation` / `pulp_ir_snapshot` / `pulp_ir_data` — plain wasm exports in `wclap-build/plugins/super_convolver_gpu_wclap.cpp`. Deliberately NOT a new CLAP extension: an extension would be permanent ABI invented for one demo, carried forever by every host that is not this page. An export is opt-in and invisible. |
| The worklet polls it and forwards the samples | `wclap-processor.js` `pollIr()`. **Polled every quantum, not on the non-realtime tick** — see the trap below. |
| The adapter latches it and re-emits | `adapters/wclap.js` `onIrChanged`. Latched because the first IR is published while the plugin ACTIVATES, before the page's handler exists; assigning the handler replays it. |
| The page hands it to the worker | `lane.setIr()` → `gpu-worker.mjs`, which re-prepares at a safe point in its loop. |

**The trap, and it is a good one.** Gating the poll on the non-realtime tick looks obviously
right — the tick is what rebuilds the IR — and it silently never fires for the FIRST one.
The plugin builds its initial IR while activating, so by the time audio is running it has
nothing pending, never asks the host for a callback, and a tick-gated poll waits forever for
a rebuild that already happened. The GPU worker sits with no kernel, the page never offers
the toggle, and nothing anywhere reports an error. Poll the generation counter every
quantum; it is one wasm call returning a `uint32`.

**The swap is covered by the existing safety net, for free.** `pulp_gpu_prepare` rebuilds
the pipelines and the IR's frequency-domain partitions, so the worker DRAINS everything in
flight before re-preparing — a re-prepare under a submitted block would read back audio
convolved with half of each kernel. Those drained blocks are misses, and a miss is exactly
what `MissPolicy::CpuFallback` exists for: the plugin's CPU convolver covers them, and it is
*also* crossfading to the new IR (its rebuild is time-sliced for precisely that reason). So
a Size move on the GPU engine is heard as the same smooth change the CPU engine gives.
Under "GPU only" those blocks are silent instead — which is what "no safety net" means, and
is why it is not the default.

## Measured numbers

`validate-gpu.mjs` has been run end-to-end, in a real-time `AudioContext` in real
headless Chrome on an Apple M-series GPU (`apple` / `metal-3`), against the real
`SuperConvolverGpu.wasm` (wasi-sdk WebCLAP) and the real `pulp-gpu-dsp.wasm`
(emdawnwebgpu). Exit 0. Verbatim, from the run:

```
ok  Run C — the CPU engine's impulse response is TIME-INVARIANT across the capture — relative RMS drift 0.00e+0 (tolerance 1e-4); pre-roll 24000 samples
ok  Run C — the CPU audio matches the float64 direct-convolution oracle — relative RMS error 2.04e-7 (tolerance 1e-5)
ok  Run A — the GPU-only engine produced audio (not silence) — marker at 25408, peak 0.508
ok  Run A — the GPU audio matches the float64 direct-convolution oracle — relative RMS error 1.71e-7 (tolerance 1e-5)
..  HEADLINE — the GPU-produced share of a 3 s real-time run — 312/314 = 99.4% (2 blocks the CPU net would have covered, had it been alive)
..  counters (diagnostic) — queueSubmits=313 mapResolves=312 expired=0 · queueSubmits >= produced: true
..  block time (diagnostic) — avg 2212 µs of a 10667 µs budget (20.7% of real time)
ok  Run B — the audio is Run A x 0.5, SAMPLE-WISE — max |b - 0.5a| = 0.00e+0 (floor 1.08e-4), rms ratio 0.5000
```

**The GPU path holds real time on this machine**, with the CPU net switched off:
312 of 314 blocks produced on the GPU over a 3-second real-time run, zero expiries,
at ~21% of the per-block wall-clock budget. The 2 blocks it did not produce are the
L = 2 START-UP blocks, whose wet cannot exist because nothing was pushed 2 blocks
before the stream began — they are misses by construction, not by failure (there is
no silence priming; see "Every block carries its sequence number"). Run B is the one that
matters most: editing the WGSL *text* — read back out of the shipped module and
rewritten in JavaScript — scaled the audible samples by exactly 0.5, bit-for-bit.
Nothing but a real WGSL compute shader on the real device can do that.

**What this is NOT.** It is not a claim that the GPU is faster than the CPU. It is
not. Run C shows the CPU `PartitionedConvolver` rendering the same convolution to
the same accuracy in the same real time, and the 2026-06-29 spike measured a
competent real-FFT CPU convolver beating or tying the GPU at every musically
plausible setting. This is a **capability and headroom** demonstration: the
convolution really is running as WebGPU compute shaders in a browser tab. CPU
remains the default and the always-available fallback.

**Not yet measured:** the miss-rate curve at `latencyBlocks = 1/2/3` (L=2 is still
an engineering guess — see `kWebGpuLatencyBlocks`), behaviour in a backgrounded
tab, and behaviour on a non-Apple GPU. Note what that means for the fixture's
headline: this run never entered the miss / drop / throttle regime, so it is NOT
evidence that the drop and expiry paths behave — those are proved by
`js/gpu-ring.test.mjs` (a real SAB, a stalled worker that forces drops, an
unpublished wet, an Engine-flip stretch) and by `test/test_super_convolver_web_gpu.cpp`
(the same scenarios through the real plugin, against a faithful C++ port of the
ring). `gpu_ns` is still always 0 — an honest
capability gap, not a stall: `GpuCompute` exposes timestamp-query timing only on
its `*_timed()` variants, every one of which resolves its query set with a
BLOCKING map, which is precisely what the async path exists to avoid.
