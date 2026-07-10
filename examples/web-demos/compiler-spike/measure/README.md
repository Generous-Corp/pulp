# Pulp Live — measurement / harness rig

The **independent tooling lane** (agent #2) of the own-real-time-compiler build
plan (`planning/2026-07-09-own-realtime-compiler-build-plan.md`, §9 task 2 and
§10). It does **not** touch the kernel architecture, so it is built before the S0
spike and provides the measured go/no-go gates every later phase needs.

Four headless harnesses, each printing a `PASS:`/`FAIL:` measured-value line and
exiting non-zero on failure, so they run in CI unchanged:

| # | Harness | Gate it backs (S0 acceptance) |
|---|---------|-------------------------------|
| 1 | `harness1-aot-baseline.mjs` | the **AOT CPU reference** the "≤ 2× AOT" gate compares against (#2) |
| 2 | `harness2-cpu.mjs` | generic per-`process()` CPU rig + **interpreter/AOT ratio ≤ 2×** gate (#2) |
| 3 | `harness3-nulltest.mjs` | **interpreter matches AOT to ≤ −60 dBFS** correctness null test (#4) |
| 4 | `harness4-edit-latency.mjs` | **edit→sound ≤ 30 ms** (#3) |

Nothing here overlaps the WS-C2 `wclap-build/` dirs or the live demo sites: it
lives entirely under `examples/web-demos/compiler-spike/measure/` and only
*reads* built wasm modules and the shipped `core/format/src/wasm/wam-runtime.mjs`.

## Why it is reusable by the S0 kernel spike

Every harness is written against a tiny **DSP adapter contract**
(`lib/dsp-adapter.mjs`): an object exposing `inBuf` / `outBuf` typed arrays,
`processQuantum()`, and optional `setParam` / `midi`. The AOT WAM plugin, the
future kernel-VM wasm, and the future emitted-wasm module all satisfy it. To get
gate numbers for the kernel, write a `loadLiveKernel(bytes, opts)` that returns
that same shape and pass its wasm to `--a`/`--b`/`--wasm`. No harness changes.

The measurement math (`lib/measure-core.mjs`) is pure JS with no Node/browser
APIs, so it runs identically in Node and in a headless-Chrome page.

## Two execution engines

- `--engine chrome` (**default**): launches **system Chrome** headless via
  `playwright-core`, serves the tree, and runs the measurement in the page — V8,
  the same engine the AudioWorklet uses, so CPU numbers are representative.
- `--engine node`: the deterministic filesystem fallback (no browser). Same
  `measure-core` math; handy for CI and quick local numbers. Harness 4 is
  Chrome-only (it needs a real `AudioContext`).

The two engines agree to <5% on CPU and bit-exactly on the null test.

## Prerequisites

```sh
# 1. Build a WAM plugin to measure (emsdk 6.0.2, the existing WAM path).
cd examples/web-demos/wasm-build
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DPULP_WAM_CHOC_INCLUDE=/path/to/choc         # dir containing choc/
cmake --build build --target PulpGain-wam PulpPluck-wam -j4
# → build/PulpGain.wasm, build/PulpPluck.wasm  (export the wam_* C ABI)

# 2. Install the headless-Chrome driver.
cd ../compiler-spike/measure
npm install            # playwright-core only; uses your system Chrome
```

## Running

```sh
cd examples/web-demos/compiler-spike/measure
G=../../wasm-build/build/PulpGain.wasm
P=../../wasm-build/build/PulpPluck.wasm

# 1 — AOT baseline (µs/quantum + % of the 128-frame budget @ 48 kHz)
node harness1-aot-baseline.mjs --wasm $G
node harness1-aot-baseline.mjs --wasm $P --midi-note 60 --settle 32   # synth

# 2 — CPU ratio (pass AOT to --a, the interpreter to --b)
node harness2-cpu.mjs --a $G --b $P --midi-note-b 60 --ratio-max 2.0

# 3 — null test. Self-check FIRST (validates the meter), then real A-vs-B:
node harness3-nulltest.mjs --self-check --a $G
node harness3-nulltest.mjs --a $AOT.wasm --b $INTERP.wasm --gate-db -60

# 4 — edit→sound latency (Chrome only)
node harness4-edit-latency.mjs --wasm $G --trials 10 --gate-ms 30
```

`npm run selfcheck` runs the meter self-check (node engine) as a zero-setup smoke
test once the wasm is built.

## How each gate is measured

### 1 & 2 — CPU (`measureCpu`, `lib/measure-core.mjs`)
Fill `inBuf` once, warm the JIT (`--warmup`, default 2000 quanta), then time
`--quanta` (default 20000) calls of `processQuantum()` per trial across `--trials`
(default 5) with `performance.now()`. Report **median** (headline) and **min**
(least-noisy true cost) µs/quantum. The 128-frame budget at 48 kHz is
128/48000 s = **2666.7 µs**; `budget% = µs/quantum ÷ 2666.7 × 100`. Harness 2
divides candidate/AOT to get the ratio and applies `--ratio-max` (default 2.0).

### 3 — Null test (`nullTest`, `lib/measure-core.mjs`)
Render the **same** interleaved sine through two adapters (or two param configs)
block-by-block (deterministic → bit-identical when configs match), then report:
- **residual** — level of the difference signal `A−B` in dBFS. This is the null
  test proper (backs ≤ −60 dBFS). Two identical renders → **−inf**.
- **level delta** — `level(B) − level(A)` in dB, a sanity meter (+6 dB gain → +6 dB).

`--self-check` proves the meter before it is trusted: an *identical* render must
give residual −inf and 0 dB delta; a *+6 dB output-gain* diff must give a +6 dB
level delta. `--max-shift N` enables an integer-sample best-alignment search for
comparing modules that differ in reported latency.

### 4 — Edit→sound latency (`page/latency.html` + `worklet/latency-worklet.js`)
A live graph: `OscillatorNode → [WAM DSP in a classic-script AudioWorklet] →
AnalyserNode → destination`. The worklet compiles the wasm the proven way — raw
**bytes transferred in, `new WebAssembly.Module` synchronously in the worklet**
(never a postMessage'd `Module`; DECISION.md §3) — which is the same
teardown-free path the kernel VM will hot-swap through. Two edits, N trials each:
- **param** — drop input gain to silence, then time (wall-clock) from posting the
  gain-up param to the analyser crossing the audible threshold. Detection uses a
  sub-millisecond `MessageChannel` self-yield loop (rAF's 16 ms is too coarse).
- **structural swap (stub)** — transfer fresh wasm bytes, sync-compile a new slot
  in the worklet, time swap-post→audible. Exercises the real swap mechanism.

Also reported: the **audio-clock apply→audible** delta the worklet observes
internally (≈ one quantum = the in-buffer floor).

## Measured baseline (this checkout, M-class Mac, Chrome, 48 kHz / 128)

| Module | Workload | µs/quantum (median) | % of budget |
|--------|----------|--------------------:|------------:|
| PulpGain  | unity gain (trivial node) | ~0.28 | ~0.01% |
| PulpPluck | 1-voice plucked synth     | ~0.82 | ~0.03% |
| PulpPluck | 8-voice chord             | ~0.80 | ~0.03% |

- **Null self-check:** identical → **−inf dBFS** residual; +6 dB → **+6.00 dB**
  level delta. Meter validated.
- **Edit→sound:** param edit **~5 ms** median, structural swap **~4 ms** median;
  audio-clock apply→audible floor **2.67 ms** (exactly one 128-frame quantum).

These are AOT reference numbers for single plugins. The kernel VM's ≤ 2× gate is
applied against the AOT build of the *same graph* the interpreter runs — drop its
wasm into `--a`/`--b` when S0 produces it.

## Files

```
measure/
  lib/measure-core.mjs   pure-JS gate math (CPU, null test, render) — Node + browser
  lib/dsp-adapter.mjs    the adapter contract + WAM loader (reuses wam-runtime.mjs)
  lib/node-engine.mjs    --engine node surface (mirrors page/measure.html)
  lib/serve.mjs          static server rooted at repo root (COOP/COEP)
  lib/harness-util.mjs   CLI args, Chrome discovery, headless launch
  lib/fmt.mjs            output formatters
  page/measure.html      in-Chrome CPU / null-test surface (window.MeasureAPI)
  page/latency.html      live oscillator→worklet→analyser (window.LatencyAPI)
  worklet/latency-worklet.js  classic-script AudioWorklet hosting the WAM DSP
  harness1-aot-baseline.mjs  harness2-cpu.mjs  harness3-nulltest.mjs  harness4-edit-latency.mjs
```
