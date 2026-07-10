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
    this.pending = null;   // one pending edit {kind, ...}
    this.quanta = 0;
    this.reportEvery = Math.max(1, Math.round(sampleRate / 128 / 20)); // ~20 Hz meter
    this._rms = 0; this._n = 0;
    this.port.onmessage = (e) => this._onMsg(e.data);
  }

  _onMsg(m) {
    try {
      if (m.type === "wasm") {
        this.kernel = new LkKernel(m.bytes, sampleRate);
        this.ready = true;
        this.port.postMessage({ type: "ready", sampleRate });
      } else if (m.type === "structuralEdit" || m.type === "param") {
        // Defer to the next process() so we can timestamp the exact quantum the
        // edit becomes audible (edit->sound latency in the audio clock).
        this.pending = m;
      } else if (m.type === "allocProbe") {
        this.port.postMessage({ type: "allocProbe", id: m.id, count: this.kernel.allocCount() });
      }
    } catch (err) {
      this.port.postMessage({ type: "error", message: String(err && err.stack || err) });
    }
  }

  _applyPending() {
    const m = this.pending; this.pending = null;
    if (m.type === "param") {
      this.kernel.setParam(m.node, m.paramId, m.value);
      this.port.postMessage({ type: "applied", kind: "param", editId: m.editId, ctxTime: currentTime, buildMs: 0 });
    } else { // structuralEdit
      const { rc, buildMs } = this.kernel.loadPlan(m.bytes);
      if (rc === 0) this.kernel.swap(m.fadeMs);
      this.port.postMessage({ type: "applied", kind: "structural", editId: m.editId, ctxTime: currentTime, buildMs, rc });
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const frames = out[0] ? out[0].length : 128;
    if (!this.ready) { for (const ch of out) ch.fill(0); return true; }

    // Apply a pending edit at the TOP of the block so the rendered audio already
    // reflects it; currentTime here is the audio-clock time of this block's first
    // sample -> the moment the change is audible.
    if (this.pending) this._applyPending();

    const mono = this.kernel.process(frames);
    for (let c = 0; c < out.length; c++) out[c].set(mono.subarray(0, frames));

    // meter + real-time proof
    this.quanta++;
    let s = 0;
    for (let i = 0; i < frames; i++) s += mono[i] * mono[i];
    this._rms += s; this._n += frames;
    if ((this.quanta % this.reportEvery) === 0) {
      this.port.postMessage({
        type: "meter", quanta: this.quanta, currentTime, sampleRate,
        outRms: Math.sqrt(this._rms / this._n), fading: this.kernel.isFading(),
      });
      this._rms = 0; this._n = 0;
    }
    return true;
  }
}

registerProcessor("lk-processor", LkProcessor);
