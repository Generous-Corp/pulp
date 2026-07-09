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
import { makeBridge, parseMidiOutRecords, processorNameForUrl } from "./wam-runtime.mjs";

const MAX_FRAMES = 128;   // Web Audio render quantum is fixed at 128.
const MAX_CHANNELS = 2;   // Stereo lane (see plan: wider bus support is later).
const MIDI_OUT_CAP = 8192; // drain buffer; a full block of MIDI is far smaller.

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
    wam_midi_sysex: M._wam_midi_sysex,
    wam_param_epoch: M._wam_param_epoch,
    wam_read_param_values: M._wam_read_param_values,
    wam_midi_out_drain: M._wam_midi_out_drain,
    wam_reset: M._wam_reset,
    wam_prepare: M._wam_prepare,
    wam_latency_samples: M._wam_latency_samples,
    wam_set_transport: M._wam_set_transport,
    wam_descriptor: M._wam_descriptor,
    wam_parameters: M._wam_parameters,
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
      // MIDI-out drain target, allocated once (never per block).
      this._midiOutPtr = wam.malloc(MIDI_OUT_CAP);
      // Bulk parameter snapshot target + the last epoch we reported. These
      // exports (readParamValues/paramEpoch) postdate the first WAM ABI, so a
      // demo dir carrying a STALE wam-dsp.js won't have them. Calling an absent
      // export unconditionally here would throw in the constructor, before the
      // descriptor is posted — the host would then stall 1.5 s and render an
      // empty GUI over dead audio (a silent failure). Degrade instead: skip the
      // param-echo feature. process() already guards `if (this._wam.paramEpoch)`,
      // so the plugin still runs; it just won't push self-driven param updates
      // (which a DSP old enough to lack these exports never emitted anyway).
      // makeBridge drops these methods when a stale DSP module lacks their
      // backing exports (see wam-runtime.mjs), so feature-detecting the bridge
      // method is sufficient and matches how process() guards drainMidiOut.
      if (wam.readParamValues && wam.paramEpoch) {
        this._paramCount = wam.readParamValues(0, 0);
        this._paramPtr = wam.malloc(Math.max(1, this._paramCount) * 4);
        this._lastParamEpoch = wam.paramEpoch();
      } else {
        this._paramCount = 0;
        this._paramPtr = 0;
        this._lastParamEpoch = 0;
      }
      this._wam = wam;
      for (const m of this._pendingMsgs) this._handle(m);
      this._pendingMsgs.length = 0;
      // Hand the descriptor + parameter metadata to the main thread (it has no
      // DSP Module of its own) so the host can build generated controls.
      this.port.postMessage({ type: "descriptor", json: wam.descriptorJson() });
      this.port.postMessage({ type: "parameters", json: wam.parametersJson() });
    }).catch((e) => {
      this.port.postMessage({ type: "error", error: String((e && e.stack) || e) });
    });
  }

  _handle(msg) {
    if (!this._wam) { this._pendingMsgs.push(msg); return; } // queue until ready
    switch (msg.type) {
      case "param": this._wam.setParam(String(msg.id), msg.value); break;
      case "midi":  this._wam.midi(msg.status, msg.data1, msg.data2, msg.offset | 0); break;
      case "sysex": this._wam.sysex(msg.data, msg.offset | 0); break;
      // One-shot reset — the next process() clears the plugin's DSP state.
      case "reset": this._wam.reset(); break;
      // Sample-rate / block-size change. Serviced here, between render quanta,
      // so it never races an in-flight process() on the audio thread.
      case "prepare": this._wam.prepare(msg.sampleRate, msg.blockSize | 0); break;
      // Host transport snapshot, copied into ProcessContext each block.
      case "transport":
        this._wam.setTransport(msg.isPlaying, msg.bpm, msg.positionBeats,
                               msg.positionSamples, msg.timeSigNumerator,
                               msg.timeSigDenominator);
        break;
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

    // Drain whatever MIDI the plugin emitted this block. Only crosses to the
    // main thread when there is something to report, so a plugin that emits no
    // MIDI (every instrument/effect) costs one wasm call and nothing else.
    if (this._wam.drainMidiOut) {
      const available = this._wam.drainMidiOut(this._midiOutPtr, MIDI_OUT_CAP);
      if (available > 0) {
        const copied = Math.min(available, MIDI_OUT_CAP);
        const bytes = this._wam.u8().subarray(this._midiOutPtr,
                                              this._midiOutPtr + copied);
        const events = parseMidiOutRecords(bytes, copied);
        if (events.length) {
          // parseMidiOutRecords already returns {offset, bytes} records; post
          // them directly rather than re-wrapping (this is the audio thread —
          // an arpeggiator emits MIDI most blocks, so the extra object graph
          // was steady per-block GC pressure for no behavioural gain).
          this.port.postMessage({
            type: "midiOut",
            events,
            truncated: available > MIDI_OUT_CAP,
          });
        }
      }
    }

    // A plugin can rewrite its OWN parameters inside process() — synth-with-presets
    // loads a factory preset into its timbre params when Program changes. The web
    // ABI is pull-only, so without this the host's knobs would silently go stale.
    // One wasm call per block; we only marshal values when the epoch actually moves.
    if (this._wam.paramEpoch) {
      const epoch = this._wam.paramEpoch();
      if (epoch !== this._lastParamEpoch) {
        this._lastParamEpoch = epoch;
        this._wam.readParamValues(this._paramPtr, this._paramCount);
        const values = this._wam.f32().subarray(this._paramPtr >> 2,
                                                (this._paramPtr >> 2) + this._paramCount);
        this.port.postMessage({ type: "paramValues", epoch, values: Array.from(values) });
      }
    }

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

// Name this processor after its own module URL, so two different plugins in one
// AudioContext get distinct names. Must stay a synchronous top-level call: a
// top-level await before registerProcessor() leaves the name unregistered
// because addModule() resolves without waiting for it.
registerProcessor(processorNameForUrl(import.meta.url), PulpWamProcessor);
