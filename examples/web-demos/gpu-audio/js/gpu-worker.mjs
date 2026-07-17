// gpu-worker.mjs — the DedicatedWorker that owns WebGPU and runs the GPU DSP.
//
// WebGPU is exposed on Window and DedicatedWorker only, and an
// AudioWorkletProcessor can neither reach navigator.gpu nor spawn a Worker — so
// the GPU work lives HERE, and the audio thread reaches it only through the
// SharedArrayBuffer rings in ./gpu-ring.mjs.
//
// *** THE ONE INVERSION THAT MATTERS ***
// This worker NEVER calls Atomics.wait, even though Atomics.wait is legal in a
// DedicatedWorker and is the usual advice for a consumer thread. WebGPU's
// mapAsync callback only resolves when this agent's EVENT LOOP TURNS;
// Atomics.wait blocks the THREAD and starves the very loop that would resolve
// the map. That is exactly the self-deadlock that the native readback's
// spin-on-ProcessEvents becomes when ported naively to a browser. Everything
// here is timers / promises / macrotasks, and the worker is never woken by the
// audio thread: an Atomics.notify from render() would not block, but it WOULD take
// V8's shared waiter-list lock, and the audio thread takes no locks at all. The
// cost of that choice is one bounded timer poll instead of a wake — see tick().
//
// The worker also POLLS SEVERAL TIMES PER BLOCK, not once: the readback poll
// judges each block's deadline at poll ENTRY, so a once-per-block poll would
// systematically discard blocks that actually completed and make the miss rate
// look far worse than the GPU really is. At 512 frames / 48 kHz a block period
// is 10.667 ms; a ~1 ms pacing tick gives ~10 polls per block.
//
// GPU-module contract (owned by the wasm GPU-DSP module; see ./README.md):
//   Module.pulpGpu = {
//     init()                                  → Promise<bool>   adapter+device bring-up
//     prepare(sampleRate, blockSize, channels, irPtr, irFrames, irChannels) → Promise<bool>
//     inBuffer()  → f32 heap ptr (channels*blockSize planar)
//     outBuffer() → f32 heap ptr (channels*blockSize planar)
//     submit(seq, deadlineMs) → 1 accepted / 0 refused
//     poll()                  → drains completed readbacks, calls onBlockDone
//     stat(which)             → number (queue_submits, map_resolves, gpu_ns_last)
//     onBlockDone(seq, outPtr, gpuNs)         ← set by this worker
//     device                                  ← GPUDevice (for `lost`)
//   }
// A module that exports the bare `_pulp_gpu_*` C symbols instead is adapted by
// bindGpuModule() below.

import {
  attach as attachRing,
  FLAG_WORKER_READY, FLAG_DEVICE_LOST, FLAG_SHUTDOWN, FLAG_ENGINE_GPU,
  STATE_READY, STATE_DEVICE_LOST, STATE_FAILED,
} from "./gpu-ring.mjs";

const IDLE_WAIT_MS = 4;      // idle: nothing queued — still well under a block period
const BUSY_WAIT_MS = 1;      // work outstanding: ~10 polls per 512-frame block
const STATS_PERIOD_MS = 100; // page reads at ~10 Hz
const MAX_IN_FLIGHT = 3;     // multiple staging buffers, never one blocking readback

let ring = null;
let gpu = null;
let running = false;
// A new IR the plugin published, waiting to be swapped in at a safe point in the loop
// (see step 0 of runLoop). Null in steady state.
let pendingIr = null;
// What pulp_gpu_prepare needs to be re-called with. Captured at bring-up so an IR swap
// re-prepares against exactly the same stream geometry the ring was built for.
let prepared = null;   // { sampleRate, blockSize, channels }
let irFrames = 0;      // length of the IR the module is currently prepared with

const state = {
  produced: 0, expired: 0, queueSubmits: 0, mapResolves: 0, gpuNsLast: 0,
  lastBlockUs: 0, avgBlockUs: 0, state: 0,
};

// ── module binding ──────────────────────────────────────────────────────────
// Adapts the module's bare C exports (see gpu_worker_entry.cpp — that file is the
// ABI, this is the only place that knows its shape) to the api object the loop
// drives. Three details are load-bearing and each one was a live bug:
//
//   • pulp_gpu_submit takes THREE arguments (seq, in_planar, deadline_ms). Passing
//     two makes deadline_ms arrive as 0, and the module rejects a non-positive
//     deadline — the whole GPU lane dies at the self-test with "submit refused".
//     The input pointer is re-read per submit: it is stable, but a stale JS-side
//     copy across a heap growth would be a silent wrong-memory read.
//   • pulp_gpu_prepare takes a MONO ir (sample_rate, block, channels, ir, ir_len).
//     There is no per-IR-channel argument, so a multi-channel IR is a caller bug,
//     not something to quietly truncate.
//   • onBlockDone is invoked by the module as Module['onBlockDone'] (EM_ASM), so it
//     must live on Module, not on this api object — and its third argument is
//     `ok`, NOT a GPU timing. gpu_ns has no honest source yet (stat 4 is always 0;
//     see gpu_worker_entry.cpp), so it is read from stat(), never inferred here.
function bindGpuModule(Module) {
  if (Module.pulpGpu) return Module.pulpGpu;
  const api = {
    init: () => Module._pulp_gpu_init(),
    prepare: (sr, block, ch, irPtr, irFrames, irCh = 1) => {
      if (irCh !== 1) throw new Error("pipeline-failed:the module takes a mono IR, got " + irCh + " channels");
      return Module._pulp_gpu_prepare(sr, block, ch, irPtr, irFrames);
    },
    inBuffer: () => Module._pulp_gpu_in_buffer(),
    outBuffer: () => Module._pulp_gpu_out_buffer(),
    submit: (seq, deadlineMs) =>
      Module._pulp_gpu_submit(seq, Module._pulp_gpu_in_buffer(), deadlineMs),
    poll: () => Module._pulp_gpu_poll(),
    stat: (which) => Module._pulp_gpu_stat(which),
    notifyDeviceLost: () => Module._pulp_gpu_notify_device_lost(),
  };
  // The module calls Module['onBlockDone']; the loop assigns api.onBlockDone.
  Object.defineProperty(api, "onBlockDone", {
    get: () => Module.onBlockDone,
    set: (fn) => { Module.onBlockDone = fn; },
  });
  Module.pulpGpu = api;
  return api;
}

// ── pacing: a bounded timer, and NOTHING on the audio thread ────────────────
// Never Atomics.wait: that blocks the THREAD, and WebGPU's mapAsync callback only
// resolves when this agent's EVENT LOOP TURNS — the self-deadlock the native
// readback's spin-on-ProcessEvents becomes when ported naively to a browser.
//
// And deliberately not Atomics.waitAsync-on-a-wake either, even though it is
// non-blocking here: a wake needs the PRODUCER to call Atomics.notify, and the
// producer is the audio thread. Atomics.notify does not block, but it does take
// V8's shared waiter-list lock — a lock, acquired from render(). So the worklet
// signals nothing at all and this worker polls on a bounded timer instead. The
// cost is a timer wakeup per poll; the benefit is that the audio thread's GPU path
// is provably lock-free.
//
// THE MESSAGECHANNEL HOP IS NOT DECORATION. HTML clamps a setTimeout scheduled
// from inside a timer callback to >= 4 ms once the nesting level passes 5 — and a
// polling loop built out of setTimeout IS a nesting chain, so every tick silently
// becomes 4 ms. MEASURED: that alone moved the submit -> readback-resolve round trip
// from ~2.2 ms to ~6.0 ms of the 10.7 ms block budget on an M-series Mac. A message
// task has nesting level 0, so hopping through one before arming the timer restores
// the requested interval. (It also turns the event loop, which is what lets the
// pending mapAsync callbacks resolve.)
const _chan = new MessageChannel();
let _wake = null;
_chan.port1.onmessage = () => { const w = _wake; _wake = null; if (w) w(); };
function macrotask() {
  return new Promise((r) => { _wake = r; _chan.port2.postMessage(0); });
}
async function tick(ms) {
  await macrotask();
  await new Promise((r) => setTimeout(r, ms));
}

// ── the loop ────────────────────────────────────────────────────────────────
async function runLoop(blockPeriodMs, latencyBlocks) {
  const heap = () => gpu.heapF32();
  const inPtr = gpu.api.inBuffer();
  const blockFloats = ring.blockFloats;

  // The module's submit id is this worker's own CONTIGUOUS counter, not the
  // worklet's block index: the block index has GAPS (a full input ring drops a
  // block), and the in-order publish walk below needs a dense sequence. The block
  // index rides along in each record, because it — not the submit id — is what the
  // wet must be stamped with when it reaches the ring (see gpu-ring.mjs).
  const inFlight = new Map();          // id → { deadline, t0, blockSeq }
  const abandoned = new Map();         // id → blockSeq, for ids whose deadline expired
  const stash = new Map();             // id → { buf, blockSeq } (completed out of order)
  const pool = [];
  for (let i = 0; i < MAX_IN_FLIGHT + 1; i++) pool.push(new Float32Array(blockFloats));
  let poolNext = 0;

  let nextSubmit = 0;                  // monotonic id we hand to the module
  let nextPublish = 0;                 // the id the OUT ring is waiting for
  let lastStats = 0;

  // ── Engine=CPU: the GPU does NOTHING. ──────────────────────────────────────
  //
  // The plugin keeps calling the xfer seam on every block in both engines (that is what
  // advances the shared block timeline), so this worker keeps SEEING every input block —
  // but seeing one and convolving it are different things, and convolving a block nobody
  // will hear is exactly what "Engine: CPU" must not do. Measured before this gate: with
  // the page's Engine select on CPU, the worker still issued ~100 queue submits per second
  // and produced a full wet stream that was thrown away. That is a GPU meter pegged, and a
  // battery drained, by an engine the user switched off.
  //
  // Why the history exists. The convolver's tail comes from a frequency-domain delay line
  // of recent input spectra, and updating that line IS the GPU work (the forward FFT is a
  // dispatch) — so an idle GPU is a GPU whose memory of the recent past goes stale. Flip
  // back with a stale line and the tail partitions still hold audio from BEFORE the CPU
  // stretch: you would hear a ghost of old material smeared under the new. So while idle we
  // keep the raw input blocks in plain CPU memory (a memcpy, no GPU), and on the flip to GPU
  // we REPLAY them — oldest first, outputs discarded — which rebuilds exactly the delay line
  // the GPU would have had if it had been running. One priming burst, then live.
  //
  // The history is as long as the convolver's memory (ceil(irFrames / block) + 1) and no
  // longer: replaying more blocks than there are partitions just overwrites the same slots.
  const historyCap = () => Math.max(1, Math.min(512,
      Math.ceil((irFrames || ring.blockSize) / ring.blockSize) + 1));
  const history = [];                  // rolling raw input blocks, oldest → newest
  let engineWasGpu = (ring.flags() & FLAG_ENGINE_GPU) !== 0;
  const warmup = [];                   // blocks queued to prime the delay line on a flip

  // Publish everything that is now contiguous from nextPublish. An abandoned id is
  // SKIPPED (never published): the worklet's pop() then sees the next wet's seq run
  // PAST the block it wants, misses that one slot, and the plugin's CPU net covers
  // it — with no shift of the wet timeline, because every wet carries its block
  // index. Expiry routes to the MissPolicy, never to a hang.
  const drainPublishable = () => {
    for (;;) {
      if (abandoned.has(nextPublish)) { abandoned.delete(nextPublish); nextPublish++; continue; }
      const rec = stash.get(nextPublish);
      if (!rec) break;
      // A PRIMING block (blockSeq < 0). It was replayed to rebuild the delay line after an
      // idle stretch; the worklet is not waiting for it and never asked for it. Drop it —
      // publishing it would put a wet the plugin never pushed into the timeline.
      if (rec.blockSeq < 0) { stash.delete(nextPublish); nextPublish++; continue; }
      if (ring.outputFull()) break;    // worklet is not draining; try again next tick
      ring.publishOutput(rec.buf, 0, rec.blockSeq);
      stash.delete(nextPublish);
      state.produced++;
      nextPublish++;
    }
  };

  // (id, outPtr, ok) — `ok` is 0 when the module completed the block Expired or
  // Failed, or the samples were not finite. Such a block is a MISS: it is never
  // published, the worklet's pop() steps past that block index, and the plugin's
  // CPU net covers it. Publishing it would put the GPU's garbage on the output.
  gpu.api.onBlockDone = (id, outPtr, ok) => {
    state.mapResolves++;
    const rec = inFlight.get(id);
    inFlight.delete(id);
    // The publish walk has already gone past this id: the loop expired it and
    // drainPublishable stepped over it. The module is only now telling us how it
    // ended, and there is nothing left to do — dropping it here is what keeps
    // `abandoned`/`stash` from growing an entry per expiry forever, and what keeps
    // an expiry the loop already counted from being counted a second time.
    if (((id - nextPublish) | 0) < 0) { abandoned.delete(id); return; }
    if (!ok) {
      // Not double-counted: the loop may already have expired this id at a
      // deadline check, and the module then completes it Expired too.
      if (!abandoned.has(id)) { abandoned.set(id, rec ? rec.blockSeq : 0); state.expired++; }
      return;
    }
    if (rec) {
      // The honest round trip: submit → readback-resolve, the same span native's
      // Stats::last_block_us measures. Reporting submit time alone would be a lie.
      const us = (performance.now() - rec.t0) * 1000;
      state.lastBlockUs = us;
      state.avgBlockUs = state.avgBlockUs ? state.avgBlockUs * 0.9 + us * 0.1 : us;
    }
    if (abandoned.has(id)) return;     // too late; the loop already expired it
    if (!rec) return;                  // no record → no block index to stamp it with
    const buf = pool[poolNext++ % pool.length];
    const src = heap();
    const base = outPtr >> 2;
    for (let i = 0; i < blockFloats; i++) buf[i] = src[base + i];
    stash.set(id, { buf, blockSeq: rec.blockSeq });
  };

  running = true;
  while (running) {
    if (ring.flags() & FLAG_SHUTDOWN) break;

    const engineGpu = (ring.flags() & FLAG_ENGINE_GPU) !== 0;
    if (engineGpu && !engineWasGpu) {
      // CPU → GPU. Prime the delay line with everything that played while we were idle,
      // oldest first, so the first live block convolves against the same recent past the
      // CPU engine has been hearing. These are submitted like any other block and then
      // discarded on completion (blockSeq -1).
      warmup.length = 0;
      for (const b of history) warmup.push(b);
    } else if (!engineGpu && engineWasGpu) {
      warmup.length = 0;               // no point priming for an engine nobody is listening to
    }
    engineWasGpu = engineGpu;

    // 0. A NEW IR. The plugin rebuilt its impulse response (the user moved Size, or
    //    uploaded one), so the kernel this worker convolves with is now the wrong one
    //    and every block it produces from here is a different reverb than the CPU
    //    convolver's. Re-prepare.
    //
    //    Applied HERE, at the top of the loop, and only once nothing is in flight:
    //    pulp_gpu_prepare tears down and rebuilds the pipelines and the IR's frequency-
    //    domain partitions, so doing it under a block that is mid-submit would readback
    //    a block convolved with half of each kernel. Draining first costs a few blocks
    //    and is the only version of this that is correct.
    //
    //    Those few blocks are MISSES, and that is not a problem to be solved — it is the
    //    safety net doing its job. A missed block is covered by the plugin's CPU
    //    convolver, which is ALSO crossfading to the new IR (its rebuild is time-sliced
    //    for exactly that reason), so the swap is heard as the same smooth Size change
    //    the CPU engine gives. With the net removed ("GPU only") those blocks are
    //    silent instead — which is what "no safety net" means, and is why it is not the
    //    default.
    if (pendingIr) {
      const ir = pendingIr;
      pendingIr = null;
      if (inFlight.size > 0) {
        pendingIr = ir;                 // still busy — re-arm and drain first
      } else {
        try {
          const p = gpu.malloc(ir.length * 4);
          gpu.heapF32().set(ir, p >> 2);
          irFrames = ir.length;
          if (!(await gpu.api.prepare(prepared.sampleRate, prepared.blockSize,
                                      prepared.channels, p, ir.length, 1))) {
            throw new Error("pulp_gpu_prepare(ir) returned 0");
          }
          state.irSwaps = (state.irSwaps || 0) + 1;
          stash.clear();                // stamped against the OLD kernel
        } catch (err) {
          // A failed re-prepare must not take the audio down: the lane goes quiet, the
          // CPU net covers, and the page is told why rather than left guessing.
          running = false;
          state.state = STATE_FAILED;
          ring.publishStats(state);
          self.postMessage({ type: "failed", reason: "ir-swap-failed:" + String((err && err.message) || err) });
          break;
        }
      }
    }

    // 1. Expire anything past its deadline BEFORE polling, mirroring the native
    //    readback poll's deadline-at-entry semantics.
    const now = performance.now();
    for (const [id, rec] of inFlight) {
      if (now > rec.deadline) {
        inFlight.delete(id);
        abandoned.set(id, rec.blockSeq);
        state.expired++;
      }
    }

    // 1b. Engine=CPU: DRAIN, do not convolve.
    //
    //     The blocks still arrive — the plugin pushes on every block in both engines — so
    //     they must still be taken, or the input ring backs up and the worklet's pushes
    //     start dropping. Take them into plain CPU memory and submit NOTHING: no dispatch,
    //     no readback, no GPU. The plugin's pops find an empty output ring, count a miss,
    //     and its CPU convolver covers — which is not a fallback here, it is the engine the
    //     user asked for.
    if (!engineGpu) {
      const cap = historyCap();
      while (ring.inputDepth() > 0) {
        const buf = new Float32Array(blockFloats);
        const blockSeq = ring.takeInput(buf, 0);
        if (blockSeq === null) break;
        history.push(buf);
        while (history.length > cap) history.shift();
      }
      drainPublishable();              // let anything still in flight from before the flip land
      await tick(IDLE_WAIT_MS);
      if (now - lastStats > STATS_PERIOD_MS) { ring.publishStats(state); lastStats = now; }
      continue;
    }

    // 2. Submit every ready input block we have capacity for. Never run ahead of
    //    what the output ring can hold, or we burn GPU work we cannot publish.
    //    PRIMING blocks (replayed history after an idle stretch) go first: they rebuild the
    //    delay line the live blocks are about to convolve against, so they must precede them.
    while (inFlight.size < MAX_IN_FLIGHT &&
           (warmup.length > 0 || ring.inputDepth() > 0) &&
           (ring.outputDepth() + inFlight.size + stash.size) < ring.slots) {
      const dst = heap();
      let blockSeq;
      if (warmup.length > 0) {
        const buf = warmup.shift();
        dst.set(buf, inPtr >> 2);
        blockSeq = -1;                 // priming: advance the delay line, publish nothing
        state.primed = (state.primed || 0) + 1;
      } else {
        blockSeq = ring.takeInput(dst, inPtr >> 2);
      }
      if (blockSeq === null) break;
      const t0 = performance.now();
      // The block is needed in the OUT ring `latencyBlocks` block-periods after
      // the worklet pushed it; the worker takes it within a tick of the push.
      // pulp_gpu_submit's deadline_ms is a BUDGET measured from the call, not an
      // absolute timestamp — handing it performance.now()+x would give the module
      // an effectively infinite deadline (hours) and leave all expiry to the loop
      // below, so a wedged map would pin a staging buffer instead of expiring.
      const budgetMs = latencyBlocks * blockPeriodMs;
      const deadline = t0 + budgetMs;   // absolute, for THIS loop's bookkeeping
      // A refused submit (module queue full) is an abandoned block, not an error:
      // it becomes a miss the plugin's CPU path covers.
      if (!gpu.api.submit(nextSubmit, budgetMs)) {
        abandoned.set(nextSubmit, blockSeq); state.expired++; nextSubmit++; break;
      }
      inFlight.set(nextSubmit, { deadline, t0, blockSeq });
      if (blockSeq >= 0) {
        // Keep the history warm while the GPU is live too — a flip to CPU can come at any
        // moment, and the history is what makes the flip BACK correct.
        const buf = new Float32Array(blockFloats);
        buf.set(dst.subarray(inPtr >> 2, (inPtr >> 2) + blockFloats));
        history.push(buf);
        const cap = historyCap();
        while (history.length > cap) history.shift();
      }
      state.queueSubmits++;
      nextSubmit++;
    }

    // 3. Drain completed readbacks (poll_readbacks → onBlockDone), then publish.
    gpu.api.poll();
    drainPublishable();

    if (now - lastStats >= STATS_PERIOD_MS) {
      lastStats = now;
      // stat(4) is the module's ONLY honest source of a GPU-busy number: the async
      // convolution shader's timestamp-query span in ns (0 when unsupported or the
      // device quantized a small dispatch to 0). Render 0 as "no timing", never as a
      // stall. Never synthesize one from the callback.
      state.gpuNsLast = gpu.api.stat(4);
      ring.publishStats(state);
    }

    const busy = inFlight.size > 0 || stash.size > 0 || ring.inputDepth() > 0;
    await tick(busy ? BUSY_WAIT_MS : IDLE_WAIT_MS);
  }
}

// ── bring-up ────────────────────────────────────────────────────────────────
// Every failure is a DISTINCT NAMED reason, surfaced verbatim to the UI. A
// device that never arrives, a shader that will not compile, and a self-test
// that returns the wrong samples are three different bugs and must read as three
// different failures.
async function bringUp(msg) {
  ring = attachRing(msg.sab);

  if (typeof navigator === "undefined" || !navigator.gpu) throw new Error("no-navigator-gpu-in-worker");

  const adapter = await navigator.gpu.requestAdapter(msg.adapterOptions || {});
  if (!adapter) throw new Error("no-adapter");

  let device;
  try {
    // Opt INTO timestamp-query when the adapter offers it: WebGPU grants optional
    // features only if named in requiredFeatures. Without this the device never has
    // timestamp-query even where the adapter lists it (measured on Safari 2026-07-16),
    // so the honest GPU-compute-time readout could never light up. Harmless where
    // unsupported — the feature is simply absent and the timing path stays 0.
    const wantTs = adapter.features && adapter.features.has("timestamp-query");
    device = await adapter.requestDevice(wantTs ? { requiredFeatures: ["timestamp-query"] } : {});
  } catch (e) {
    throw new Error("no-device:" + (e && e.message ? e.message : e));
  }
  if (!device) throw new Error("no-device");

  // Device loss is NORMAL behavior (tab eviction, driver reset), not a fatal
  // init error: flag it, stop producing, and let the worklet miss forever — the
  // plugin's CPU path is the fallback and stays correct.
  device.lost.then((info) => {
    running = false;
    state.state = STATE_DEVICE_LOST;
    // The module ADOPTED this device, and an adopted device cannot observe its own
    // loss (WebGPU only lets the device-lost callback be installed on the device
    // DESCRIPTOR, i.e. at creation — and JS created it). Telling the module is not
    // optional: without it, GpuCompute keeps admitting submits into a dead device
    // and every one of them expires, forever.
    try { if (gpu && gpu.api.notifyDeviceLost) gpu.api.notifyDeviceLost(); } catch { /* teardown */ }
    if (ring) { ring.setFlag(FLAG_DEVICE_LOST); ring.publishStats(state); }
    self.postMessage({ type: "device-lost", reason: (info && info.reason) || "unknown",
                       message: (info && info.message) || "" });
  });

  // The wasm GPU-DSP module. It must NOT drag in Skia: it is a DSP-only build.
  const factory = (await import(/* @vite-ignore */ msg.moduleUrl)).default;
  // `preinitializedWebGPUDevice` is emdawnwebgpu's handoff: the module adopts the
  // device WE created (and whose `lost` we watch) instead of requesting its own.
  const Module = await factory({ ...(msg.moduleOptions || {}), preinitializedWebGPUDevice: device });
  const api = bindGpuModule(Module);

  const initOk = await api.init();
  if (!initOk) throw new Error("shader-compile-failed:pulp_gpu_init returned 0");

  gpu = {
    api, device,
    heapF32: () => Module.HEAPF32,
    malloc: (n) => Module._malloc(n),
  };

  // Prepare with the caller's IR (the self-test overrides it with a unit impulse
  // first: convolution with a unit impulse is the identity, so the returned
  // samples MUST equal the input — a cheap end-to-end proof that the shaders,
  // the bindings, and the readback all actually work on THIS device).
  const blockSize = ring.blockSize, channels = ring.channels;
  const impulse = new Float32Array(blockSize);
  impulse[0] = 1;
  const irPtr = gpu.malloc(impulse.length * 4);
  Module.HEAPF32.set(impulse, irPtr >> 2);
  if (!(await api.prepare(msg.sampleRate, blockSize, channels, irPtr, impulse.length, 1))) {
    throw new Error("pipeline-failed:pulp_gpu_prepare(unit impulse) returned 0");
  }
  await selfTest(api, Module, blockSize, channels);

  // Re-prepare with the real IR now that the device is proven.
  if (msg.ir && msg.ir.length) {
    const ir = msg.ir instanceof Float32Array ? msg.ir : new Float32Array(msg.ir);
    const p = gpu.malloc(ir.length * 4);
    Module.HEAPF32.set(ir, p >> 2);
    const irCh = msg.irChannels || 1;
    if (!(await api.prepare(msg.sampleRate, blockSize, channels, p, ir.length / irCh, irCh))) {
      throw new Error("pipeline-failed:pulp_gpu_prepare(ir) returned 0");
    }
  }

  prepared = { sampleRate: msg.sampleRate, blockSize, channels };
  irFrames = (msg.ir && msg.ir.length) ? (msg.ir.length / (msg.irChannels || 1)) : 0;

  state.state = STATE_READY;
  ring.setFlag(FLAG_WORKER_READY);
  ring.publishStats(state);

  const info = (typeof adapter.requestAdapterInfo === "function")
    ? await adapter.requestAdapterInfo().catch(() => null)
    : (adapter.info || null);

  self.postMessage({
    type: "ready", ok: true,
    adapterInfo: info ? { vendor: info.vendor, architecture: info.architecture,
                          device: info.device, description: info.description } : null,
    features: [...device.features],
    limits: {
      maxComputeWorkgroupSizeX: device.limits.maxComputeWorkgroupSizeX,
      maxComputeInvocationsPerWorkgroup: device.limits.maxComputeInvocationsPerWorkgroup,
      maxComputeWorkgroupStorageSize: device.limits.maxComputeWorkgroupStorageSize,
      maxStorageBufferBindingSize: device.limits.maxStorageBufferBindingSize,
      maxBufferSize: device.limits.maxBufferSize,
      maxStorageBuffersPerShaderStage: device.limits.maxStorageBuffersPerShaderStage,
    },
    timestampQuery: device.features.has("timestamp-query"),
  });

  const blockPeriodMs = (blockSize / msg.sampleRate) * 1000;
  runLoop(blockPeriodMs, ring.latencyBlocks).catch((e) => {
    running = false;
    state.state = STATE_FAILED;
    ring.publishStats(state);
    self.postMessage({ type: "failed", reason: "worker-loop:" + String((e && e.message) || e) });
  });
}

// One known block through the real path: submit → poll → readback. The IR is a
// unit impulse, so wet MUST equal dry sample-for-sample. A mismatch means the
// GPU produced wrong audio, which is a HARDER failure than "no device" — never
// silently ship it as a working GPU engine.
//
// UNLESS a kernel's WGSL has been REPLACED (stat 6 > 0 — the live-kernel editor, or
// the proof fixture's 0.5x tamper). Then the maths is deliberately not the shipped
// maths and "wet == dry" is not a property anyone promised: the check relaxes to
// "the block came back ok, finite, and not silent", which still catches a kernel
// edit that broke the shader (a shader that fails to compile does NOT fail pipeline
// creation in Dawn — it reports asynchronously and then quietly dispatches zeros).
// The strict identity check is what runs in every shipped configuration.
async function selfTest(api, Module, blockSize, channels) {
  const inPtr = api.inBuffer();
  const heap = () => Module.HEAPF32;
  const probe = new Float32Array(blockSize * channels);
  for (let c = 0; c < channels; c++) {
    for (let i = 0; i < blockSize; i++) probe[c * blockSize + i] = Math.sin(i * 0.05 + c);
  }
  heap().set(probe, inPtr >> 2);

  let done = null;
  let selfTestOk = true;
  api.onBlockDone = (seq, outPtr, ok) => { done = outPtr; selfTestOk = !!ok; };
  if (!api.submit(0, 2000)) throw new Error("pipeline-failed:submit refused");

  const deadline = performance.now() + 2000;
  while (done === null) {
    if (performance.now() > deadline) throw new Error("worker-timeout");
    api.poll();
    if (done !== null) break;
    await new Promise((r) => setTimeout(r, 1));  // turn the event loop: mapAsync resolves HERE
  }

  if (!selfTestOk) throw new Error("selftest-mismatch:the module completed the probe block not-ok");

  const out = heap();
  const base = done >> 2;
  const overridden = api.stat(6) > 0;

  let worst = 0, peak = 0, nonFinite = 0;
  for (let i = 0; i < probe.length; i++) {
    const v = out[base + i];
    if (!Number.isFinite(v)) { nonFinite++; continue; }
    peak = Math.max(peak, Math.abs(v));
    worst = Math.max(worst, Math.abs(v - probe[i]));
  }
  if (nonFinite > 0)
    throw new Error("selftest-mismatch:" + nonFinite + " nonfinite samples came back from the GPU");

  api.onBlockDone = null;

  if (overridden) {
    // A replaced kernel that produced SILENCE is a broken shader, not a valid edit.
    if (!(peak > 1e-4))
      throw new Error("selftest-mismatch:the overridden kernel produced silence (peak " +
                      peak.toExponential(2) + ") — the WGSL edit almost certainly does not compile");
    return;   // identity is not a property of a kernel someone replaced
  }
  if (!(worst < 1e-3)) throw new Error("selftest-mismatch:max |wet-dry| = " + worst.toExponential(2));
}

self.onmessage = async (e) => {
  const msg = e.data;
  if (msg.type === "init") {
    try {
      await bringUp(msg);
    } catch (err) {
      running = false;
      state.state = STATE_FAILED;
      if (ring) ring.publishStats(state);
      // The message IS the reason code (`no-adapter`, `selftest-mismatch:…`); the
      // page shows it verbatim rather than a generic "GPU unavailable".
      self.postMessage({ type: "failed", reason: String((err && err.message) || err) });
    }
  } else if (msg.type === "ir") {
    // Queued, never applied here: this runs on the worker's event loop, which can land
    // in the middle of a submitted-but-not-read-back block. runLoop applies it once
    // nothing is in flight.
    const ir = msg.ir instanceof Float32Array ? msg.ir : new Float32Array(msg.ir || []);
    if (ir.length) pendingIr = ir;
  } else if (msg.type === "shutdown") {
    running = false;
    if (ring) ring.setFlag(FLAG_SHUTDOWN);
    self.close();
  }
};
