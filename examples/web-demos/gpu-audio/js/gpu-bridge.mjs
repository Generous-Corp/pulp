// gpu-bridge.mjs — the MAIN-THREAD half of the browser GPU-audio transport.
//
// It owns the SharedArrayBuffer and hands the SAME buffer to both agents that
// need it: the DedicatedWorker (which owns WebGPU) and the AudioWorklet (which
// owns the audio thread). Neither can reach the other directly — a worklet
// cannot touch navigator.gpu and cannot spawn a Worker — so the page is the
// only place the two can be introduced.
//
//   probe() → allocate+prime ring → new Worker(gpu-worker.mjs) → postMessage(sab)
//           → the worker does the FULL bring-up and answers { ok } or a NAMED reason
//           → only then does the caller build the AudioWorkletNode with
//             processorOptions: { gpuSab, gpuLatencyBlocks }
//
// HONESTY: this is a CAPABILITY demonstration — the same convolution running as
// a WebGPU compute shader in a browser tab. It is NOT a speed claim. The CPU
// path stays the default and stays a working fallback; every missed GPU deadline
// is covered by it. A measured result of "the browser GPU path cannot hold
// real-time here" is a truthful, publishable outcome, not a failure to hide.

import {
  allocate as allocateRing,
  FLAG_SHUTDOWN, FLAG_DEVICE_LOST,
  STATE_DEVICE_LOST, STATE_FAILED, STATE_READY,
} from "./gpu-ring.mjs";

// Named failure reasons, surfaced VERBATIM to the UI. Each names one distinct
// thing that can be wrong; a generic "GPU unavailable" would hide which.
export const REASONS = {
  NOT_ISOLATED: "not-cross-origin-isolated",
  NO_SAB: "no-sab",
  NO_WORKER_GPU: "no-navigator-gpu-in-worker",
  NO_ADAPTER: "no-adapter",
  NO_DEVICE: "no-device",
  SHADER: "shader-compile-failed",
  PIPELINE: "pipeline-failed",
  SELFTEST: "selftest-mismatch",
  TIMEOUT: "worker-timeout",
  DEVICE_LOST: "device-lost",
};

const BRINGUP_TIMEOUT_MS = 20000;

// Cheap, synchronous, main-thread-only preconditions. Deliberately does NOT gate
// on main-thread navigator.gpu: Window and DedicatedWorker expose WebGPU
// independently, so the WORKER is the only authority on whether the worker can
// get a device.
export function probe() {
  if (typeof SharedArrayBuffer !== "function") return { ok: false, reason: REASONS.NO_SAB };
  if (typeof crossOriginIsolated !== "undefined" && !crossOriginIsolated) {
    // COOP: same-origin + COEP: require-corp are required for a SAB. The WebCLAP
    // Cloudflare host already sets both (wclap-build/cloudflare/_headers).
    return { ok: false, reason: REASONS.NOT_ISOLATED };
  }
  return { ok: true };
}

/**
 * Bring the GPU lane up end-to-end and, only on success, hand back the SAB the
 * AudioWorkletNode must be constructed with.
 *
 * @param {{ workerUrl?: string|URL, moduleUrl: string, ir?: Float32Array,
 *           irChannels?: number, sampleRate: number, blockSize?: number,
 *           channels?: number, slots?: number, latencyBlocks?: number,
 *           onDeviceLost?: (info) => void }} opts
 * @returns {Promise<{ ok: true, sab: SharedArrayBuffer, latencyBlocks: number,
 *                     latencySamples: number, adapterInfo, features, limits,
 *                     timestampQuery: boolean, ring, pollStats, shutdown }
 *                  | { ok: false, reason: string }>}
 */
export async function startGpuLane(opts) {
  const pre = probe();
  if (!pre.ok) return pre;

  const blockSize = opts.blockSize || 512;
  const channels = opts.channels || 2;
  const latencyBlocks = opts.latencyBlocks || 2;

  let ring;
  try {
    ring = allocateRing({ blockSize, channels, slots: opts.slots || 4, latencyBlocks });
  } catch (e) {
    return { ok: false, reason: "no-sab:" + String((e && e.message) || e) };
  }
  // Reset BEFORE either agent runs. The latency is exactly latencyBlocks *
  // blockSize samples and constant for the prepared lifetime because pop() asks
  // for the wet of the block that is latencyBlocks old — not because the ring is
  // pre-filled with silence. The first L blocks therefore MISS (the plugin's
  // latency-aligned CPU wet covers them) instead of emitting primed silence.
  ring.prime();

  const workerUrl = opts.workerUrl || new URL("./gpu-worker.mjs", import.meta.url);
  const worker = new Worker(workerUrl, { type: "module" });

  const result = await new Promise((resolve) => {
    const timer = setTimeout(() => resolve({ ok: false, reason: REASONS.TIMEOUT }), BRINGUP_TIMEOUT_MS);
    worker.onmessage = (e) => {
      const m = e.data;
      if (m.type === "ready") { clearTimeout(timer); resolve(m); }
      else if (m.type === "failed") { clearTimeout(timer); resolve({ ok: false, reason: m.reason }); }
      else if (m.type === "device-lost") {
        // Device loss is normal behavior, never a crash: the worklet just keeps
        // missing and the plugin's CPU path covers every block from here on.
        opts.onDeviceLost && opts.onDeviceLost(m);
      }
    };
    worker.onerror = (e) => {
      clearTimeout(timer);
      resolve({ ok: false, reason: "worker-error:" + (e && e.message ? e.message : "load failed") });
    };
    worker.postMessage({
      type: "init", sab: ring.sab, moduleUrl: String(opts.moduleUrl),
      sampleRate: opts.sampleRate, ir: opts.ir || null, irChannels: opts.irChannels || 1,
      adapterOptions: opts.adapterOptions || {},
    });
  });

  if (!result.ok) {
    try { worker.terminate(); } catch { /* already gone */ }
    return { ok: false, reason: result.reason };
  }

  const lane = {
    ok: true,
    sab: ring.sab,
    ring,
    latencyBlocks,
    // Adaptive depth can grow latencyBlocks up to slots-1 without re-allocating the
    // SAB, because the ring is pre-sized to `slots` up front — the page picks slots
    // to cover the deepest depth it will ever request.
    maxLatencyBlocks: (opts.slots || 4) - 1,
    latencySamples: latencyBlocks * blockSize,
    adapterInfo: result.adapterInfo,
    features: result.features,
    limits: result.limits,
    timestampQuery: !!result.timestampQuery,

    // Hand the worker the impulse response the PLUGIN is actually using. Both engines
    // have to convolve with the same kernel or the CPU convolver stops being a
    // sample-for-sample substitute for a block the GPU missed, and the "safety net" is
    // really a second, different reverb cutting in.
    //
    // The plugin publishes its IR post-normalize and post-window (see the WebCLAP
    // module's pulp_ir_* exports), so this is the transformed IR, not the source file
    // the user picked. Handing the page's raw IR to both sides would NOT be equivalent.
    //
    // Safe to call at any time, including before the worker has finished bring-up: it
    // is queued and applied at a safe point in the worker's loop.
    setIr(samples) {
      if (!samples || !samples.length) return false;
      const ir = samples instanceof Float32Array ? samples : Float32Array.from(samples);
      // Copied, never transferred: the caller (the page) still owns this array, and the
      // plugin may publish the same buffer again.
      try { worker.postMessage({ type: "ir", ir: ir.slice() }); return true; }
      catch { return false; }
    },

    // Adaptive depth: retarget the pipeline latency live. Writes CTRL_LATENCY — read
    // live by the worklet's pop() and the worker's deadline budget — and the worklet
    // moves the plugin's L to match on its next non-RT tick (pollDepth). Clamped to
    // < slots. Returns the applied value.
    setLatency(L) {
      const applied = ring.setLatency(L);
      lane.latencyBlocks = applied;
      lane.latencySamples = applied * blockSize;
      return applied;
    },

    // Poll at ~10 Hz with setInterval, NOT requestAnimationFrame: rAF is throttled
    // in a background tab, so the readout would freeze exactly when misses are
    // most interesting (a backgrounded tab keeps rendering audio but throttles the
    // worker — missed GPU deadlines are the expected steady state there).
    pollStats() {
      const s = ring.readStats();
      if (!s) return null;
      s.deviceLost = (ring.flags() & FLAG_DEVICE_LOST) !== 0 || s.state === STATE_DEVICE_LOST;
      s.failed = s.state === STATE_FAILED;
      s.ready = s.state === STATE_READY;
      const total = s.produced + s.miss;
      s.missRate = total > 0 ? s.miss / total : 0;
      return s;
    },

    shutdown() {
      ring.setFlag(FLAG_SHUTDOWN);
      try { worker.postMessage({ type: "shutdown" }); } catch { /* already gone */ }
      setTimeout(() => { try { worker.terminate(); } catch { /* already gone */ } }, 250);
    },
  };
  return lane;
}

export default startGpuLane;
