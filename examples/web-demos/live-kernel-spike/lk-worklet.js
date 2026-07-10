// Pulp Live Kernel — S0 spike — resident worklet processor (classic script).
//
// A single AudioWorkletProcessor that owns ONE resident wasm kernel instance and
// renders it one 128-frame quantum per process() call. Edits arrive over the
// port: a graph blob (structural edit -> build a new plan + equal-power swap) or
// a param poke (zero-interruption set_param). Nothing here is ever torn down at
// the wasm level — the swap happens INSIDE the instance.
//
// It is a CLASSIC script (audioWorklet.addModule only runs classic scripts): no
// ES imports, and AudioWorkletGlobalScope lacks fetch/TextEncoder/atob — none of
// which we need. The wasm arrives as raw BYTES transferred from the main thread
// and is compiled SYNCHRONOUSLY here with new WebAssembly.Module (posting a
// WebAssembly.Module is silently dropped in Chrome — WS-C2 DECISION.md §3). The
// module is STANDALONE_WASM with internal memory + a 3-call WASI shim.

/* eslint-disable no-undef */

const nowMs = (typeof performance !== "undefined" && performance.now)
  ? () => performance.now() : () => 0;

function makeWasi(getMem) {
  return {
    fd_close: () => 0,
    fd_seek: () => 0,
    fd_write: (fd, iov, cnt, pw) => {
      const dv = new DataView(getMem().buffer);
      let total = 0;
      for (let i = 0; i < cnt; i++) total += dv.getUint32(iov + i * 8 + 4, true);
      dv.setUint32(pw, total, true);
      return 0;
    },
  };
}

class LkKernel {
  constructor(bytes, sampleRate) {
    const module = new WebAssembly.Module(bytes); // synchronous, in-worklet
    let inst;
    inst = new WebAssembly.Instance(module, {
      wasi_snapshot_preview1: makeWasi(() => inst.exports.memory),
    });
    this.ex = inst.exports;
    this.ex._initialize();
    this.ex.lk_init(sampleRate, 128);
    this.outPtr = this.ex.malloc(128 * 4);
    this.blobPtr = this.ex.malloc(4096); // bounded blob scratch (LK_MAX_BYTES)
    // Cache the wasm memory views ONCE (memory growth is disabled, so the buffer
    // never detaches) — zero JS allocation per render quantum, not just zero
    // wasm-heap allocation.
    this.outView = new Float32Array(this.ex.memory.buffer, this.outPtr, 128);
    this.blobView = new Uint8Array(this.ex.memory.buffer, this.blobPtr, 4096);
    // Per-node RMS readout for the live signal-flow graph. Preallocated once
    // (LK_MAX_NODES = 64); lk_node_levels() copies into it in-wasm, so reading
    // levels never allocates on the wasm heap (lk_alloc_count stays flat).
    this.MAX_NODES = 64;
    this.levelsPtr = this.ex.malloc(this.MAX_NODES * 4);
    this.levelsView = new Float32Array(this.ex.memory.buffer, this.levelsPtr, this.MAX_NODES);
  }
  nodeLevels() {
    const n = this.ex.lk_node_levels(this.levelsPtr, this.MAX_NODES);
    return this.levelsView.subarray(0, n);
  }
  // Build a graph blob into the inactive plan; returns { rc, buildMs }.
  // Accepts a Uint8Array or an ArrayBuffer (transferred from the main thread).
  loadPlan(bytes) {
    const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this.blobView.set(u8);
    const t0 = nowMs();
    const rc = this.ex.lk_load_plan(this.blobPtr, u8.length);
    return { rc, buildMs: nowMs() - t0 };
  }
  swap(fadeMs) { this.ex.lk_swap(fadeMs); }
  setParam(node, paramId, value) { this.ex.lk_set_param(node, paramId, value); }
  process(n) {
    this.ex.lk_process(this.outPtr, n);
    return this.outView; // cached view; caller reads the first n frames
  }
  allocCount() { return this.ex.lk_alloc_count(); }
  isFading() { return this.ex.lk_is_fading() === 1; }
}

class LkProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.kernel = null;
    this.ready = false;
    // A QUEUE of pending edits (not a single slot): live playing sends bursts of
    // note pokes interleaved with patch edits, and each must land — dropping one
    // (the old single-slot behaviour) would swallow a note or an edit. Drained in
    // order at the top of the next process() so each is timestamped at the exact
    // quantum it becomes audible (edit->sound latency on the audio clock).
    this.pendingEdits = [];
    this.quanta = 0;
    this.reportEvery = Math.max(1, Math.round(sampleRate / 128 / 20)); // ~20 Hz meter
    this._rms = 0; this._n = 0; this._peak = 0;
    this.port.onmessage = (e) => this._onMsg(e.data);
  }

  _onMsg(m) {
    try {
      if (m.type === "wasm") {
        this.kernel = new LkKernel(m.bytes, sampleRate);
        this.ready = true;
        this.port.postMessage({ type: "ready", sampleRate });
      } else if (m.type === "structuralEdit" || m.type === "param" || m.type === "params") {
        this.pendingEdits.push(m);
      } else if (m.type === "allocProbe") {
        this.port.postMessage({ type: "allocProbe", id: m.id, count: this.kernel.allocCount() });
      }
    } catch (err) {
      this.port.postMessage({ type: "error", message: String(err && err.stack || err) });
    }
  }

  _apply(m) {
    if (m.type === "param") {
      this.kernel.setParam(m.node, m.paramId, m.value);
      if (m.editId != null)
        this.port.postMessage({ type: "applied", kind: "param", editId: m.editId, ctxTime: currentTime, buildMs: 0 });
    } else if (m.type === "params") {
      // Atomic batch (a note event: pitch across N oscillators + gate applied in
      // one quantum, so a legato change never briefly gates the wrong pitch).
      for (let i = 0; i < m.sets.length; i++) this.kernel.setParam(m.sets[i][0], m.sets[i][1], m.sets[i][2]);
    } else { // structuralEdit
      const { rc, buildMs } = this.kernel.loadPlan(m.bytes);
      if (rc === 0) this.kernel.swap(m.fadeMs);
      if (m.editId != null)
        this.port.postMessage({ type: "applied", kind: "structural", editId: m.editId, ctxTime: currentTime, buildMs, rc });
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const frames = out[0] ? out[0].length : 128;
    if (!this.ready) { for (const ch of out) ch.fill(0); return true; }

    // Drain every pending edit at the TOP of the block so the rendered audio
    // already reflects them; currentTime here is the audio-clock time of this
    // block's first sample -> the moment the change is audible.
    while (this.pendingEdits.length) this._apply(this.pendingEdits.shift());

    const mono = this.kernel.process(frames);
    for (let c = 0; c < out.length; c++) out[c].set(mono.subarray(0, frames));

    // meter + real-time proof
    this.quanta++;
    let s = 0, pk = 0;
    for (let i = 0; i < frames; i++) { const v = mono[i]; s += v * v; const a = v < 0 ? -v : v; if (a > pk) pk = a; }
    this._rms += s; this._n += frames; if (pk > this._peak) this._peak = pk;
    if ((this.quanta % this.reportEvery) === 0) {
      // Per-node levels ride the existing meter message (the one deliberate RT
      // telemetry post, ~20 Hz, same as the scope). Array.from copies off the
      // preallocated wasm view — no wasm-heap allocation, so lk_alloc_count is
      // unaffected and the zero-alloc-in-process proof still holds.
      const lv = this.kernel.nodeLevels();
      this.port.postMessage({
        type: "meter", quanta: this.quanta, currentTime, sampleRate,
        outRms: Math.sqrt(this._rms / this._n), peak: this._peak,
        fading: this.kernel.isFading(),
        levels: Array.from(lv),
      });
      this._rms = 0; this._n = 0; this._peak = 0;
    }
    return true;
  }
}

registerProcessor("lk-processor", LkProcessor);
