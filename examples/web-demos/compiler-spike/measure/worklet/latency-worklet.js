// latency-worklet.js — classic-script AudioWorkletProcessor that hosts a Pulp
// WAM DSP module, used only by harness4 to time edit→sound latency.
//
// Classic script (no ES imports in AudioWorkletGlobalScope — DECISION.md §2), so
// the wasm import stubs + minimal wam_* bridge are inlined rather than imported
// from wam-runtime.mjs. The wasm is delivered the proven way: raw BYTES are
// transferred in and compiled SYNCHRONOUSLY inside the worklet
// (new WebAssembly.Module) — never a WebAssembly.Module postMessage'd across the
// boundary (Chrome drops that silently, DECISION.md §3). The same byte-transfer
// + in-worklet sync-compile path is exactly how the S0 kernel VM will hot-swap.
//
// Messages in:  {type:'load', bytes, sampleRate}     — compile+init slot
//               {type:'setParam', id, value}         — param edit
//               {type:'swap', bytes, sampleRate, params} — structural-swap stub
// The processor multiplies its input by the DSP (PulpGain: params "1"=input gain
// dB, "2"=output gain dB), so raising a gain param turns near-silence audible.

function makeImports(getMem) {
  const dv = () => new DataView(getMem().buffer);
  return {
    env: {
      _abort_js: () => { throw new Error("wasm abort"); },
      _tzset_js: () => {},
      emscripten_resize_heap: () => false,
    },
    wasi_snapshot_preview1: {
      environ_get: () => 0,
      environ_sizes_get: (c, s) => { dv().setUint32(c, 0, true); dv().setUint32(s, 0, true); return 0; },
      fd_close: () => 0, fd_seek: () => 0,
      fd_write: (fd, iov, cnt, nw) => { const d = dv(); let t = 0; for (let i = 0; i < cnt; i++) t += d.getUint32(iov + i * 8 + 4, true); d.setUint32(nw, t, true); return 0; },
    },
  };
}

const FR = 128, CH = 2, N = FR * CH;

class LatencyProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.dsp = null;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  loadSlot(bytes, sampleRate, params) {
    // Sync compile INSIDE the worklet (bytes transferred, not a Module).
    const mod = new WebAssembly.Module(bytes);
    let inst;
    inst = new WebAssembly.Instance(mod, makeImports(() => inst.exports.memory));
    const ex = inst.exports;
    if (ex.__wasm_call_ctors) ex.__wasm_call_ctors();
    ex.wam_init(sampleRate, FR);
    const inPtr = ex.malloc(N * 4), outPtr = ex.malloc(N * 4);
    const dsp = { ex, inPtr, outPtr };
    if (params) for (const [id, v] of Object.entries(params)) this.setParam(dsp, id, v);
    return dsp;
  }

  setParam(dsp, id, value) {
    const ex = dsp.ex;
    const s = String(id);
    const p = ex.malloc(s.length + 1);
    const u8 = new Uint8Array(ex.memory.buffer);
    for (let i = 0; i < s.length; i++) u8[p + i] = s.charCodeAt(i);
    u8[p + s.length] = 0;
    ex.wam_set_param(p, value);
    ex.free(p);
  }

  onMessage(m) {
    if (m.type === "load") {
      this.dsp = this.loadSlot(m.bytes, m.sampleRate, m.params);
      this.port.postMessage({ type: "loaded" });
    } else if (m.type === "setParam" && this.dsp) {
      // Applied on the audio thread the moment onmessage runs (between quanta).
      this.setParam(this.dsp, m.id, m.value);
      this.port.postMessage({ type: "applied", id: m.id, at: currentTime });
    } else if (m.type === "swap") {
      // Teardown-free structural-swap stub: compile a fresh slot from transferred
      // bytes and replace the resident one (the old Instance GCs). Mirrors how the
      // kernel VM swaps a plan under an equal-power fade.
      this.dsp = this.loadSlot(m.bytes, m.sampleRate, m.params);
      this.port.postMessage({ type: "swapped", at: currentTime });
    }
  }

  process(inputs, outputs) {
    const inp = inputs[0], out = outputs[0];
    if (!this.dsp || !inp || inp.length === 0) {
      if (out) for (const ch of out) ch.fill(0);
      return true;
    }
    const ex = this.dsp.ex;
    const heap = new Float32Array(ex.memory.buffer);
    const inBase = this.dsp.inPtr >> 2, outBase = this.dsp.outPtr >> 2;
    const l = inp[0] || new Float32Array(FR);
    const r = inp[1] || l;
    for (let f = 0; f < FR; f++) { heap[inBase + f * CH] = l[f]; heap[inBase + f * CH + 1] = r[f]; }
    ex.wam_process(this.dsp.inPtr, this.dsp.outPtr, CH, FR);
    const oL = out[0], oR = out[1] || out[0];
    for (let f = 0; f < FR; f++) { oL[f] = heap[outBase + f * CH]; if (out[1]) oR[f] = heap[outBase + f * CH + 1]; }
    return true;
  }
}

registerProcessor("pulp-latency-processor", LatencyProcessor);
