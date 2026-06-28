// PulpWamProcessor — the AudioWorkletProcessor half of a Pulp WAMv2 plugin.
//
// Loaded into the AudioWorkletGlobalScope via audioWorklet.addModule() AFTER the
// SINGLE_FILE DSP module (which BASE64-embeds + synchronously compiles the wasm
// and parks the Emscripten Module on globalThis.Module — there is no fetch /
// async compile available in worklet scope, so the module must be pre-embedded).
//
// All JS<->wasm-heap marshalling reuses the shared bridge in wam-runtime.mjs;
// this file only adapts the Emscripten Module surface (Module._wam_*, HEAPF32)
// to the {exports}-shaped object makeBridge expects, and drives the audio
// render quantum.
//
// DSP loading: registerProcessor() MUST run synchronously at module top level —
// AudioWorklet.addModule() resolves WITHOUT waiting for a module top-level
// await, so a TLA before registerProcessor leaves the processor name
// unregistered. Instead the DSP factory is instantiated asynchronously inside
// the constructor; process() outputs silence until it is ready, and the
// descriptor is posted to the main thread on ready (the host awaits it before
// rendering). Packaging convention: each plugin's SINGLE_FILE DSP module is
// served next to this file as ./wam-dsp.js (an ES-module factory).

import createDspModule from "./wam-dsp.js";
import { makeBridge } from "./wam-runtime.mjs";

const MAX_FRAMES = 128;   // Web Audio render quantum is fixed at 128.
const MAX_CHANNELS = 2;   // Stereo lane (see plan: wider bus support is later).

// Adapt the Emscripten Module (Module._name, Module.HEAPF32) to the raw-exports
// shape makeBridge wraps, so the worklet and the Node runner share one bridge.
function moduleExports(M) {
  return {
    // ALLOW_MEMORY_GROWTH can swap the buffer, so report it live via a getter.
    get memory() { return { buffer: M.HEAPF32.buffer }; },
    malloc: (n) => M._malloc(n),
    free: (p) => M._free(p),
    __wasm_call_ctors: M.__wasm_call_ctors,
    wam_init: M._wam_init,
    wam_process: M._wam_process,
    wam_set_param: M._wam_set_param,
    wam_get_param: M._wam_get_param,
    wam_midi: M._wam_midi,
    wam_descriptor: M._wam_descriptor,
    wam_state_size: M._wam_state_size,
    wam_read_state: M._wam_read_state,
    wam_write_state: M._wam_write_state,
  };
}

class PulpWamProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._wam = null;        // set when the DSP module finishes loading
    this._pendingMsgs = [];  // control messages received before ready
    this.port.onmessage = (e) => this._handle(e.data);

    // Instantiate the DSP asynchronously; register stays synchronous (above).
    createDspModule().then((M) => {
      if (!M || typeof M._wam_init !== "function") {
        this.port.postMessage({ type: "error", error: "DSP module missing wam_* exports" });
        return;
      }
      const wam = makeBridge(moduleExports(M));
      wam.callCtors();
      wam.init(sampleRate, MAX_FRAMES); // sampleRate is an AudioWorklet global
      // Lifetime-persistent interleaved scratch buffers (not per-block).
      this._inPtr = wam.malloc(MAX_CHANNELS * MAX_FRAMES * 4);
      this._outPtr = wam.malloc(MAX_CHANNELS * MAX_FRAMES * 4);
      this._wam = wam;
      for (const m of this._pendingMsgs) this._handle(m);
      this._pendingMsgs.length = 0;
      // Hand the descriptor to the main thread (it has no DSP Module of its own).
      this.port.postMessage({ type: "descriptor", json: wam.descriptorJson() });
    }).catch((e) => {
      this.port.postMessage({ type: "error", error: String((e && e.stack) || e) });
    });
  }

  _handle(msg) {
    if (!this._wam) { this._pendingMsgs.push(msg); return; } // queue until ready
    switch (msg.type) {
      case "param": this._wam.setParam(String(msg.id), msg.value); break;
      case "midi":  this._wam.midi(msg.status, msg.data1, msg.data2, msg.offset | 0); break;
      case "getState":
        this.port.postMessage({ type: "state", reqId: msg.reqId, data: this._wam.readState() });
        break;
      case "setState":
        if (msg.data) this._wam.writeState(msg.data instanceof Uint8Array ? msg.data : new Uint8Array(msg.data));
        break;
      case "getParam":
        this.port.postMessage({ type: "paramValue", reqId: msg.reqId, value: this._wam.getParam(String(msg.paramId)) });
        break;
    }
  }

  process(inputs, outputs) {
    const input = inputs[0];
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    if (!this._wam) return true; // DSP not ready yet — output stays silent

    const frames = Math.min(output[0].length, MAX_FRAMES);
    const ch = Math.min(output.length, MAX_CHANNELS);

    // Interleave inputs into the wasm heap (the bridge expects interleaved and
    // de-interleaves internally). ONE process() call per block — not
    // channel-by-channel, which would corrupt channel-coupled DSP.
    const heap = this._wam.f32();
    const ib = this._inPtr >> 2;
    for (let f = 0; f < frames; f++) {
      for (let c = 0; c < ch; c++) {
        const chan = input && input[c];
        heap[ib + f * ch + c] = chan ? chan[f] : 0;
      }
    }

    this._wam.process(this._inPtr, this._outPtr, ch, frames);

    const out = this._wam.f32(); // refetch in case the heap grew
    const ob = this._outPtr >> 2;
    for (let f = 0; f < frames; f++) {
      for (let c = 0; c < ch; c++) {
        output[c][f] = out[ob + f * ch + c];
      }
    }
    return true;
  }

  static get parameterDescriptors() {
    return []; // WAMv2 manages parameters over the port, not via AudioParam.
  }
}

registerProcessor("pulp-wam-processor", PulpWamProcessor);
