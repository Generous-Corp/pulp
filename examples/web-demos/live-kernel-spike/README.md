# Pulp Live Kernel — S0 spike

Proves **"edit → crossfade → hear it"** inside ONE resident AudioWorklet wasm
instance running six real `core/signal` node classes. This is the S0 de-risking
spike for the in-browser DSP compiler effort
(`planning/2026-07-09-own-realtime-compiler-build-plan.md`). The go/no-go memo
with all measured numbers is `experimental/live_kernel/S0-RESULTS.md`.

The kernel is a **VM, not a compiler**: one prebuilt module interprets a bounded
binary graph blob by instantiating the actual C++ node classes. "Compile" =
validate + build plan; there is no per-edit codegen and nothing to tear down.

## Layout
| File | Role |
|---|---|
| `../../../experimental/live_kernel/*.hpp`, `lk_entry.cpp` | the kernel (codec, registry, executor, crossfade, ABI) |
| `../../../experimental/live_kernel/aot_twin.cpp` | hand-fused AOT baseline / null-test oracle |
| `build.sh` | Emscripten build → `dist/lk_kernel.wasm`, `dist/aot_twin.wasm` (STANDALONE_WASM) |
| `lk-worklet.js` | classic AudioWorklet processor: byte-transfer + in-worklet sync-compile + resident kernel |
| `lk-driver.mjs`, `index.html` | main-thread driver page (start audio, param edit, structural swap) |
| `lk-patches.mjs` | shared `LKB0` blob encoder + the three test patches |
| `serve.mjs` | static dev server (no COOP/COEP needed — single-thread build) |
| `measure.mjs` | offline harness: null test, CPU vs AOT, zero-alloc, plan-build, click-free |
| `validate.mjs` | dependency-free headless-Chrome (CDP) validator: real-time + edit→sound latency |

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

`dist/` is git-ignored; rebuild with `build.sh`.
