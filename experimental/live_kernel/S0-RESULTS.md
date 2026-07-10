# Pulp Live Kernel — S0 spike — go/no-go memo

**Date:** 2026-07-09
**Author:** S0 kernel-spike agent (lane 1)
**Scope:** Prove "edit → crossfade → hear it" inside ONE resident AudioWorklet
wasm instance with 6 real `core/signal` nodes, and report measured numbers.
**Source of truth:** `planning/2026-07-09-own-realtime-compiler-build-plan.md`
§S0 (with Codex's alignment corrections, which win).

## Verdict: **GO.** The kernel-VM approach clears the S0 bar.

Every S0 acceptance gate passes, most with large margin. The representative
patch runs at **1.17× AOT** (beats the ≤1.5× *target*, well under the ≤2.0× go
line). The single number that grazes a threshold is the *deliberately
pathological* worst case (osc + 9 unity gains), which hovers at **2.9–3.1×** — at
**0.03 % of one core** absolute, and exactly the interpreter cliff the plan
predicts (R2) and that Option 2 (the emitter) exists to remove. It does not gate
the decision. Recommendation: **proceed to M1**, and keep the interpreter as the
permanent reference/fallback.

---

## What was built (this lane)

A resident **kernel VM**, not a compiler: one prebuilt wasm module that
INTERPRETS a bounded binary graph blob by instantiating the ACTUAL C++ node
classes.

- `experimental/live_kernel/`
  - `codec.hpp` — bounded binary IR v0 (`LKB0`): magic + node/edge/param tables,
    fixed-stride, byte-wise, bounds-checked, **zero-alloc** decode into inline
    `PlanDesc` storage. Explicit maxima: 64 nodes / 128 edges / 255 params /
    4096 bytes.
  - `registry.hpp` — the 6 nodes behind a uniform init/set_param/process surface,
    instantiating the real headers: `OscillatorT`, `GainT`+`SimpleMixerT`,
    `BiquadT`, `LadderFilterT`, `AdsrT` (as a gated VCA), `DelayLineT`.
  - `executor.hpp` — a tiny block executor: topo-sort (feedback edges excluded),
    then per 128-frame block gather→dispatch→capture, mirroring
    `GraphRuntimeExecutor::process_routed()` (bus-copy | summed gather;
    one-block `feedback_prev`). All storage preallocated inline; the `DelayLineT`
    pool is `prepare()`d ONCE at kernel init (off the steady-state audio path).
  - `crossfade.hpp` — two preallocated Plan slots + equal-power (cos/sin)
    crossfade, mirroring native `LiveSwapCurve::EqualPower`. Param-only edits →
    `set_param` (zero interruption); structural edits → decode+build into the
    inactive slot then `swap(fade_ms)`.
  - `lk_entry.cpp` — the `lk_*` C ABI (separate from the shipped `wam_*` ABI) +
    the zero-alloc instrument (replaced global `operator new` bumps a counter
    read by `lk_alloc_count()`).
  - `aot_twin.cpp` — hand-fused straight-line compiled equivalents of the three
    patches (reusing the same registry setup), the CPU baseline + null-test
    oracle.
- `examples/web-demos/live-kernel-spike/` — `build.sh` (Emscripten →
  STANDALONE_WASM), `lk-worklet.js` (classic, byte-transfer + sync-compile),
  `lk-driver.mjs` + `index.html` (main-thread driver page), `lk-patches.mjs`
  (shared blobs + encoder), `serve.mjs`, `measure.mjs` (offline harness),
  `validate.mjs` (dependency-free headless-Chrome CDP validator).
- `tools/cmake/PulpLiveKernel.cmake` — productionized build helper (cloned from
  `PulpWam.cmake`); does not touch the `wam_*` ABI.

Kernel wasm size: **~39 KB** (`lk_kernel.wasm`, `-O3`, 6 nodes).

---

## Measured results

### Environment
- emsdk **6.0.2** (pinned), `-O3 -fno-exceptions -fno-rtti`, single-thread
  STANDALONE_WASM (no threads, no SharedArrayBuffer, **no COOP/COEP**).
- Offline harness: Node **v26** V8 wasm engine (same engine the browser uses).
- Browser harness: **headless system Google Chrome**, `--autoplay-policy=
  no-user-gesture-required`, driven over the DevTools Protocol.
- 48 kHz, 128-frame quantum.

### 1. Real-time in the worklet (audio-clock-locked) — **PASS**
Headless Chrome, resident `lk-processor` rendering the 10-node musical graph:
- Δquanta ≈ 475–494 over ~1.30 s wall; **quanta/wall ratio 0.97–1.01** (gate
  ≥ 0.95). Quanta track the *AudioContext* clock at ≈ **0.998** (ctxΔ), i.e.
  audio-clock-locked, not offline/free-running. (The ratio-vs-wall dips below 1.0
  only because muted headless Chrome advances its audio clock slightly slower
  than wall — the audio clock is the ground truth and quanta follow it.)

### 2. Edit → crossfade → hear it — **PASS**
Both edit paths take effect audibly under a click-free crossfade, measured in
headless Chrome (main-thread wall-clock round-trip = an upper bound on true
edit→sound):
- **Param edit** (ladder cutoff): median **~3 ms**, max **~5 ms**.
- **Structural swap** (musical ⇄ drone: different node count + topology + tuning,
  40 ms equal-power fade): median **~4 ms**, max **~5.4 ms**; output RMS shifts
  audibly (Δrms ≈ 0.006–0.009).
- **Both well under the ≤ 30 ms gate** (≈ 6× margin).

### 3. CPU vs AOT on three patches — **PASS (representative)**
Ratio = kernel-VM time / hand-fused AOT twin time (same V8 wasm engine), min of
8 reps × 8000 blocks:

| Patch | kernel/AOT | kernel % of realtime |
|---|---|---|
| **musical (~10-node chain)** — the representative patch | **1.17×** (stable) | 0.38 % |
| trivial worst case (osc + 9 unity gains) | **2.9–3.1×** (noisy) | 0.03 % |
| legal one-block feedback/delay | **1.8×** | 0.03 % |

- Representative patch **1.17×** → **passes the go line (≤ 2.0×) and beats the
  target (≤ 1.5×).**
- The trivial micro-chain grazes the 3× kill line. It is the documented
  interpreter cliff (per-node buffer traffic + no cross-node fusion, R2), at
  negligible absolute cost, and is precisely what Option 2 addresses. Not a
  realistic patch; does not gate the go decision.

### 4. Null test — **PASS (bit-exact)**
Kernel-VM musical render vs the AOT twin over 2 s: **maxAbsDiff = 0**, residual
**−∞ dBFS** (≤ −60 gate). The block-per-node interpreter and the fused AOT build
produce bit-identical output — they run the *same* `core/signal` classes, so the
interpreter's routing/buffering introduces zero error.

### 5. Click-free crossfade — **PASS**
Recorded a real 50 ms equal-power swap between two smooth sine patches; a click is
an impulsive discontinuity, detected as a spike in the second difference
(discrete Laplacian):
- 50 ms fade: **−64.4 dBFS** (≤ −60), ≈ the signal's own curvature
  (steady −65.0 dBFS; +0.6 dB, i.e. no injected impulse).
- Instant-cut control (fade = 0): **−11.7 dBFS** — the fade is **52.7 dB** below
  a hard cut. Endpoint continuity is exact by construction (equal-power endpoints
  cos 0 = 1, sin 0 = 0).

### 6. Zero allocation in `process()` / swap-fade — **PASS**
`lk_alloc_count()` (replaced global `operator new`) delta:
- 20 000 render blocks: **Δ = 0**.
- load_plan + 50 ms equal-power swap-fade rendering: **Δ = 0**.
- Total allocations since init: **16** — the two Plans' 8+8 `DelayLineT` rings,
  all at kernel construction (off the steady-state audio path), never per edit.

### 7. Plan-build on the audio thread (R1) — **PASS, with margin to spare**
`port.onmessage` plan-build runs on the render thread; the F0.4 budget is one
quantum (**2667 µs**). Measured (Node, precise; the worklet global scope has no
`performance.now`, so timed offline in the same V8 wasm engine, and corroborated
by the real-time ratio holding through 4 live swaps):
- 10-node musical: **1.2 µs**. 64-node chain: **2.3 µs**.
- **~1000× under budget.** Because all storage is preallocated and the delay pool
  is pre-prepared, a structural edit is just decode + topo-sort + placement
  resets + param apply — no allocation, no re-`prepare()`.

---

## Kill/pivot criteria (from the plan) — none tripped
- "CPU > 3× AOT **on the chain**" — the representative chain is 1.17×; only the
  pathological trivial micro-chain hovers at 3× (negligible absolute).
- "plan-build cannot be made glitch-free" — plan-build is 2.3 µs / 64 nodes,
  ~1000× under one quantum; **R1 is effectively retired for this node scale.**

## D3 (executor provenance) — costed recommendation
**Recommend the fresh minimal block executor (default), not compiling
`SignalGraph`+`GraphRuntimeExecutor` to wasm.** Evidence: the whole 6-node kernel
is **~39 KB** and single-thread, with **bit-exact** parity to the fused twin and
plan-build in microseconds. `core/host` drags plugin slots, anticipation lanes,
triple buffers, and `std::thread`/mutex — none of which builds cleanly or belongs
in a single-thread worklet, for a routing surface the null test already proves
equivalent. Keep conformance-testing the browser executor against native renders
(R3) rather than porting the native executor in.

## What works vs what is stubbed
**Works (measured):** resident single-instance kernel; 6 real node classes;
bounded zero-alloc binary decode; topo-sort with one-block feedback; equal-power
dual-plan crossfade; param + structural edits audible in headless Chrome;
real-time; bit-exact null; provable zero-alloc; microsecond plan-build; byte-
transfer + in-worklet sync-compile of a STANDALONE_WASM module.

**Stubbed / out of S0 scope (deferred to M1+):** mono only, no MIDI/voice pool;
6 nodes (M1 grows to ~20); no DSL front-end (S1) and no wasm emitter (F2); a
`load_plan` during an in-flight fade is refused (busy) rather than queued;
param-during-structural-fade is not a supported interaction; the `LKB0` v0 param
table caps at 255 entries; three-browser + mobile matrix is an M1 gate (only
headless desktop Chrome measured here).

## Reproduce
```
source ~/Code/emsdk/emsdk_env.sh
bash examples/web-demos/live-kernel-spike/build.sh   # -> dist/*.wasm
node examples/web-demos/live-kernel-spike/measure.mjs # null / CPU / zero-alloc / plan-build / click-free
node examples/web-demos/live-kernel-spike/validate.mjs # headless Chrome: real-time + edit->sound latency
```

---

## Iteration 2 addendum (node breadth + live signal-flow graph)

Built on top of the S0 kernel, additive only (no `wam_*` ABI touched). Same
grammar — only vocabulary and a level readout grew.

**Node breadth 6 → 14 wire types.** New `core/signal` classes registered behind
the same init/set_param/process surface, each default-constructible + alloc-safe
(scalar) or pool-backed (buffer-owning), each given DSL verbs:

| New node | Class | Verbs | RT-alloc strategy |
|---|---|---|---|
| Svf | `SvfT` | `svf` | inline (scalar) |
| Shaper | `WaveShaperT` | `shape` `fold` `clip` | inline (scalar) |
| DcBlock | `DcBlocker` | `dcblock` | inline (scalar) |
| Noise | kernel-local `NoiseGen` | `noise` | inline (scalar) |
| Chorus | `ChorusT` | `chorus` | pool (pre-`prepare()`d ×4) |
| Reverb | `ReverbT` | `reverb` | pool (pre-`prepare()`d ×2) |
| Comp | `CompressorT` | `comp` | inline (0 lookahead → no heap) |

Chorus/Reverb are stereo classes collapsed to mono (`0.5·(L+R)`); their delay
buffers are pooled and `prepare()`d once at kernel init, so a structural edit
that adds one stays zero-alloc.

**Per-node level tap (the signal-flow graph).** `render_block` writes each node's
output mean-square into a preallocated `float[LK_MAX_NODES]`; `lk_node_levels()`
copies it out (sqrt on read). The worklet posts it on the existing ~20 Hz meter
channel; the demo renders a live node graph that glows with the audio. The tap is
alloc-free and is gated off in measurement mode (`lk_set_meter(0)`) so CPU
numbers stay clean.

**Conformance (`conformance.mjs`).** Every one of the 14 node types is rendered
through the full DSL→encode→decode→executor path and null-tested against a
hand-wired AOT twin that bypasses the executor: **maxAbsDiff = 0 for all 17
node/variant cases** (residual −∞ dBFS, gate ≤ −60). All 7 shipped presets parse
+ encode cleanly.

**Re-measured (meter gated off), all S0 gates still pass:**

| Gate | S0 | Iteration 2 |
|---|---|---|
| null test (musical) | −∞ dBFS | −∞ dBFS |
| CPU musical (representative) | 1.17× | 1.17× |
| CPU trivial (pathological) | 2.9–3.1× | 2.80× |
| CPU feedback | 1.8× | 1.65× |
| plan-build (64 nodes) | 2.3 µs | 2.5 µs |
| zero-alloc (process / swap) | 0 / 0 | 0 / 0 |
| click-free (50 ms fade) | −64.4 dBFS | −64.4 dBFS |

Kernel size grew ~39 KB → **~48 KB** for the 8 extra node classes.

Headless editor (system Chrome, no COOP/COEP): scrub param latency **~2.4 ms
median**, `crossOriginIsolated === false`, live graph shows 10 lit nodes on
LushPad, `lk_alloc_count` delta **0** across a burst of scrubs + a structural
swap.

**Design challenge deferred to Fable — FauxFM / oscillator `pm:`.** True FM needs
an audio-rate phase/pitch input on the oscillator, but `OscillatorT::next()`
exposes no external phase term, and adding one would (a) reimplement the
oscillator's polyBLEP DSP in the kernel rather than reusing the shipped class
(breaking the bit-exact-to-native guarantee), and (b) make the oscillator's port
arity dynamic (0 inputs normally, 1 when `pm:` is wired), which the static
`ports_for()` model doesn't express. Left gated per the original design. LushPad
(the other gated preset) is now unlocked and shipping.

**Reproduce (iteration 2):**
```
node examples/web-demos/live-kernel-spike/conformance.mjs    # per-node bit-exact null test
node examples/web-demos/live-kernel-spike/validate-editor.mjs # headless: scrub + graph + zero-alloc
```
