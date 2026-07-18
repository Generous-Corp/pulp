// wclap-processor.js — the production worklet-resident WebCLAP host (Architecture A).
//
// A *classic* AudioWorklet script (loaded via `audioWorklet.addModule`), NOT an
// ES module: `addModule` only runs classic scripts, so the ES-module CLAP host
// (core/format/src/wasm/wclap-host.mjs) and the shared ABI table
// (./wclap-abi.mjs) cannot be `import`ed here. This bundle therefore INLINES the
// same CLAP struct offsets / event constants / trampolines as ./wclap-abi.mjs —
// which is the single source of truth; the package test
// test/wclap-abi-parity.test.mjs asserts the two never drift.
//
// It productizes the WS-C2 spike (examples/web-demos/wclap-build/realtime-spike/
// wclap-worklet.js): the whole minimal CLAP host + WASI shim + host-vtable
// trampolines run INSIDE AudioWorkletGlobalScope and render REAL TIME, one
// 128-frame quantum per process() call, with ZERO allocation on the audio thread
// (every wasm buffer, the clap_process_t, and pooled event structs are allocated
// once in prepare()). Beyond the spike it adds, all inside the worklet:
//   • MIDI/sysex IN  — scheduleMidi → clap_event_midi, sendSysex → clap_event_midi_sysex.
//   • param + MIDI OUT — out_events.try_push captures CLAP_EVENT_PARAM_VALUE
//     (onParamsChanged) and note/midi out (onMidiOut). (wclap-host.mjs DROPS these.)
//   • clap.state — save/load via the clap.state extension's ostream/istream
//     vtable, producing/consuming the SAME PLST blob the native builds use.
//   • honest descriptor flags from clap.audio-ports / clap.note-ports.
// Why the whole host lives here: a CLAP plugin's process() calls the host's
// in/out-event vtable SYNCHRONOUSLY during the render; a worklet-side wasm cannot
// synchronously call a main-thread JS vtable. See realtime-spike/DECISION.md.

/* ─────────────────────────── worklet-scope polyfills ─────────────────────── */
// AudioWorkletGlobalScope lacks TextEncoder/TextDecoder/atob; CLAP ids + param
// names are ASCII, so a byte loop is sufficient and never allocates a coder.
function utf8Encode(s) {
  const out = new Uint8Array(s.length + 1); // + NUL
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0x7f;
  return out;
}
function utf8Decode(u8) {
  let s = "";
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return s;
}
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64decode(b64) {
  const lut = b64decode._lut ??= (() => { const t = new Int16Array(256).fill(-1);
    for (let i = 0; i < B64.length; i++) t[B64.charCodeAt(i)] = i; return t; })();
  const clean = b64.replace(/=+$/, "");
  const out = new Uint8Array((clean.length * 6) >> 3);
  let acc = 0, bits = 0, o = 0;
  for (let i = 0; i < clean.length; i++) {
    acc = (acc << 6) | lut[clean.charCodeAt(i)]; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 0xff; }
  }
  return out;
}

/* ───── CLAP ABI — INLINED from ./wclap-abi.mjs (parity test guards drift) ──── */
const CLAP_PLUGIN_FACTORY_ID = "clap.plugin-factory";
const CLAP_EXT_PARAMS = "clap.params";
const CLAP_EXT_STATE = "clap.state";
const CLAP_EXT_AUDIO_PORTS = "clap.audio-ports";
const CLAP_EXT_NOTE_PORTS = "clap.note-ports";
const CLAP_EXT_LOG = "clap.log";
const CLAP_EXT_THREAD_CHECK = "clap.thread-check";
const CLAP_EXT_LATENCY = "clap.latency";
const CLAP_EXT_TAIL = "clap.tail";
const CLAP_CORE_EVENT_SPACE_ID = 0;
const CLAP_EVENT_NOTE_ON = 0;
const CLAP_EVENT_NOTE_OFF = 1;
const CLAP_EVENT_PARAM_VALUE = 5;
const CLAP_EVENT_MIDI = 10;
const CLAP_EVENT_MIDI_SYSEX = 11;
const CLAP_PARAM_IS_STEPPED = 1 << 0;
const PLUGIN = { init: 8, destroy: 12, activate: 16, deactivate: 20, start_processing: 24, stop_processing: 28, reset: 32, process: 36, get_extension: 40, on_main_thread: 44 };
const FACTORY = { count: 0, descriptor: 4, create: 8 };
const ENTRY = { init: 12, get_factory: 20 };
const DESC = { id: 12, name: 16 };
const PARAM_INFO = { size: 1320, id: 0, flags: 4, name: 12, min: 1296, max: 1304, def: 1312 };
const PARAMS_EXT = { count: 0, get_info: 4, get_value: 8, value_to_text: 12, text_to_value: 16, flush: 20 };
const EVENT_HEADER = { size: 0, time: 4, space_id: 8, type: 10, flags: 12 };
const PARAM_EVENT = { size: 48, param_id: 16, cookie: 20, note_id: 24, port: 28, channel: 30, key: 32, value: 40 };
const NOTE_EVENT = { size: 40, note_id: 16, port: 20, channel: 22, key: 24, velocity: 32 };
const MIDI_EVENT = { size: 24, port: 16, data: 18 };
const SYSEX_EVENT = { size: 28, port: 16, buffer: 20, bytes: 24 };
const IN_EVENTS = { size: 12, ctx: 0, count: 4, get: 8 };
const OUT_EVENTS = { size: 8, ctx: 0, try_push: 4 };
const PROCESS = { size: 40, frames: 8, transport: 12, audio_in: 16, audio_out: 20, in_count: 24, out_count: 28, in_events: 32, out_events: 36 };
const AUDIO_BUFFER = { size: 24, data32: 0, data64: 4, channels: 8, latency: 12, constant_mask: 16 };
const HOST = { size: 48, name: 16, vendor: 20, url: 24, version: 28, get_extension: 32, request_restart: 36, request_process: 40, request_callback: 44 };
const STATE_EXT = { save: 0, load: 4 };
const OSTREAM = { size: 8, ctx: 0, write: 4 };
const ISTREAM = { size: 8, ctx: 0, read: 4 };
const PLUGIN_LATENCY = { get: 0 };
const PLUGIN_TAIL = { get: 0 };
const HOST_LOG = { size: 4, log: 0 };
const HOST_THREAD_CHECK = { size: 8, is_main_thread: 0, is_audio_thread: 4 };
const HOST_LATENCY = { size: 4, changed: 0 };
const HOST_TAIL = { size: 4, changed: 0 };
const HOST_STATE = { size: 4, mark_dirty: 0 };
const HOST_PARAMS = { size: 12, rescan: 0, clear: 4, request_flush: 8 };
const TRAMPOLINES = {
  "ii->i": "AGFzbQEAAAABBwFgAn9/AX8CBwEBaAFmAAADAgEABwYBAmZuAAEKCgEIACAAIAEQAAs=",
  "i->i": "AGFzbQEAAAABBgFgAX8BfwIHAQFoAWYAAAMCAQAHBgECZm4AAQoIAQYAIAAQAAs=",
  "i->": "AGFzbQEAAAABBQFgAX8AAgcBAWgBZgAAAwIBAAcGAQJmbgABCggBBgAgABAACw==",
  "iiI->I": "AGFzbQEAAAABCAFgA39/fgF+AgcBAWgBZgAAAwIBAAcGAQJmbgABCgwBCgAgACABIAIQAAs=",
  "ii->": "AGFzbQEAAAABBgFgAn9/AAIHAQFoAWYAAAMCAQAHBgECZm4AAQoKAQgAIAAgARAACw==",
  "iii->": "AGFzbQEAAAABBwFgA39/fwACBwEBaAFmAAADAgEABwYBAmZuAAEKDAEKACAAIAEgAhAACw==",
};
/* ────────────────────────── end inlined ABI block ──────────────────────────── */

/* ───── GPU-audio SAB ring — INLINED from examples/web-demos/gpu-audio/js/gpu-ring.mjs.
   The GPU DSP runs in a DedicatedWorker (an AudioWorkletProcessor cannot touch
   navigator.gpu); the two meet only in this SharedArrayBuffer. Byte-identical to
   the module source — test/gpu-ring-parity.test.mjs guards the drift. Wholly INERT
   unless processorOptions.gpuSab is passed. ───────────────────────────────────── */
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
const STAT_RECOMMENDED_DEPTH = 12;  // adaptive: block-periods the round trip wants
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

  // Adaptive depth: retarget the pipeline latency live (page thread). Both sides read
  // CTRL_LATENCY live — pop() (worklet) asks for the block this many old, the worker
  // budgets this many block-periods — so writing it here moves the WHOLE JS transport
  // to the new depth atomically. The plugin's own L is moved in lockstep by the
  // worklet calling pulp_sc_set_pipeline_depth (see wclap-processor pollDepth); the two
  // MUST move together or the CPU-net wet stops covering a missed GPU block. Clamped so
  // latencyBlocks stays < slots (the ring is pre-sized to the max depth).
  setLatency(L) {
    let v = L | 0;
    if (v < 1) v = 1;
    if (v >= this.slots) v = this.slots - 1;
    Atomics.store(this.ctrl, CTRL_LATENCY, v);
    this.latencyBlocks = v;
    return v;
  }

  // The current pipeline latency, read live from the control block — used by the
  // worklet's pop() and the worker's deadline budget so a page-driven depth change
  // reaches both sides without re-attaching either.
  liveLatency() { return Atomics.load(this.ctrl, CTRL_LATENCY) >>> 0; }

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
    // Read the latency LIVE from the control block: adaptive depth changes it from
    // the page (setLatency), and the pop must ask for the block the CURRENT depth is
    // owed or the JS transport and the plugin's L drift apart. One atomic load per
    // block — wait-free, audio-thread-safe. The slots are pre-sized to the max depth
    // so a deeper L never needs a bigger ring.
    const L = Atomics.load(c, CTRL_LATENCY) >>> 0;
    const want = (this.blockIndex - L) | 0;
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
    st[STAT_RECOMMENDED_DEPTH] = s.recommendedDepth || 0;
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
        primed: st[STAT_PRIMED], recommendedDepth: st[STAT_RECOMMENDED_DEPTH],
      };
      if (Atomics.load(c, CTRL_STATS_SEQ) === seq) return snap;
    }
    return null;
  }
}
/* ── END gpu-ring core ──────────────────────────────────────────────────────── */

const _trampolineModuleCache = {};
function trampolineModule(key) {
  if (!_trampolineModuleCache[key]) {
    if (!TRAMPOLINES[key]) throw new Error("no WebCLAP host trampoline for '" + key + "'");
    _trampolineModuleCache[key] = new WebAssembly.Module(b64decode(TRAMPOLINES[key]));
  }
  return _trampolineModuleCache[key];
}

/* ─────────────────────────── WASI shim (from wclap-wasi.mjs) ──────────────── */
function makeWasiImports(getMemory, onText) {
  const dv = () => new DataView(getMemory().buffer);
  const fd_write = (fd, iovsPtr, iovsLen, nwrittenPtr) => {
    const view = dv();
    let total = 0; const chunks = [];
    for (let i = 0; i < iovsLen; i++) {
      const base = view.getUint32(iovsPtr + i * 8, true);
      const len = view.getUint32(iovsPtr + i * 8 + 4, true);
      total += len;
      if (onText && (fd === 1 || fd === 2) && len > 0) {
        chunks.push(new Uint8Array(getMemory().buffer, base, len).slice());
      }
    }
    view.setUint32(nwrittenPtr, total, true); // CRITICAL: else libc write loop spins
    if (chunks.length && onText) { let t = ""; for (const c of chunks) t += utf8Decode(c); onText(fd, t); }
    return 0;
  };
  const wasi = {
    fd_write,
    proc_exit: (code) => { throw new Error("wasi proc_exit(" + code + ")"); },
    environ_sizes_get: (c, s) => { const v = dv(); v.setUint32(c, 0, true); v.setUint32(s, 0, true); return 0; },
    environ_get: () => 0, clock_time_get: () => 0, fd_close: () => 0, fd_seek: () => 0,
    fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8, path_readlink: () => 8, sched_yield: () => 0,
  };
  return new Proxy(wasi, { get: (t, p) => (p in t ? t[p] : (() => 0)) });
}

/* ─────────────────── worklet-resident host + real-time plugin ─────────────── */
class WorkletWclapHost {
  constructor(onLog, hooks = {}, gpuRing = null) {
    this.onLog = onLog;
    this.hooks = hooks;            // { onLatencyChanged, onTailChanged, onStateDirty, onParamsRescan, onRequestRestart }
    this.gpuRing = gpuRing;        // null unless processorOptions.gpuSab was passed
    this.memory = new WebAssembly.Memory({ initial: 512, maximum: 16384, shared: true });
    this.instance = null; this.ex = null;
    this.currentPlugin = null;     // set by createPlugin; host callbacks reach the plugin through it
    // clap.thread-check state: true only while the plugin's process() is on the
    // stack. The deferred main-thread callback (request_callback → on_main_thread)
    // runs with this false, so the plugin's is_main_thread() assertions hold.
    this._inAudioThread = false;
    this._mainThreadCallbackPending = false;
    this._hostExtById = null;      // id → vtable ptr, built once in _buildHost
  }
  instantiateSync(bytes) {
    // Posting a WebAssembly.Module INTO an AudioWorklet is silently dropped in
    // Chrome, so the main thread transfers raw bytes and we compile HERE. A
    // synchronous multi-MB `new WebAssembly.Module` is permitted in
    // AudioWorkletGlobalScope (unlike the main thread's 4 KB sync-compile cap).
    const module = bytes instanceof WebAssembly.Module ? bytes : new WebAssembly.Module(bytes);
    this.instance = new WebAssembly.Instance(module, {
      env: {
        memory: this.memory,
        // The GPU seam. A plugin built with a GPU engine imports this; one built
        // without it never does, and the extra import is simply unused (a wasm
        // module that DOES import it would LinkError if we omitted it, so it is
        // always provided — with no ring attached it returns 0 = miss, and the
        // plugin falls back to its CPU path, which is the default engine anyway).
        pulp_gpu_xfer: (inPtr, outPtr, frames, channels) =>
          this._gpuXfer(inPtr, outPtr, frames, channels),
      },
      wasi_snapshot_preview1: makeWasiImports(() => this.memory, (fd, t) => this.onLog && this.onLog(fd, t)),
    });
    this.ex = this.instance.exports;
    this.ex._initialize();
    return this;
  }
  // Cache a single DataView over the wasm heap. A fresh `new DataView` per
  // accessor call was dozens of short-lived allocations per render quantum on
  // the audio thread (every u32/u16/f64/setU32/… routes through this). Rebuild
  // only when the buffer changes: memory.grow() detaches and replaces a
  // non-shared buffer (identity changes) and extends a shared one (byteLength
  // changes), so covering both keeps a grown heap from being read through a
  // stale view while allocating zero DataViews in steady state.
  get _dv() {
    const buf = this.memory.buffer;
    if (buf !== this._dvBuffer || buf.byteLength !== this._dvByteLength) {
      this._dvBuffer = buf;
      this._dvByteLength = buf.byteLength;
      this._dvView = new DataView(buf);
    }
    return this._dvView;
  }
  // Float32 view over the same heap, cached on the same terms as _dv (a grown
  // shared memory keeps the buffer identity but changes byteLength).
  get _f32() {
    const buf = this.memory.buffer;
    if (buf !== this._f32Buffer || buf.byteLength !== this._f32ByteLength) {
      this._f32Buffer = buf;
      this._f32ByteLength = buf.byteLength;
      this._f32View = new Float32Array(buf);
    }
    return this._f32View;
  }

  // env.pulp_gpu_xfer(inPtr, outPtr, frames, channels) → 1 wet delivered / 0 miss.
  //
  // The plugin's GPU engine calls this from its process(): it hands over one
  // block of dry planar audio ([ch0 frames][ch1 frames] at inPtr) and takes back
  // the block the GPU worker produced `gpuLatencyBlocks` blocks ago (at outPtr).
  // Synchronous, allocation-free, lock-free, and it NEVER blocks or posts a
  // message — a full input ring drops the block and a ring with no wet for this
  // block index returns 0, which is the plugin's cue to substitute its own (CPU)
  // result. A missed GPU deadline is normal here, not exceptional.
  //
  // BOTH dimensions are checked, and `channels` is in the ABI for exactly that
  // reason: the ring writes ring.channels * ring.blockSize floats into the
  // plugin's heap at outPtr, and the plugin's staging buffer is sized for ITS
  // channel count. Validating only `frames` would leave the one dimension that can
  // overrun the plugin's buffer — a heap corruption from the render callback —
  // unchecked. A mismatch refuses the lane (0 = miss) rather than writing.
  _gpuXfer(inPtr, outPtr, frames, channels) {
    const ring = this.gpuRing;
    if (!ring || frames !== ring.blockSize || channels !== ring.channels) return 0;
    const heap = this._f32;
    ring.push(heap, inPtr >> 2);
    return ring.pop(heap, outPtr >> 2);
  }

  u32(p) { return this._dv.getUint32(p, true); }
  setU32(p, v) { this._dv.setUint32(p, v, true); }
  u16(p) { return this._dv.getUint16(p, true); }
  setU16(p, v) { this._dv.setUint16(p, v, true); }
  f64(p) { return this._dv.getFloat64(p, true); }
  setF64(p, v) { this._dv.setFloat64(p, v, true); }
  call(idx, ...a) { return this.ex.__indirect_function_table.get(idx)(...a); }
  cstr(s) {
    const bytes = utf8Encode(s);
    const p = this.ex.malloc(bytes.length);
    new Uint8Array(this.memory.buffer, p, bytes.length).set(bytes);
    return p;
  }
  readCstr(p, limit = 4096) {
    if (!p) return "";
    const u8 = new Uint8Array(this.memory.buffer);
    let e = p; const end = p + limit;
    while (e < end && u8[e]) e++;
    return utf8Decode(u8.slice(p, e));
  }
  _addFn(key, fn) {
    const inst = new WebAssembly.Instance(trampolineModule(key), { h: { f: fn } });
    const tbl = this.ex.__indirect_function_table;
    const idx = tbl.length; tbl.grow(1); tbl.set(idx, inst.exports.fn);
    return idx;
  }
  _buildHost() {
    const h = this.ex.malloc(HOST.size);
    this.setU32(h + 0, 1); this.setU32(h + 4, 2); this.setU32(h + 8, 2); // clap_version 1.2.2
    this.setU32(h + 12, 0);                                              // host_data
    this.setU32(h + HOST.name, this.cstr("Pulp WebCLAP RT Host"));
    this.setU32(h + HOST.vendor, this.cstr("Pulp"));
    this.setU32(h + HOST.url, this.cstr("https://github.com/danielraffel/pulp"));
    this.setU32(h + HOST.version, this.cstr("0.0.1"));

    // Host-provided extensions (parity with the native ClapSlot host services a
    // Pulp CLAP plugin actually queries). Every vtable is built ONCE here — never
    // during on_main_thread / process — so get_extension is an allocation-free id
    // lookup and no host callback grows the wasm table on the audio thread.
    this._hostExtById = {
      [CLAP_EXT_LOG]: this._buildHostLog(),
      [CLAP_EXT_THREAD_CHECK]: this._buildHostThreadCheck(),
      [CLAP_EXT_LATENCY]: this._buildHostLatency(),
      [CLAP_EXT_TAIL]: this._buildHostTail(),
      [CLAP_EXT_STATE]: this._buildHostState(),
      [CLAP_EXT_PARAMS]: this._buildHostParams(),
    };
    this.setU32(h + HOST.get_extension, this._addFn("ii->i",
      (_host, idPtr) => this._hostExtById[this.readCstr(idPtr, 64)] || 0));

    // request_restart: a Pulp plugin asks for one when latency changes while
    // active (latency may only change across (de)activate). The shared player
    // treats a running demo as fixed-latency, so we surface it to the app (which
    // can rebuild) rather than tear the graph down mid-render.
    this.setU32(h + HOST.request_restart, this._addFn("i->",
      () => this.hooks.onRequestRestart && this.hooks.onRequestRestart()));
    // request_process: the worklet renders every quantum unconditionally, so a
    // request to be scheduled for processing is already satisfied — no-op.
    this.setU32(h + HOST.request_process, this._addFn("i->", () => {}));
    // request_callback: defer plugin.on_main_thread() until the current
    // process() unwinds (see processQuantum). Called on the audio thread; only
    // flips a flag, allocates nothing.
    this.setU32(h + HOST.request_callback, this._addFn("i->",
      () => { this._mainThreadCallbackPending = true; }));
    return h;
  }

  // ── host-extension vtables (each allocated + wired once) ───────────────────
  _buildHostLog() {
    const p = this.ex.malloc(HOST_LOG.size);
    this.setU32(p + HOST_LOG.log, this._addFn("iii->", (_host, severity, msgPtr) => {
      // Route plugin diagnostics through the same channel as wasm stdio; map
      // CLAP severity (>=ERROR) onto fd 2 so the app can flag it.
      if (this.onLog) this.onLog(severity >= 3 ? 2 : 1, "[clap] " + this.readCstr(msgPtr, 1024));
    }));
    return p;
  }
  _buildHostThreadCheck() {
    const p = this.ex.malloc(HOST_THREAD_CHECK.size);
    this.setU32(p + HOST_THREAD_CHECK.is_main_thread, this._addFn("i->i", () => (this._inAudioThread ? 0 : 1)));
    this.setU32(p + HOST_THREAD_CHECK.is_audio_thread, this._addFn("i->i", () => (this._inAudioThread ? 1 : 0)));
    return p;
  }
  _buildHostLatency() {
    const p = this.ex.malloc(HOST_LATENCY.size);
    this.setU32(p + HOST_LATENCY.changed, this._addFn("i->", () => {
      const s = this.currentPlugin ? this.currentPlugin.currentLatency() : 0;
      this.hooks.onLatencyChanged && this.hooks.onLatencyChanged(s);
    }));
    return p;
  }
  _buildHostTail() {
    const p = this.ex.malloc(HOST_TAIL.size);
    this.setU32(p + HOST_TAIL.changed, this._addFn("i->", () => {
      const s = this.currentPlugin ? this.currentPlugin.currentTail() : 0;
      this.hooks.onTailChanged && this.hooks.onTailChanged(s);
    }));
    return p;
  }
  _buildHostState() {
    const p = this.ex.malloc(HOST_STATE.size);
    this.setU32(p + HOST_STATE.mark_dirty, this._addFn("i->", () => {
      this.hooks.onStateDirty && this.hooks.onStateDirty();
    }));
    return p;
  }
  _buildHostParams() {
    const p = this.ex.malloc(HOST_PARAMS.size);
    this.setU32(p + HOST_PARAMS.rescan, this._addFn("ii->", (_host, flags) => {
      this.hooks.onParamsRescan && this.hooks.onParamsRescan(flags);
    }));
    // clear(host, param_id, flags): drop host-side references to a param. The
    // web host holds none beyond the id list the app owns, so nothing to clear.
    this.setU32(p + HOST_PARAMS.clear, this._addFn("iii->", () => {}));
    // request_flush(host): the plugin wants its output param events delivered
    // even when idle. The worklet processes every quantum and already captures
    // out_events each block, so the flush is inherently satisfied — no-op.
    this.setU32(p + HOST_PARAMS.request_flush, this._addFn("i->", () => {}));
    return p;
  }
  createPlugin(index = 0) {
    const entry = this.ex.clap_entry.value;
    if (!this.call(this.u32(entry + ENTRY.init), 0)) throw new Error("clap_entry.init() failed");
    const factory = this.call(this.u32(entry + ENTRY.get_factory), this.cstr(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) throw new Error("get_factory returned null");
    const count = this.call(this.u32(factory + FACTORY.count), factory);
    if (index >= count) throw new Error("plugin index out of range");
    const desc = this.call(this.u32(factory + FACTORY.descriptor), factory, index);
    const id = this.readCstr(this.u32(desc + DESC.id));
    const name = this.readCstr(this.u32(desc + DESC.name));
    const ptr = this.call(this.u32(factory + FACTORY.create), factory, this._buildHost(), this.cstr(id));
    if (!ptr) throw new Error("create_plugin returned null");
    const plugin = new RealtimeWclapPlugin(this, ptr, { id, name });
    this.currentPlugin = plugin;   // host latency/tail callbacks read the live plugin
    return plugin;
  }
}

// A CLAP plugin driven in real time: prepare() allocates every wasm buffer +
// event-struct pool ONCE and calls start_processing once; processQuantum() does
// no allocation.
class RealtimeWclapPlugin {
  constructor(host, ptr, descriptor) {
    this.host = host; this.ptr = ptr; this.descriptor = descriptor;
    this.channels = 2; this.maxFrames = 128;
    this._paramPool = []; this._midiPool = []; this._sysexPool = []; this._sysexBufs = [];
    this._curEvents = [];       // pooled event pointers handed to the plugin this quantum
    this._outParamEvents = [];  // captured host-visible param changes
    this._outMidi = [];         // captured host-visible note/midi output
    this._stateReady = false;
  }
  _fn(off) { return this.host.u32(this.ptr + off); }
  init() { if (!this.host.call(this._fn(PLUGIN.init), this.ptr)) throw new Error("init failed"); return this; }
  activate(sr, minF, maxF) {
    if (!this.host.call(this._fn(PLUGIN.activate), this.ptr, sr, minF, maxF)) throw new Error("activate failed");
    return this;
  }
  _ext(id) { return this.host.call(this._fn(PLUGIN.get_extension), this.ptr, this.host.cstr(id)); }

  // Honest descriptor flags from clap.audio-ports / clap.note-ports.
  descriptorFlags() {
    const h = this.host;
    const portCount = (extId) => {
      const ext = this._ext(extId);
      if (!ext) return { in: 0, out: 0 };
      // clap_plugin_{audio,note}_ports_t.count(plugin, is_input) is at offset 0.
      return { in: h.call(h.u32(ext + 0), this.ptr, 1), out: h.call(h.u32(ext + 0), this.ptr, 0) };
    };
    const audio = portCount(CLAP_EXT_AUDIO_PORTS);
    const note = portCount(CLAP_EXT_NOTE_PORTS);
    const hasAudioInput = audio.in > 0, hasAudioOutput = audio.out > 0;
    const hasMidiInput = note.in > 0, hasMidiOutput = note.out > 0;
    // An instrument takes notes and makes audio without needing an audio input.
    const isInstrument = hasMidiInput && hasAudioOutput && !hasAudioInput;
    return { hasAudioInput, hasAudioOutput, hasMidiInput, hasMidiOutput, isInstrument };
  }

  // Render a parameter value the way a CLAP host displays it, via the plugin's
  // clap_plugin_params.value_to_text ("35.00 %"). This is the ONLY display source
  // the CLAP ABI defines — clap_param_info has no unit field. Main-thread op (it
  // runs in the worklet's message loop, never inside process()); the scratch
  // buffer is allocated once and reused so a later on-demand call stays cheap.
  valueToText(id, value) {
    const h = this.host;
    if (!this._paramsExt) return null;
    const fn = h.u32(this._paramsExt + PARAMS_EXT.value_to_text);
    if (!fn) return null;
    if (!this._textBuf) this._textBuf = h.ex.malloc(256);
    if (!h.call(fn, this.ptr, id, value, this._textBuf, 256)) return null;
    return h.readCstr(this._textBuf, 256);
  }

  params() {
    const h = this.host;
    const ext = this._paramsExt = this._ext(CLAP_EXT_PARAMS);
    if (!ext) return (this._paramIds = []);
    const count = h.call(h.u32(ext + PARAMS_EXT.count), this.ptr);
    const buf = h.ex.malloc(PARAM_INFO.size);
    const out = [];
    for (let i = 0; i < count; i++) {
      if (!h.call(h.u32(ext + PARAMS_EXT.get_info), this.ptr, i, buf)) continue;
      const min = h.f64(buf + PARAM_INFO.min), max = h.f64(buf + PARAM_INFO.max);
      const flags = h.u32(buf + PARAM_INFO.flags);
      const stepped = (flags & CLAP_PARAM_IS_STEPPED) !== 0;
      const id = h.u32(buf + PARAM_INFO.id);
      const def = h.f64(buf + PARAM_INFO.def);
      // Two value_to_text probes at DISTINCT values; the adapter recovers the
      // display unit from them (deriveDisplayUnit in wclap-abi.mjs), because
      // clap_param_info carries no unit and the UI formats numbers itself.
      const other = max !== def ? max : min;
      const pname = h.readCstr(buf + PARAM_INFO.name, 256);
      // Which parameter selects the offload engine. Captured HERE because this is the one
      // place the host sees parameter NAMES; everything downstream is ids and floats.
      if (/^engine$/i.test(pname)) this._engineParamId = id;
      out.push({
        id,
        name: pname,
        min, max, default: def,
        stepped, boolean: stepped && (max - min) === 1,
        textProbes: [{ value: def, text: this.valueToText(id, def) },
                     { value: other, text: this.valueToText(id, other) }],
      });
    }
    h.ex.free(buf);
    this._paramIds = out.map((p) => p.id);
    return out;
  }

  // Read the plugin's CURRENT parameter values via clap.params.get_value (offset
  // 8). Used after a state load to re-sync the host mirror + UI (the plugin does
  // not emit param-out events on load). Returns [{id,value}].
  readParamValues() {
    const h = this.host;
    if (!this._paramsExt || !this._paramIds) return [];
    if (!this._pvBuf) this._pvBuf = h.ex.malloc(8); // reusable double scratch
    const out = [];
    for (const id of this._paramIds) {
      if (h.call(h.u32(this._paramsExt + PARAMS_EXT.get_value), this.ptr, id, this._pvBuf)) {
        out.push({ id, value: h.f64(this._pvBuf) });
      }
    }
    return out;
  }

  // Run the plugin's on_main_thread handler. A Pulp CLAP plugin drains its
  // pending latency/tail-change flags here and calls the host's
  // clap_host_latency/tail.changed(), which the host forwards to the app. This
  // is invoked by processQuantum AFTER process() unwinds (see request_callback),
  // so it is never reentrant with process().
  onMainThread() { this.host.call(this._fn(PLUGIN.on_main_thread), this.ptr); }

  // ── Optional: a module that publishes its live impulse response ──────────────
  //
  // A convolution plugin whose DSP can also run somewhere OUTSIDE the worklet (the
  // GPU worker, which an AudioWorkletGlobalScope cannot even reach) has to tell that
  // somewhere WHICH impulse response to convolve with — and it must be the IR the
  // plugin actually ended up with, after its own normalize/window pass, or the two
  // engines are different reverbs and neither can stand in for the other.
  //
  // Three optional wasm exports carry it. Entirely opt-in: a module without them is
  // untouched, and this whole path costs one `undefined` check.
  //
  // Polled right after on_main_thread(), which is the ONLY place the plugin can change
  // its IR (the rebuild is off-audio-thread work; see non_realtime_tick_pending), so
  // there is no per-quantum cost and no polling loop. The generation counter is the
  // cheap question; the snapshot is the expensive one, and only a changed generation
  // asks it.
  pollIr() {
    const ex = this.host.ex;
    if (this._irGen === undefined) {
      this._irGen = ex.pulp_ir_generation ? 0 : -1;   // -1 = this module has no IR to publish
    }
    if (this._irGen < 0) return null;

    const gen = ex.pulp_ir_generation() >>> 0;
    if (gen === this._irGen) return null;
    this._irGen = gen;

    const len = ex.pulp_ir_snapshot() >>> 0;
    if (!len) return null;
    // Copied, not a view: the wasm heap can be grown or rewritten by the next call,
    // and this is about to cross a thread boundary.
    return new Float32Array(this.host.memory.buffer, ex.pulp_ir_data(), len).slice();
  }

  // Adaptive GPU pipeline depth: the page's DepthController retargets the ring's
  // latency (CTRL_LATENCY, via setLatency) when the measured round trip warrants it;
  // this moves the PLUGIN's L to match, on the non-RT tick — never inside process() —
  // exactly like pollIr. The JS transport (pop/worker budget) reads the latency live,
  // so calling pulp_sc_set_pipeline_depth here keeps the plugin's CPU-net wet delay in
  // lockstep with the transport. A no-op when the module lacks the export (only the
  // GPU SuperConvolver has it) or when the depth has not changed. Cheap: one atomic
  // load, and a wasm call only on an actual change.
  pollDepth() {
    const ex = this.host.ex;
    const ring = this.host.gpuRing;
    if (this._depthExport === undefined) {
      this._depthExport =
        (ring && typeof ex.pulp_sc_set_pipeline_depth === "function")
          ? ex.pulp_sc_set_pipeline_depth : null;
    }
    if (!this._depthExport || !ring) return;
    const L = ring.liveLatency() >>> 0;
    if (L === this._appliedDepth) return;
    this._appliedDepth = L;
    this._depthExport(L);
  }

  // Current plugin-reported latency / tail in samples (clap.latency / clap.tail
  // plugin extensions). Extension pointers are resolved + cached once; a plugin
  // that does not expose the extension reports 0. `>>> 0` keeps the uint32 ABI.
  currentLatency() {
    const h = this.host;
    if (this._latencyExt === undefined) this._latencyExt = this._ext(CLAP_EXT_LATENCY) || 0;
    if (!this._latencyExt) return 0;
    return h.call(h.u32(this._latencyExt + PLUGIN_LATENCY.get), this.ptr) >>> 0;
  }
  currentTail() {
    const h = this.host;
    if (this._tailExt === undefined) this._tailExt = this._ext(CLAP_EXT_TAIL) || 0;
    if (!this._tailExt) return 0;
    return h.call(h.u32(this._tailExt + PLUGIN_TAIL.get), this.ptr) >>> 0;
  }

  // Allocate every wasm structure ONCE. After this, processQuantum() is
  // allocation-free and audio-thread-safe.
  prepare(channels, maxFrames) {
    const h = this.host;
    this.channels = channels; this.maxFrames = maxFrames;

    const chBuf = (arr) => { const p = h.ex.malloc(maxFrames * 4); arr.push(p); return p; };
    this._inCh = []; this._outCh = [];
    const inPtrs = [], outPtrs = [];
    for (let c = 0; c < channels; c++) { inPtrs.push(chBuf(this._inCh)); outPtrs.push(chBuf(this._outCh)); }

    const ptrArray = (ptrs) => { const p = h.ex.malloc(ptrs.length * 4);
      ptrs.forEach((q, i) => h.setU32(p + i * 4, q)); return p; };
    const audioBuf = (chPtrs) => { const p = h.ex.malloc(AUDIO_BUFFER.size);
      h.setU32(p + AUDIO_BUFFER.data32, ptrArray(chPtrs)); h.setU32(p + AUDIO_BUFFER.data64, 0);
      h.setU32(p + AUDIO_BUFFER.channels, chPtrs.length); h.setU32(p + AUDIO_BUFFER.latency, 0);
      h.setU32(p + AUDIO_BUFFER.constant_mask, 0); h.setU32(p + AUDIO_BUFFER.constant_mask + 4, 0); return p; };
    this._inBuf = audioBuf(inPtrs);
    this._outBuf = audioBuf(outPtrs);

    // Reusable in/out event lists. get()/size() index this._curEvents; try_push
    // CAPTURES param + note/midi output events (the "drops output events" fix).
    this._inEvents = h.ex.malloc(IN_EVENTS.size);
    h.setU32(this._inEvents + IN_EVENTS.ctx, 0);
    h.setU32(this._inEvents + IN_EVENTS.count, h._addFn("i->i", () => this._curEvents.length));
    h.setU32(this._inEvents + IN_EVENTS.get, h._addFn("ii->i", (_c, i) => this._curEvents[i] ?? 0));
    this._outEvents = h.ex.malloc(OUT_EVENTS.size);
    h.setU32(this._outEvents + OUT_EVENTS.ctx, 0);
    h.setU32(this._outEvents + OUT_EVENTS.try_push, h._addFn("ii->i", (_c, evPtr) => {
      const type = h.u16(evPtr + EVENT_HEADER.type);
      if (type === CLAP_EVENT_PARAM_VALUE) {
        this._outParamEvents.push({ id: h.u32(evPtr + PARAM_EVENT.param_id), value: h.f64(evPtr + PARAM_EVENT.value) });
      } else if (type === CLAP_EVENT_NOTE_ON || type === CLAP_EVENT_NOTE_OFF) {
        const ch = h.u16(evPtr + NOTE_EVENT.channel) & 0x0f, key = h.u16(evPtr + NOTE_EVENT.key) & 0x7f;
        const vel = Math.max(0, Math.min(127, Math.round(h.f64(evPtr + NOTE_EVENT.velocity) * 127)));
        this._outMidi.push({ bytes: type === CLAP_EVENT_NOTE_ON ? [0x90 | ch, key, vel] : [0x80 | ch, key, 0] });
      } else if (type === CLAP_EVENT_MIDI) {
        this._outMidi.push({ bytes: [
          h._dv.getUint8(evPtr + MIDI_EVENT.data),
          h._dv.getUint8(evPtr + MIDI_EVENT.data + 1),
          h._dv.getUint8(evPtr + MIDI_EVENT.data + 2)] });
      }
      return 1; // accepted
    }));

    // clap_process_t, allocated once and reused.
    this._proc = h.ex.malloc(PROCESS.size);
    h.setU32(this._proc + 0, 0); h.setU32(this._proc + 4, 0);           // steady_time i64
    h.setU32(this._proc + PROCESS.frames, maxFrames);
    h.setU32(this._proc + PROCESS.transport, 0);
    h.setU32(this._proc + PROCESS.audio_in, this._inBuf);
    h.setU32(this._proc + PROCESS.audio_out, this._outBuf);
    h.setU32(this._proc + PROCESS.in_count, 1); h.setU32(this._proc + PROCESS.out_count, 1);
    h.setU32(this._proc + PROCESS.in_events, this._inEvents);
    h.setU32(this._proc + PROCESS.out_events, this._outEvents);

    // Pre-built event-struct pools (filled per quantum, never malloc'd there).
    for (let i = 0; i < 16; i++) this._paramPool.push(h.ex.malloc(PARAM_EVENT.size));
    for (let i = 0; i < 64; i++) this._midiPool.push(h.ex.malloc(MIDI_EVENT.size));
    for (let i = 0; i < 4; i++) {
      this._sysexPool.push(h.ex.malloc(SYSEX_EVENT.size));
      this._sysexBufs.push(h.ex.malloc(512)); // per-slot payload scratch
    }

    // Resolve the clap.latency / clap.tail plugin-extension pointers now (they
    // malloc a cstr for the id) so currentLatency()/currentTail() — which the
    // host's changed() callbacks invoke on the audio thread — never allocate.
    this.currentLatency();
    this.currentTail();

    if (!h.call(this._fn(PLUGIN.start_processing), this.ptr)) throw new Error("start_processing failed");
    this._started = true;
    return this;
  }

  _writeParamEvent(slot, id, value) {
    const h = this.host, e = this._paramPool[slot];
    h.setU32(e + EVENT_HEADER.size, PARAM_EVENT.size); h.setU32(e + EVENT_HEADER.time, 0);
    h.setU16(e + EVENT_HEADER.space_id, CLAP_CORE_EVENT_SPACE_ID);
    h.setU16(e + EVENT_HEADER.type, CLAP_EVENT_PARAM_VALUE);
    h.setU32(e + EVENT_HEADER.flags, 0);
    h.setU32(e + PARAM_EVENT.param_id, id); h.setU32(e + PARAM_EVENT.cookie, 0);
    h._dv.setInt32(e + PARAM_EVENT.note_id, -1, true);
    h._dv.setInt16(e + PARAM_EVENT.port, -1, true);
    h._dv.setInt16(e + PARAM_EVENT.channel, -1, true);
    h._dv.setInt16(e + PARAM_EVENT.key, -1, true);
    h.setF64(e + PARAM_EVENT.value, value);
    return e;
  }
  _writeMidiEvent(slot, bytes) {
    const h = this.host, e = this._midiPool[slot];
    h.setU32(e + EVENT_HEADER.size, MIDI_EVENT.size); h.setU32(e + EVENT_HEADER.time, 0);
    h.setU16(e + EVENT_HEADER.space_id, CLAP_CORE_EVENT_SPACE_ID);
    h.setU16(e + EVENT_HEADER.type, CLAP_EVENT_MIDI);
    h.setU32(e + EVENT_HEADER.flags, 0);
    h.setU16(e + MIDI_EVENT.port, 0);
    h._dv.setUint8(e + MIDI_EVENT.data, bytes[0] & 0xff);
    h._dv.setUint8(e + MIDI_EVENT.data + 1, bytes[1] & 0xff);
    h._dv.setUint8(e + MIDI_EVENT.data + 2, bytes[2] & 0xff);
    return e;
  }
  _writeSysexEvent(slot, bytes) {
    const h = this.host, e = this._sysexPool[slot], buf = this._sysexBufs[slot];
    const n = Math.min(bytes.length, 512);
    new Uint8Array(h.memory.buffer, buf, n).set(bytes.subarray ? bytes.subarray(0, n) : bytes.slice(0, n));
    h.setU32(e + EVENT_HEADER.size, SYSEX_EVENT.size); h.setU32(e + EVENT_HEADER.time, 0);
    h.setU16(e + EVENT_HEADER.space_id, CLAP_CORE_EVENT_SPACE_ID);
    h.setU16(e + EVENT_HEADER.type, CLAP_EVENT_MIDI_SYSEX);
    h.setU32(e + EVENT_HEADER.flags, 0);
    h.setU16(e + SYSEX_EVENT.port, 0);
    h.setU32(e + SYSEX_EVENT.buffer, buf); h.setU32(e + SYSEX_EVENT.bytes, n);
    return e;
  }

  // Render ONE quantum. inputChannels: Float32Array[]; paramEvents [{id,value}];
  // midiEvents [[status,d1,d2]]; sysexEvents [Uint8Array]. Writes outputChannels.
  // Returns { params: [...], midi: [...] } captured host-visible output events.
  processQuantum(inputChannels, frames, paramEvents, midiEvents, sysexEvents, outputChannels) {
    const h = this.host;
    for (let c = 0; c < this.channels; c++) {
      const src = inputChannels[c] || inputChannels[0];
      new Float32Array(h.memory.buffer, this._inCh[c], frames).set(src.subarray(0, frames));
    }
    // Fill pooled event structs (all latched at frame 0). Reuse this._curEvents
    // in place (length reset + push) so no fresh array is allocated per quantum.
    const cur = this._curEvents; cur.length = 0;
    const np = Math.min(paramEvents.length, this._paramPool.length);
    for (let i = 0; i < np; i++) cur.push(this._writeParamEvent(i, paramEvents[i].id, paramEvents[i].value));
    const nm = Math.min(midiEvents.length, this._midiPool.length);
    for (let i = 0; i < nm; i++) cur.push(this._writeMidiEvent(i, midiEvents[i]));
    const ns = Math.min(sysexEvents.length, this._sysexPool.length);
    for (let i = 0; i < ns; i++) cur.push(this._writeSysexEvent(i, sysexEvents[i]));

    h.setU32(this._proc + PROCESS.frames, frames);
    this._outParamEvents.length = 0; this._outMidi.length = 0;

    // Mark the audio thread across the plugin's process() so clap.thread-check
    // answers is_audio_thread() correctly for any RT assertions inside it.
    h._inAudioThread = true;
    const status = h.call(this._fn(PLUGIN.process), this.ptr, this._proc);
    h._inAudioThread = false;
    cur.length = 0;
    if (status < 0) throw new Error("process() status " + status);

    for (let c = 0; c < this.channels; c++) {
      const out = new Float32Array(h.memory.buffer, this._outCh[c], frames);
      outputChannels[c].set(out.subarray(0, frames));
    }

    // If the plugin called host->request_callback() during process() (e.g. it
    // flagged a latency/tail change), run on_main_thread() now that process()
    // has unwound — outside the plugin's process() call and with thread-check
    // reporting main-thread. The plugin's handler drives host latency/tail
    // .changed(), which the host forwards to the app via the hooks.
    if (h._mainThreadCallbackPending) {
      h._mainThreadCallbackPending = false;
      this.onMainThread();
    }
    // Polled EVERY quantum, not just after a tick. Gating this on the tick looks right —
    // the tick is what REBUILDS the IR — and it silently never fires for the first one:
    // the plugin builds its initial IR while ACTIVATING, so by the time audio is running
    // it has nothing pending, never asks the host for a callback, and a tick-gated poll
    // waits forever for a rebuild that already happened. The GPU worker would then sit
    // with no kernel until the user happened to move a knob.
    //
    // The cost is one wasm call per quantum returning a uint32 (~375/s), which is
    // nothing; the expensive part — copying the IR out — still only happens when the
    // generation actually changes.
    const ir = this.pollIr();
    this.pollDepth();   // apply an adaptive pipeline-depth change, if the page made one
    return { params: this._outParamEvents, midi: this._outMidi, ir };
  }

  // ── clap.state (main-thread op; runs in the worklet message loop, never
  //    concurrent with process()). Produces/consumes the SAME PLST blob the
  //    native VST3/AU/CLAP builds serialize. ────────────────────────────────
  _ensureState() {
    if (this._stateReady) return this._stateExt;
    const h = this.host;
    this._stateExt = this._ext(CLAP_EXT_STATE) || 0;
    if (this._stateExt) {
      // ostream/istream structs + funcrefs, built once and reused.
      this._ostream = h.ex.malloc(OSTREAM.size); h.setU32(this._ostream + OSTREAM.ctx, 0);
      h.setU32(this._ostream + OSTREAM.write, h._addFn("iiI->I", (_s, bufPtr, size) => {
        const n = Number(size);
        this._saveChunks.push(new Uint8Array(h.memory.buffer, bufPtr, n).slice());
        return BigInt(n);
      }));
      this._istream = h.ex.malloc(ISTREAM.size); h.setU32(this._istream + ISTREAM.ctx, 0);
      h.setU32(this._istream + ISTREAM.read, h._addFn("iiI->I", (_s, bufPtr, size) => {
        const remaining = this._loadBuf.length - this._loadPos;
        const n = Math.min(remaining, Number(size));
        if (n > 0) { new Uint8Array(h.memory.buffer, bufPtr, n).set(this._loadBuf.subarray(this._loadPos, this._loadPos + n)); this._loadPos += n; }
        return BigInt(n);
      }));
    }
    this._stateReady = true;
    return this._stateExt;
  }
  getState() {
    const h = this.host;
    if (!this._ensureState()) return null; // extension not exposed by this wasm
    this._saveChunks = [];
    const ok = h.call(h.u32(this._stateExt + STATE_EXT.save), this.ptr, this._ostream);
    if (!ok) return new Uint8Array(0);
    let total = 0; for (const c of this._saveChunks) total += c.length;
    const out = new Uint8Array(total); let o = 0;
    for (const c of this._saveChunks) { out.set(c, o); o += c.length; }
    this._saveChunks = null;
    return out;
  }
  setState(bytes) {
    const h = this.host;
    if (!this._ensureState()) return false;
    this._loadBuf = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this._loadPos = 0;
    return !!h.call(h.u32(this._stateExt + STATE_EXT.load), this.ptr, this._istream);
  }
  hasState() { return !!this._ensureState(); }
}

/* ────────────────────────────── the processor ────────────────────────────── */
const EMPTY = []; // shared, never mutated — handed to processQuantum on an idle quantum
class WclapProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions || {};
    this.useInternalTone = o.useInternalTone ?? false; // shared-player default: process routed input
    this.toneHz = o.toneHz || 220; this.toneAmp = o.toneAmp ?? 0.3; this.phase = 0;
    this.diag = o.diag ?? false;
    // GPU lane (opt-in). The PAGE allocates + primes the SharedArrayBuffer and
    // hands the SAME buffer to the WebGPU DedicatedWorker and to us. Absent it,
    // every path below is inert and this processor behaves byte-for-byte as it
    // did before — every other demo on this player is unaffected.
    this.gpuSab = o.gpuSab || null;
    this.gpuLatencyBlocks = o.gpuLatencyBlocks || 0;
    this.gpuRing = null;
    this.ready = false;
    this.pendingParams = []; this.pendingMidi = []; this.pendingSysex = [];
    this._engineParamId = null;      // set when the params are enumerated, by NAME
    this._engineOn = false;          // the plugin boots on its CPU engine
    this._engineFlagged = false;     // what the ring currently says, so we only write on change
    this.quanta = 0;
    const reportHz = o.reportHz || 20;
    this.reportEvery = Math.max(1, Math.round(sampleRate / 128 / reportHz));
    this._rmsAcc = 0; this._inAcc = 0; this._rmsN = 0;
    this._toneBuf = [new Float32Array(128), new Float32Array(128)];
    this.port.onmessage = (e) => this._onMessage(e.data);
  }

  _onMessage(msg) {
    if (msg.type === "load") {
      try {
        // Attach the GPU ring before the wasm is instantiated: the plugin's
        // env.pulp_gpu_xfer import is bound at instantiation. A malformed or
        // mismatched SAB is NOT fatal — it degrades to no GPU lane (CPU engine).
        if (this.gpuSab && !this.gpuRing) {
          try {
            this.gpuRing = gpuRingAttach(this.gpuSab);
          } catch (e) {
            this.gpuRing = null;
            this.port.postMessage({ type: "log", fd: 2, text: "gpu-ring attach failed: " + String(e && e.message || e) });
          }
        }
        this.host = new WorkletWclapHost(
          (fd, t) => this.port.postMessage({ type: "log", fd, text: t.replace(/\n$/, "") }),
          {
            // clap_host_latency.changed → re-read plugin latency, tell the app so
            // its PDC is correct (mirrors WAM's descriptor.latencySamples).
            onLatencyChanged: (s) => { this._latencySamples = s; this.port.postMessage({ type: "latencyChanged", latencySamples: s }); },
            onTailChanged: (s) => this.port.postMessage({ type: "tailChanged", tailSamples: s }),
            // clap_host_state.mark_dirty → the plugin's persisted state went
            // stale; let the app re-snapshot if it tracks a saved blob.
            onStateDirty: () => this.port.postMessage({ type: "stateDirty" }),
            // clap_host_params.rescan → re-read the plugin's current values and
            // push them so widgets re-sync (the value path, like a state load).
            onParamsRescan: () => {
              const changes = this.plugin ? this.plugin.readParamValues() : [];
              if (changes.length) this.port.postMessage({ type: "paramsChanged", changes });
            },
            onRequestRestart: () => this.port.postMessage({ type: "restartRequested" }),
          },
          this.gpuRing);
        this.host.instantiateSync(msg.bytes || msg.module);
        this.plugin = this.host.createPlugin(msg.pluginIndex || 0);
        this.plugin.init();
        this.plugin.activate(sampleRate, 1, 128);
        const flags = this.plugin.descriptorFlags();
        this.paramList = this.plugin.params();
        this.plugin.prepare(2, 128);
        this._latencySamples = this.plugin.currentLatency();
        this.ready = true;
        this.port.postMessage({ type: "ready",
          descriptor: { ...this.plugin.descriptor, ...flags, hasState: this.plugin.hasState(),
                        latencySamples: this._latencySamples,
                        // Whether the GPU lane is actually wired in THIS instance.
                        // The page must not offer an Engine=GPU toggle otherwise.
                        gpuLane: !!this.gpuRing,
                        gpuLatencyBlocks: this.gpuRing ? this.gpuRing.latencyBlocks : 0 },
          params: this.paramList, sampleRate });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String(err && err.stack || err) });
      }
    } else if (msg.type === "param") {
      const ex = this.pendingParams.find((p) => p.id === msg.id);
      if (ex) ex.value = msg.value; else this.pendingParams.push({ id: msg.id, value: msg.value });
    } else if (msg.type === "midi") {
      this.pendingMidi.push(msg.bytes);
    } else if (msg.type === "sysex") {
      this.pendingSysex.push(msg.bytes instanceof Uint8Array ? msg.bytes : new Uint8Array(msg.bytes));
    } else if (msg.type === "getState") {
      let bytes = null, error = null;
      try { bytes = this.plugin ? this.plugin.getState() : null; } catch (e) { error = String(e && e.stack || e); }
      // Clone (a plain number[] payload), do NOT transfer: transferring an
      // ArrayBuffer OUT of an AudioWorkletGlobalScope is unreliable in Chrome
      // (the receiver gets a detached/empty buffer). State blobs are small.
      this.port.postMessage({ type: "stateResult", token: msg.token, bytes: bytes ? Array.from(bytes) : null, error });
    } else if (msg.type === "setState") {
      let ok = false, error = null;
      try { ok = this.plugin ? this.plugin.setState(msg.bytes) : false; } catch (e) { error = String(e && e.message || e); }
      // Re-sync the host mirror + UI to the loaded values (the plugin does not
      // emit param-out events on load), so widgets reflect the restored state.
      if (ok && this.plugin) {
        const changes = this.plugin.readParamValues();
        if (changes.length) this.port.postMessage({ type: "paramsChanged", changes });
      }
      this.port.postMessage({ type: "setStateResult", token: msg.token, ok, error });
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const frames = out[0] ? out[0].length : 128;
    if (!this.ready) { for (const ch of out) ch.fill(0); return true; }

    let inCh;
    if (this.useInternalTone) {
      const inc = (2 * Math.PI * this.toneHz) / sampleRate;
      const L = this._toneBuf[0], R = this._toneBuf[1];
      for (let i = 0; i < frames; i++) { const s = this.toneAmp * Math.sin(this.phase);
        L[i] = R[i] = s; this.phase += inc; if (this.phase > 2 * Math.PI) this.phase -= 2 * Math.PI; }
      inCh = this._toneBuf;
    } else {
      const inp = inputs[0];
      inCh = (inp && inp.length) ? inp : (this._toneBuf[0].fill(0), this._toneBuf[1].fill(0), this._toneBuf);
    }

    // Swap only when there is something to drain, else hand the plugin a shared
    // empty array — so a steady-state quantum allocates nothing on the audio thread.
    let params = EMPTY, midi = EMPTY, sysex = EMPTY;
    if (this.pendingParams.length) { params = this.pendingParams; this.pendingParams = []; }
    if (this.pendingMidi.length) { midi = this.pendingMidi; this.pendingMidi = []; }
    if (this.pendingSysex.length) { sysex = this.pendingSysex; this.pendingSysex = []; }

    // ── The GPU works ONLY while the plugin is actually on the GPU engine. ──────────
    //
    // Owned HERE, not by the page. The plugin pushes every block across the xfer seam in
    // both engines (that is what keeps the two paths sample-aligned), so a worker that
    // convolves whatever it is handed burns the GPU flat out while the user is on CPU —
    // measured at ~100 dispatches/second producing audio nobody hears. The worker skips all
    // GPU work while this flag is clear.
    //
    // The page must NOT be the one to set it: a page that forgets leaves the worker idle and
    // the GPU engine silent, which is exactly what happened to the engine-proof fixture the
    // first time this was wired page-side. The parameter IS the truth — a <select>, a preset,
    // and a host automation lane are all just ways of moving it — and the audio thread is
    // where the parameter lands. So every consumer of the lane gets this for free.
    const engineId = this.plugin ? this.plugin._engineParamId : null;
    if (this.gpuRing && engineId != null) {
      for (const pv of params) {
        if (pv.id === engineId) this._engineOn = pv.value >= 0.5;
      }
      if (this._engineOn !== this._engineFlagged) {
        if (this._engineOn) this.gpuRing.setFlag(FLAG_ENGINE_GPU);
        else this.gpuRing.clearFlag(FLAG_ENGINE_GPU);
        this._engineFlagged = this._engineOn;
      }
    }

    const captured = this.plugin.processQuantum(inCh, frames, params, midi, sysex, out);
    if (captured.params.length) {
      // The PLUGIN moved a parameter (a preset, a state load, a host automation lane). Engine
      // is one it can move, so the flag has to follow from this direction too — otherwise a
      // loaded preset that selects GPU would leave the worker idle and the plugin silent.
      const engOutId = this.plugin ? this.plugin._engineParamId : null;
      if (this.gpuRing && engOutId != null) {
        for (const c of captured.params) {
          if (c.id !== engOutId) continue;
          this._engineOn = c.value >= 0.5;
          if (this._engineOn !== this._engineFlagged) {
            if (this._engineOn) this.gpuRing.setFlag(FLAG_ENGINE_GPU);
            else this.gpuRing.clearFlag(FLAG_ENGINE_GPU);
            this._engineFlagged = this._engineOn;
          }
        }
      }
      this.port.postMessage({ type: "paramsChanged", changes: captured.params.slice() });
    }
    if (captured.midi.length) this.port.postMessage({ type: "midiOut", events: captured.midi.map((m) => ({ bytes: m.bytes.slice() })) });
    // Rare by construction — only when the plugin actually rebuilt its IR (a Size move,
    // an uploaded impulse), which is a user action, not a per-block event. The array is
    // CLONED, not transferred: transferring out of an AudioWorklet is unreliable (the
    // receiver can get a detached buffer), and it is not ours to detach anyway.
    if (captured.ir) this.port.postMessage({ type: "irChanged", ir: captured.ir });

    if (this.diag) {
      this.quanta++;
      let so = 0, si = 0;
      for (let i = 0; i < frames; i++) { so += out[0][i] * out[0][i]; si += inCh[0][i] * inCh[0][i]; }
      this._rmsAcc += so; this._inAcc += si; this._rmsN += frames;
      if ((this.quanta % this.reportEvery) === 0) {
        // The GPU counters ride the EXISTING throttled meter message (20 Hz) —
        // zero new per-quantum postMessage traffic on the audio thread. The page
        // reads the worker's own stats straight out of the SAB; these three are
        // the worklet-side truth (misses the plugin's CPU path had to cover).
        const g = this.gpuRing ? this.gpuRing.workletCounters() : null;
        this.port.postMessage({ type: "meter", quanta: this.quanta,
          outRms: Math.sqrt(this._rmsAcc / this._rmsN), inRms: Math.sqrt(this._inAcc / this._rmsN),
          currentTime, sampleRate,
          gpuMiss: g ? g.miss : 0, gpuDropped: g ? g.dropped : 0, gpuResynced: g ? g.resynced : 0 });
        this._rmsAcc = 0; this._inAcc = 0; this._rmsN = 0;
      }
    }
    return true;
  }
}

registerProcessor("pulp-wclap", WclapProcessor);
