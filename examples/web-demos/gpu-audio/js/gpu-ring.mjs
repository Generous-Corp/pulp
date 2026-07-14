// gpu-ring.mjs — the pinned SharedArrayBuffer transport between the AudioWorklet
// and the WebGPU DedicatedWorker (SAB layout v2, magic 'PGR2').
//
// WHY A SAB AT ALL: an AudioWorkletProcessor cannot touch `navigator.gpu`
// (WebGPU is exposed on Window and DedicatedWorker only) and cannot spawn a
// Worker. So the MAIN THREAD allocates one SharedArrayBuffer and hands the same
// buffer to both the GPU worker and the worklet; the two never postMessage on
// the audio path.
//
// This is the browser translation of pulp::gpu_audio::GpuAudioTransport
// (core/gpu_audio/include/pulp/gpu_audio/gpu_audio_transport.hpp): two planar
// rings and a non-RT worker. The audio thread never waits on, allocates for, or
// synchronizes with the GPU — a missed GPU deadline is a normal, expected outcome
// that the plugin's CPU path covers.
//
// *** EVERY BLOCK CARRIES ITS SEQUENCE NUMBER — v2's one real difference. ***
// The native transport is POSITIONAL: it primes the output ring with L blocks of
// silence and then trusts that exactly one wet block is published per input block
// forever, correcting the rare miss with a `blocks_owed_` debt. In a browser that
// invariant does not hold, and it fails on the paths the design calls NORMAL:
//   • a full input ring DROPS a block (the worker is >= slots blocks behind — a
//     backgrounded tab does this by design), so one input never reaches the GPU;
//   • a block whose deadline expires is never published, so one wet never lands.
// Under a positional protocol each of those permanently shifts the wet stream
// against the dry path by one block — silently, with no miss to observe, comb
// filtering forever. So here every input slot carries the worklet's monotonic
// BLOCK INDEX, the worker echoes it on the wet it produces, and pop() asks for
// the one block it is owed (`blockIndex - latencyBlocks`):
//   head seq  < want → a late wet for a slot already covered → discard (resync)
//   head seq  > want → the wet for this slot was dropped/expired → MISS (the
//                      plugin's CPU net covers it), and the ring is NOT consumed
//   head seq == want → hit
// Drops, expiries, worker stalls, and a live Engine flip all reduce to at most a
// few MISSES, which is the one failure mode the whole design is built to absorb.
// There is no priming, no silence prefix, and no owed-block debt to get wrong.
//
// The core block below is INLINED VERBATIM into the classic worklet bundle
// (packages/pulp-web-player/src/vendor/pulp-wasm/wclap-processor.js — an
// AudioWorklet module loaded with addModule() is a classic script and cannot
// `import`). packages/pulp-web-player/test/gpu-ring-parity.test.mjs asserts the
// two copies are byte-identical, so they can never silently drift.
//
// Nothing in the core block may call Atomics.wait: it runs on the audio thread,
// where blocking is forbidden (and throws in a real browser). It also does not
// call Atomics.notify: notify does not block, but it does take V8's shared
// waiter-list lock, and the audio thread acquires NO locks. The worker therefore
// polls on a bounded timer rather than parking on a wake.

/* ── BEGIN gpu-ring core (inlined verbatim into wclap-processor.js) ─────────── */
const PGR_MAGIC = 0x50475232;      // 'PGR2'
const PGR_VERSION = 2;

// ctrl — Int32Array(64) over bytes [0,256). Lanes are spaced to keep the four
// hot cursors off each other's cache lines. Cursors are MONOTONIC uint32 counts
// (never wrapped by hand): slot = idx % slots, depth = (write - read) >>> 0.
const CTRL_MAGIC = 0;
const CTRL_FLAGS = 1;              // bit0 worker_ready, bit1 device_lost, bit2 shutdown
const CTRL_BLOCK = 2;
const CTRL_CHANNELS = 3;
const CTRL_SLOTS = 4;
const CTRL_LATENCY = 5;
const CTRL_VERSION = 6;
const CTRL_STATS_SEQ = 7;          // seqlock; odd = the worker is inside the stats block
const CTRL_IN_WRITE = 16;          // producer: worklet
const CTRL_IN_READ = 24;           // consumer: worker
const CTRL_OUT_WRITE = 32;         // producer: worker
const CTRL_OUT_READ = 40;          // consumer: worklet
// Worklet-owned counters. The stats block has ONE writer (the worker) so it can
// use a seqlock; these three are counted on the audio thread, so they live in
// atomic ctrl lanes and the worker MIRRORS them into the stats block.
const CTRL_MISS = 48;
const CTRL_DROPPED = 52;
const CTRL_RESYNC = 56;

const FLAG_WORKER_READY = 1 << 0;
const FLAG_DEVICE_LOST = 1 << 1;
const FLAG_SHUTDOWN = 1 << 2;
// Is the GPU the engine the plugin is actually LISTENING to? Set by the main thread
// from the plugin's Engine parameter. The worker does no GPU work while it is clear —
// convolving blocks nobody will hear is exactly what "Engine: CPU" must not do. It is a
// flag rather than a message because the worker reads it once per loop tick and a message
// would need draining on a thread whose whole job is to not stall.
const FLAG_ENGINE_GPU = 1 << 3;

// stats — Float64Array(16) over bytes [256,384), written by the worker under the
// CTRL_STATS_SEQ seqlock, read by the page at ~10 Hz.
const STAT_PRODUCED = 0;
const STAT_MISS = 1;
const STAT_DROPPED = 2;
const STAT_RESYNCED = 3;
const STAT_EXPIRED = 4;
const STAT_LAST_BLOCK_US = 5;      // submit → readback-resolve, the honest round trip
const STAT_AVG_BLOCK_US = 6;
const STAT_GPU_NS_LAST = 7;        // GPU timestamp-query span, 0 when unsupported
const STAT_QUEUE_SUBMITS = 8;
const STAT_MAP_RESOLVES = 9;
const STAT_STATE = 10;             // 0 init, 1 ready, 2 device-lost, 3 failed
// Blocks replayed to rebuild the convolver's delay line after an Engine=CPU stretch, during
// which the worker does no GPU work and the line therefore goes stale. Observable on purpose:
// "the GPU is idle on CPU" and "the flip back is still correct" are two claims, and this is
// the evidence for the second one.
const STAT_PRIMED = 11;
const STATS_DOUBLES = 16;

const STATE_INIT = 0;
const STATE_READY = 1;
const STATE_DEVICE_LOST = 2;
const STATE_FAILED = 3;

const CTRL_OFFSET = 0;
const CTRL_INTS = 64;
const STATS_OFFSET = 256;          // 8-byte aligned for the Float64Array view
// Two Int32Array(slots) seq lanes — the input block index the worklet stamped on
// each slot, and the one the worker echoes onto the wet it produced from it.
const SEQ_OFFSET = 384;

const GPU_RING_DEFAULTS = { blockSize: 512, channels: 2, slots: 4, latencyBlocks: 2 };

function gpuRingLayout(opts) {
  const o = opts || {};
  const blockSize = o.blockSize || GPU_RING_DEFAULTS.blockSize;
  const channels = o.channels || GPU_RING_DEFAULTS.channels;
  const slots = o.slots || GPU_RING_DEFAULTS.slots;
  const latencyBlocks = o.latencyBlocks == null ? GPU_RING_DEFAULTS.latencyBlocks : o.latencyBlocks;
  // The worker has `latencyBlocks` block-periods to turn a block around, and its
  // result must still fit in the ring behind whatever it has already published.
  if (latencyBlocks >= slots) throw new Error("gpu-ring: latencyBlocks must be < slots");
  if (latencyBlocks < 1) throw new Error("gpu-ring: latencyBlocks must be >= 1");
  const blockFloats = channels * blockSize;
  const ringBytes = slots * blockFloats * 4;
  const seqBytes = slots * 4;
  const dataOffset = SEQ_OFFSET + seqBytes * 2;   // stays 4-byte aligned for Float32Array
  return {
    version: PGR_VERSION, blockSize, channels, slots, latencyBlocks, blockFloats,
    ctrlOffset: CTRL_OFFSET, statsOffset: STATS_OFFSET,
    inSeqOffset: SEQ_OFFSET, outSeqOffset: SEQ_OFFSET + seqBytes,
    inOffset: dataOffset, outOffset: dataOffset + ringBytes,
    byteLength: dataOffset + ringBytes * 2,
  };
}

function gpuRingAllocate(opts) {
  const L = gpuRingLayout(opts);
  const sab = new SharedArrayBuffer(L.byteLength);
  const ctrl = new Int32Array(sab, CTRL_OFFSET, CTRL_INTS);
  ctrl[CTRL_BLOCK] = L.blockSize;
  ctrl[CTRL_CHANNELS] = L.channels;
  ctrl[CTRL_SLOTS] = L.slots;
  ctrl[CTRL_LATENCY] = L.latencyBlocks;
  ctrl[CTRL_VERSION] = PGR_VERSION;
  // MAGIC last: a consumer that sees the magic sees a fully described header.
  Atomics.store(ctrl, CTRL_MAGIC, PGR_MAGIC);
  return gpuRingAttach(sab);
}

function gpuRingAttach(sab) {
  return new GpuRing(sab);
}

// One attachment to the shared ring. Every method on the audio-thread path
// (push/pop) is allocation-free, lock-free, and never blocks.
class GpuRing {
  constructor(sab) {
    const ctrl = new Int32Array(sab, CTRL_OFFSET, CTRL_INTS);
    if (Atomics.load(ctrl, CTRL_MAGIC) !== PGR_MAGIC) throw new Error("gpu-ring: bad magic");
    if (ctrl[CTRL_VERSION] !== PGR_VERSION) throw new Error("gpu-ring: version mismatch");
    this.sab = sab;
    this.ctrl = ctrl;
    this.stats = new Float64Array(sab, STATS_OFFSET, STATS_DOUBLES);
    this.blockSize = ctrl[CTRL_BLOCK];
    this.channels = ctrl[CTRL_CHANNELS];
    this.slots = ctrl[CTRL_SLOTS];
    this.latencyBlocks = ctrl[CTRL_LATENCY];
    this.blockFloats = this.channels * this.blockSize;
    const ringFloats = this.slots * this.blockFloats;
    this.inSeq = new Int32Array(sab, SEQ_OFFSET, this.slots);
    this.outSeq = new Int32Array(sab, SEQ_OFFSET + this.slots * 4, this.slots);
    const dataOffset = SEQ_OFFSET + this.slots * 8;
    this.in = new Float32Array(sab, dataOffset, ringFloats);
    this.out = new Float32Array(sab, dataOffset + ringFloats * 4, ringFloats);
    // The worklet's monotonic internal-block counter — the timeline every seq
    // stamp is expressed in. Touched ONLY by push()/pop() on the audio thread, so
    // it needs no atomicity; it is not shared with the worker (the worker learns
    // each block's index from the slot's seq stamp).
    this.blockIndex = 0;
  }

  // Reset both rings to their start-of-stream state. Call once, from the main
  // thread, before either side runs. There is NO silence priming: the fixed
  // latency comes from pop() asking for the block that is exactly `latencyBlocks`
  // old, so the first L pops simply MISS (the plugin's latency-aligned CPU wet
  // covers them) instead of emitting a burst of primed digital silence.
  prime() {
    const c = this.ctrl;
    Atomics.store(c, CTRL_IN_WRITE, 0);
    Atomics.store(c, CTRL_IN_READ, 0);
    Atomics.store(c, CTRL_OUT_READ, 0);
    Atomics.store(c, CTRL_OUT_WRITE, 0);
    Atomics.store(c, CTRL_MISS, 0);
    Atomics.store(c, CTRL_DROPPED, 0);
    Atomics.store(c, CTRL_RESYNC, 0);
    for (let i = 0; i < this.slots; i++) { this.inSeq[i] = 0; this.outSeq[i] = 0; }
    this.blockIndex = 0;
    return this;
  }

  // ── audio thread (worklet) ────────────────────────────────────────────────
  // Copy one block of planar interleaved-by-channel floats ([ch0 B][ch1 B]) from
  // `src` at float offset `off` into the input ring, stamped with this block's
  // index. A full ring DROPS the block — the audio thread never blocks and never
  // waits for the GPU — and because the block carries its index, the drop costs
  // exactly ONE miss `latencyBlocks` later rather than shifting the wet stream.
  // Returns 1/0. MUST be called exactly once per internal block, before pop().
  push(src, off) {
    const c = this.ctrl;
    const w = Atomics.load(c, CTRL_IN_WRITE) >>> 0;
    const r = Atomics.load(c, CTRL_IN_READ) >>> 0;
    if (((w - r) >>> 0) >= this.slots) { Atomics.add(c, CTRL_DROPPED, 1); return 0; }
    const n = this.blockFloats;
    const slot = w % this.slots;
    const base = slot * n;
    const dst = this.in;
    for (let i = 0; i < n; i++) dst[base + i] = src[off + i];
    // Seq first, then the cursor: the worker reads the cursor to decide the slot
    // is readable, so the stamp must already be there when it does.
    Atomics.store(this.inSeq, slot, this.blockIndex | 0);
    Atomics.store(c, CTRL_IN_WRITE, (w + 1) | 0);
    return 1;
  }

  // Deliver the wet of the block that is exactly `latencyBlocks` old into `dst` at
  // float offset `off`. Returns 1 on a hit, 0 on a MISS — the caller substitutes
  // per its MissPolicy (SuperConvolver: its own latency-aligned CPU wet). MUST be
  // called exactly once per internal block, after push(): together they advance
  // the block timeline both stamps are expressed in.
  pop(dst, off) {
    const c = this.ctrl;
    const n = this.blockFloats;
    const want = (this.blockIndex - this.latencyBlocks) | 0;
    this.blockIndex = (this.blockIndex + 1) | 0;

    let r = Atomics.load(c, CTRL_OUT_READ) >>> 0;
    const w = Atomics.load(c, CTRL_OUT_WRITE) >>> 0;
    let hit = 0;
    let dropped = 0;
    while (((w - r) >>> 0) > 0) {
      const slot = r % this.slots;
      // Wrap-safe ordering: compare the DIFFERENCE, never the raw values.
      const age = (Atomics.load(this.outSeq, slot) - want) | 0;
      if (age > 0) break;          // this slot's wet was never produced → miss
      r = (r + 1) | 0;
      if (age < 0) { dropped++; continue; }   // a late wet we already covered
      const base = slot * n;
      const src = this.out;
      for (let i = 0; i < n; i++) dst[off + i] = src[base + i];
      hit = 1;
      break;
    }
    Atomics.store(c, CTRL_OUT_READ, r | 0);
    if (dropped > 0) Atomics.add(c, CTRL_RESYNC, dropped);
    if (!hit) Atomics.add(c, CTRL_MISS, 1);
    return hit;
  }

  // ── worker ────────────────────────────────────────────────────────────────
  inputDepth() {
    return (Atomics.load(this.ctrl, CTRL_IN_WRITE) - Atomics.load(this.ctrl, CTRL_IN_READ)) >>> 0;
  }
  outputDepth() {
    return (Atomics.load(this.ctrl, CTRL_OUT_WRITE) - Atomics.load(this.ctrl, CTRL_OUT_READ)) >>> 0;
  }
  outputFull() { return this.outputDepth() >= this.slots; }

  // Copy the next input block into `dst` at float offset `off` and consume it.
  // Returns the worklet's BLOCK INDEX for that block — the seq the wet produced
  // from it must be published with — or null when the ring is empty. (Not -1: a
  // block index is a wrap-safe int32 and 0 is a perfectly ordinary one.)
  takeInput(dst, off) {
    const c = this.ctrl;
    const r = Atomics.load(c, CTRL_IN_READ) >>> 0;
    const w = Atomics.load(c, CTRL_IN_WRITE) >>> 0;
    if (((w - r) >>> 0) === 0) return null;
    const n = this.blockFloats;
    const slot = r % this.slots;
    const base = slot * n;
    const src = this.in;
    for (let i = 0; i < n; i++) dst[off + i] = src[base + i];
    const seq = Atomics.load(this.inSeq, slot) | 0;
    Atomics.store(c, CTRL_IN_READ, (r + 1) | 0);
    return seq;
  }

  // Publish one produced block under the block index it was made from. Blocks MUST
  // be published in increasing seq order (pop() reads the ring head-first); the
  // caller reorders late completions. A block that is never published (expired,
  // failed, or refused) needs no placeholder: pop() sees the next seq run PAST the
  // one it wants and misses that one slot, which the plugin's CPU net covers.
  publishOutput(src, off, seq) {
    const c = this.ctrl;
    const w = Atomics.load(c, CTRL_OUT_WRITE) >>> 0;
    const r = Atomics.load(c, CTRL_OUT_READ) >>> 0;
    if (((w - r) >>> 0) >= this.slots) return 0;   // worklet is not draining; drop
    const n = this.blockFloats;
    const slot = w % this.slots;
    const base = slot * n;
    const dst = this.out;
    for (let i = 0; i < n; i++) dst[base + i] = src[off + i];
    Atomics.store(this.outSeq, slot, seq | 0);
    Atomics.store(c, CTRL_OUT_WRITE, (w + 1) | 0);
    return 1;
  }

  // ── flags + stats ─────────────────────────────────────────────────────────
  flags() { return Atomics.load(this.ctrl, CTRL_FLAGS); }
  setFlag(bit) { Atomics.or(this.ctrl, CTRL_FLAGS, bit); }
  clearFlag(bit) { Atomics.and(this.ctrl, CTRL_FLAGS, ~bit); }

  workletCounters() {
    return {
      miss: Atomics.load(this.ctrl, CTRL_MISS) >>> 0,
      dropped: Atomics.load(this.ctrl, CTRL_DROPPED) >>> 0,
      resynced: Atomics.load(this.ctrl, CTRL_RESYNC) >>> 0,
    };
  }

  // Worker-only. Publishes the worker's own counters AND mirrors the worklet's
  // atomic ctrl counters into the stats block, under the seqlock, so the page
  // gets one coherent snapshot.
  publishStats(s) {
    const c = this.ctrl, st = this.stats;
    Atomics.add(c, CTRL_STATS_SEQ, 1);           // odd: writer inside
    st[STAT_PRODUCED] = s.produced || 0;
    st[STAT_MISS] = Atomics.load(c, CTRL_MISS) >>> 0;
    st[STAT_DROPPED] = Atomics.load(c, CTRL_DROPPED) >>> 0;
    st[STAT_RESYNCED] = Atomics.load(c, CTRL_RESYNC) >>> 0;
    st[STAT_EXPIRED] = s.expired || 0;
    st[STAT_LAST_BLOCK_US] = s.lastBlockUs || 0;
    st[STAT_AVG_BLOCK_US] = s.avgBlockUs || 0;
    st[STAT_GPU_NS_LAST] = s.gpuNsLast || 0;
    st[STAT_QUEUE_SUBMITS] = s.queueSubmits || 0;
    st[STAT_MAP_RESOLVES] = s.mapResolves || 0;
    st[STAT_STATE] = s.state == null ? STATE_INIT : s.state;
    st[STAT_PRIMED] = s.primed || 0;
    Atomics.add(c, CTRL_STATS_SEQ, 1);           // even: snapshot complete
  }

  // Seqlock reader. Returns a snapshot object, or null if the writer kept the
  // block busy across every retry (the caller just polls again).
  readStats() {
    const c = this.ctrl, st = this.stats;
    for (let attempt = 0; attempt < 8; attempt++) {
      const seq = Atomics.load(c, CTRL_STATS_SEQ);
      if (seq & 1) continue;
      const snap = {
        produced: st[STAT_PRODUCED], miss: st[STAT_MISS], dropped: st[STAT_DROPPED],
        resynced: st[STAT_RESYNCED], expired: st[STAT_EXPIRED],
        lastBlockUs: st[STAT_LAST_BLOCK_US], avgBlockUs: st[STAT_AVG_BLOCK_US],
        gpuNsLast: st[STAT_GPU_NS_LAST], queueSubmits: st[STAT_QUEUE_SUBMITS],
        mapResolves: st[STAT_MAP_RESOLVES], state: st[STAT_STATE],
        primed: st[STAT_PRIMED],
      };
      if (Atomics.load(c, CTRL_STATS_SEQ) === seq) return snap;
    }
    return null;
  }
}
/* ── END gpu-ring core ──────────────────────────────────────────────────────── */

export {
  GpuRing,
  gpuRingLayout as layout,
  gpuRingAllocate as allocate,
  gpuRingAttach as attach,
  GPU_RING_DEFAULTS,
  PGR_MAGIC, PGR_VERSION,
  FLAG_WORKER_READY, FLAG_DEVICE_LOST, FLAG_SHUTDOWN, FLAG_ENGINE_GPU,
  STATE_INIT, STATE_READY, STATE_DEVICE_LOST, STATE_FAILED,
  CTRL_FLAGS, CTRL_IN_WRITE, CTRL_IN_READ, CTRL_OUT_WRITE, CTRL_OUT_READ,
  CTRL_MISS, CTRL_DROPPED, CTRL_RESYNC, CTRL_STATS_SEQ,
};
