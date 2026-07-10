# Pulp Live Kernel — S0 spike + M1 live editor

Proves **"edit → crossfade → hear it"** inside ONE resident AudioWorklet wasm
instance running six real `core/signal` node classes. This is the S0 de-risking
spike for the in-browser DSP compiler effort
(`planning/2026-07-09-own-realtime-compiler-build-plan.md`). The go/no-go memo
with all measured numbers is `experimental/live_kernel/S0-RESULTS.md`.

The kernel is a **VM, not a compiler**: one prebuilt module interprets a bounded
binary graph blob by instantiating the actual C++ node classes. "Compile" =
validate + build plan; there is no per-edit codegen and nothing to tear down.

## M1 — "Pulp Live" editor (`editor.html`)

The demo built on top of S0 (design:
`planning/2026-07-09-live-compiler-demo-and-dsl-design.md`). A text editor beside
a running synth: you write a **patch-sheet DSL**, and because there is no compile
step, **every number in the source is a live control**. Drag `cutoff: 700hz` (or
type digits) while a riff plays and the filter sweeps under your finger — the
edit re-parses, diffs against the running graph, and lands as a `set_param` in
~3 ms with a `→ N ms` badge. Value edits are instant (`set_param`); structural
edits (a new node, `saw`→`sine`) crossfade click-free (40 ms equal-power) on
idle. Errors never silence — the last valid patch keeps playing with inline
squiggles.

The Pulp DSL: one line per module, positional audio args, named `param: value`
with unit literals (`1.2khz`, `180ms`, `-6db`, `7ct`), `~name` as the one-block
feedback cable, `out` is the graph output, `note.hz`/`note.gate` bind the mono
voice (main-thread, same `set_param` path — the kernel needs zero changes).

It is **fully static** — one HTML page + three scripts + the ~39 KB kernel wasm.
Single-thread, no `SharedArrayBuffer`, **no COOP/COEP**, so `crossOriginIsolated`
is `false` yet it runs and hot-swaps on plain static hosting (GitHub Pages /
Cloudflare Pages). Deploy with `./deploy.sh` (stages a flat, GitHub-Pages-ready
folder and pushes it to Cloudflare Pages with no headers).

## Layout
| File | Role |
|---|---|
| `../../../experimental/live_kernel/*.hpp`, `lk_entry.cpp` | the kernel (codec, registry, executor, crossfade, ABI) |
| `../../../experimental/live_kernel/aot_twin.cpp` | hand-fused AOT baseline / null-test oracle |
| `build.sh` | Emscripten build → `dist/lk_kernel.wasm`, `dist/aot_twin.wasm` (STANDALONE_WASM) |
| `lk-worklet.js` | classic AudioWorklet processor: byte-transfer + in-worklet sync-compile + resident kernel |
| `lk-driver.mjs`, `index.html` | S0 main-thread driver page (start audio, param edit, structural swap) |
| `lk-patches.mjs` | S0 shared `LKB0` blob encoder + the three test patches |
| `editor.html`, `editor.mjs` | **M1 live editor**: scrubbable-number editor + running synth + keyboard + receipts |
| `lk-dsl.mjs` | the Pulp DSL — parser, unit literals, `~` feedback, LKB0 lowering, diff, presets |
| `deploy.sh` | stage a flat GitHub-Pages-ready site + deploy to Cloudflare Pages (no COOP/COEP) |
| `serve.mjs` | static dev server (no COOP/COEP needed — single-thread build) |
| `measure.mjs` | offline harness: null test, CPU vs AOT, zero-alloc, plan-build, click-free |
| `validate.mjs` | S0 headless-Chrome (CDP) validator: real-time + edit→sound latency |
| `validate-editor.mjs` | M1 headless validator: scrub→param audible + fast, structural crossfade, no-COI |
| `f2-emitter.js` | **F2-S1 graph→wasm emitter** (classic script, no deps): LKB0 → fused straight-line wasm module, bit-exact via the kernel's `f2_*` libm bridge |
| `f2-measure.mjs` | F2 offline harness: per-patch bit-exact null (vs twins AND interpreter), CPU multiplier vs the ≥1.5× gate, handoff click metric, emit cost |
| `f2-validate.mjs` | F2 headless validator: in-worklet emit + compile, equal-power handoff both ways, real-time in the compiled tier |

## F2 — compiled "stable" tier (`f2-emitter.js`)

When a graph stops changing, the worklet can compile it: `f2Compile` runs the
emitter **inside the worklet** (pure JS, ~0.06 ms emit + ~0.02 ms sync-compile
for the 10-node musical patch, module ≈ 1.3 KB), then equal-power-crossfades
interpreter→compiled. Any live edit falls back instantly (the interpreter still
holds the editable plan) and fades back. Output is **bit-exact** to both the
interpreter and the hand-fused AOT twins (all conformance nodes, maxAbsDiff = 0)
because every rational f32 op mirrors the C++ source order and every
transcendental routes through the kernel's `f2_*` musl exports. Verdict data vs
the ≥1.5× speedup gate lives in
`planning/2026-07-09-f2-graph-wasm-emitter-design.md`.

## Run
```
source ~/Code/emsdk/emsdk_env.sh
bash build.sh
node measure.mjs      # headless numbers (no browser)
node validate.mjs     # headless system Chrome
```
Or open interactively: `node serve.mjs` then
`http://localhost:8793/examples/web-demos/live-kernel-spike/` — click **Start
audio**, then **Param edit** / **Structural swap** and listen.

### M1 editor
```
source ~/Code/emsdk/emsdk_env.sh
bash build.sh
node validate-editor.mjs   # headless: scrub→param, structural morph, no-COI
node serve.mjs             # then open editor.html and play:
#   http://localhost:8793/examples/web-demos/live-kernel-spike/editor.html
./deploy.sh                # stage flat site + deploy to Cloudflare Pages (no headers)
```
Hit **LATCH** (or hold a key), then **drag a highlighted number** and hear it.

`dist/` is git-ignored; rebuild with `build.sh`.
