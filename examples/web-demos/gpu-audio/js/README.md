# Browser GPU-audio transport (JS side)

The JS half of running Pulp's GPU-audio compute in a browser tab: WGSL compute
shaders in a `DedicatedWorker`, feeding an `AudioWorklet` through a
`SharedArrayBuffer`.

**This is a capability demonstration, not a speed claim.** A measured spike
(`planning/2026-06-29-superconvolver-irreducible-rooms-and-multi-ir.md`) showed a
competent real-FFT CPU convolver beats or ties the GPU at every musically
plausible setting. Nothing here claims otherwise. The CPU engine stays the
default and stays a working fallback; a missed GPU deadline is a **normal**
outcome that the plugin's CPU path covers, and if the browser GPU path cannot
hold real time on a given machine, the honest thing to do is say so in the UI.

## Topology (and why it is shaped this way)

An `AudioWorkletProcessor` **cannot** touch `navigator.gpu` — WebGPU is exposed
on `Window` and `DedicatedWorker` only — and it cannot spawn a `Worker`. So the
main thread owns both agents and introduces them by handing the **same**
SharedArrayBuffer to each:

```
 main thread   gpu-bridge.mjs   probe → allocate+prime ring → new Worker(sab)
                                → capability handshake → new AudioWorkletNode(
                                     …, { processorOptions: { gpuSab, gpuLatencyBlocks } })
 worker        gpu-worker.mjs   navigator.gpu → adapter/device → wasm GPU DSP
                                → drain IN ring → submit → poll → fill OUT ring
 audio thread  wclap-processor.js (inlines gpu-ring.mjs)
                                env.pulp_gpu_xfer → ring.push / ring.pop
```

A SAB requires cross-origin isolation (`COOP: same-origin` + `COEP:
require-corp`). The WebCLAP Cloudflare host already sets both
(`examples/web-demos/wclap-build/cloudflare/_headers`).

## The rules that are not negotiable

- **The audio thread never blocks.** `push` on a full input ring DROPS the block;
  `pop` on an empty output ring returns 0 (a miss). No `Atomics.wait`, no
  `postMessage`, no allocation on that path.
- **The worker never calls `Atomics.wait` either** — even though it is legal in a
  DedicatedWorker. WebGPU's `mapAsync` callback only resolves when the worker's
  **event loop turns**; `Atomics.wait` blocks the thread and starves the very
  loop that would resolve the map. That is the browser form of the self-deadlock
  the native blocking readback becomes if ported naively. Everything is
  `Atomics.waitAsync` / promises / macrotasks.
- **Poll several times per block.** The readback poll judges each block's deadline
  at poll *entry*, so a once-per-block poll would systematically discard blocks
  that actually completed and make the miss rate look far worse than the GPU is.
- **Expiry routes to the MissPolicy, never to a hang.** A block whose deadline
  passes is abandoned: the output cursor is not advanced, `expired` increments,
  the worklet finds the output empty, misses, and the plugin's CPU net covers it.
- **Device loss is normal behavior**, not a fatal init error: flag it, stop
  producing, keep missing, stay on CPU forever.

## SAB layout v1 (`PGR1`)

`CH=2`, `B=512`, `R=4` slots, `L=latencyBlocks` (default 2). Total ≈ 33 KB.

| bytes | contents |
|---|---|
| `[0,256)` | ctrl `Int32Array(64)` |
| `[256,384)` | stats `Float64Array(16)` |
| `[384, 384+R*CH*B*4)` | IN ring, planar per slot: `[ch0 B][ch1 B]` |
| next `R*CH*B*4` | OUT ring, same shape |

**ctrl** — `[0]` magic `0x50475231`, `[1]` flags (bit0 worker_ready, bit1
device_lost, bit2 shutdown), `[2]` block_size, `[3]` channels, `[4]` R, `[5]` L,
`[6]` version, `[7]` stats seqlock (odd = writer inside), `[16]` IN_WRITE
(producer: worklet), `[24]` IN_READ (consumer: worker), `[32]` OUT_WRITE
(producer: worker), `[40]` OUT_READ (consumer: worklet), `[48]`/`[52]`/`[56]`
worklet-owned miss / input_dropped / resynced counters. Lanes are spaced to keep
the hot cursors off each other's cache lines. Cursors are MONOTONIC uint32
counts: `slot = idx % R`, `depth = (write - read) >>> 0` — never wrapped by hand.

**stats** (worker-written under the ctrl[7] seqlock, page-read at ~10 Hz) —
`[0]` produced_blocks, `[1]` miss_blocks, `[2]` input_dropped, `[3]` resynced,
`[4]` expired, `[5]` last_block_us, `[6]` avg_block_us, `[7]` gpu_ns_last,
`[8]` queue_submits, `[9]` map_resolves, `[10]` state (0 init, 1 ready,
2 device-lost, 3 failed). The three worklet-owned counters live in atomic ctrl
lanes (the audio thread cannot participate in a single-writer seqlock) and the
worker mirrors them into the stats block so the page reads one coherent snapshot.

`last_block_us` / `avg_block_us` are measured **submit → readback-resolve** — the
honest round trip, the same span native's `Stats::last_block_us` measures.
Reporting submit time as if it were the round trip would be a lie.

## Index discipline (mirrors `GpuAudioTransport` exactly)

- **Priming.** At prepare: `IN_WRITE = IN_READ = OUT_READ = 0`, **`OUT_WRITE =
  L`**, OUT slots `0..L-1` zeroed. Block *n* popped by the worklet is primed
  silence for *n < L* and the wet of IN block *n−L* thereafter. Latency is
  **exactly `L * B` samples**, fixed for the prepared lifetime, with zero
  per-block bookkeeping. This is the literal browser translation of native's
  "prime the output ring with `latency_blocks` of silence".
- **Resync debt — NOT optional.** On a miss the worklet increments `blocksOwed`.
  On a later pop it first drains that debt (dropping at most `avail-1` blocks so a
  resync can never starve the read). Without it, every miss permanently adds one
  block of effective latency and comb-filters the plugin's dry against its wet.
  It is exactly native's `blocks_owed_` / `resynced_blocks_`.

## Worklet seam

`packages/pulp-web-player/src/vendor/pulp-wasm/wclap-processor.js` inlines
`gpu-ring.mjs` verbatim (an `addModule()` worklet is a classic script and cannot
`import`; `packages/pulp-web-player/test/gpu-ring-parity.test.mjs` asserts the two
copies are byte-identical) and provides one wasm import:

```
pulp_gpu_xfer(inPtr, outPtr, frames) -> int
```

Reads `2*frames` planar f32 from the wasm heap at `inPtr` and pushes them to the
IN ring; pops the OUT ring into `outPtr`; returns 1 = wet delivered, 0 = miss.
Synchronous, allocation-free, non-blocking, no postMessage. Returns 0 immediately
if `frames !== blockSize` or no SAB was passed. With no `gpuSab` in
`processorOptions` the whole path is **inert** — every other demo on this player
is unaffected.

GPU stats ride the existing throttled `meter` message (20 Hz); there is no new
per-quantum postMessage traffic.

## GPU-module contract (the wasm DSP module the worker loads)

The worker expects an ES-module Emscripten factory whose instance exposes either
a `Module.pulpGpu` object or the bare `_pulp_gpu_*` C exports:

| member | meaning |
|---|---|
| `init()` → `Promise<bool>` | adapter/device bring-up, shader + pipeline creation |
| `prepare(sampleRate, blockSize, channels, irPtr, irFrames, irChannels)` → `Promise<bool>` | (re)build the convolution state |
| `inBuffer()` / `outBuffer()` → f32 heap ptr | `channels*blockSize` planar floats each |
| `submit(seq, deadlineMs)` → `1`/`0` | enqueue the block currently in `inBuffer()` |
| `poll()` | drain completed readbacks; calls `onBlockDone(seq, outPtr, gpuNs)` |
| `stat(which)` → number | queue_submits / map_resolves / gpu_ns_last |
| `onBlockDone` | set by the worker |

The module is constructed with `preinitializedWebGPUDevice` so it adopts the
device the worker created (and whose `lost` promise the worker watches) rather
than requesting its own. It must be a **DSP-only** build: no Skia.

## Capability handshake

`gpu-bridge.mjs` does **not** gate on main-thread `navigator.gpu` (Window and
DedicatedWorker can differ). It spawns the worker and makes it do the full
bring-up — `requestAdapter` → `requestDevice` → instantiate → `pulp_gpu_init` →
`pulp_gpu_prepare` with a **unit-impulse IR** → submit one known block → poll
until it lands → verify the returned samples equal the input (impulse convolution
is the identity). Only on `{ ok: true }` should the page load the GPU-capable
plugin and expose the Engine toggle.

Every failure is a distinct named reason, surfaced verbatim to the UI:
`not-cross-origin-isolated`, `no-sab`, `no-navigator-gpu-in-worker`, `no-adapter`,
`no-device`, `shader-compile-failed:<msg>`, `pipeline-failed:<msg>`,
`selftest-mismatch`, `worker-timeout`, `device-lost:<reason>`. The handshake also
reports `maxComputeWorkgroupSizeX`, `maxComputeInvocationsPerWorkgroup`,
`maxComputeWorkgroupStorageSize`, `maxStorageBufferBindingSize`, `maxBufferSize`
and whether `timestamp-query` is available. A pipeline that exceeds a device cap
must be a **named refusal**, not a silent shrink.

Poll the stats with `setInterval` at ~10 Hz — **not** `requestAnimationFrame`:
rAF is throttled in a background tab, and the readout would freeze exactly when
misses are most interesting.

## Tests

```bash
node --test examples/web-demos/gpu-audio/js/gpu-ring.test.mjs   # priming, ordering, miss+resync, overflow, no-wait, cross-thread SAB
node packages/pulp-web-player/test/gpu-ring-parity.test.mjs     # inline drift, inertness, the real pulp_gpu_xfer path
```

Both are browser-free and GPU-free: they run on any Node (and on `ubuntu-latest`).
