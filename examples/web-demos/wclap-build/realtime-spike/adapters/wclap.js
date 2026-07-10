// WS-C2 SPIKE — WebCLAP adapter sketch for the shared player (WS-B contract).
//
// This sketches `adapters/wclap.js` on top of the real-time worklet-resident
// host proven by the PoC (../wclap-worklet.js). It is the WebCLAP twin of the
// shipped WAM adapter and implements the SAME contract the shared player drives:
//
//   descriptor, audioNode, getParameterInfo, setParameterValue,
//   getParameterValue, scheduleMidi, sendSysex, getState, setState,
//   onMidiOut, onParamsChanged, createSecondary, destroy
//
// WHAT THE POC PROVES (implemented + measured, real-time in the worklet):
//   • audioNode            — the AudioWorkletNode IS the adapter's node.
//   • getParameterInfo     — from clap.params (id/name/min/max/default).
//   • setParameterValue    — posts a param event, latched at the next quantum.
//   • getParameterValue    — last value the adapter set (mirror).
//   • onParamsChanged      — the worklet's out_events try_push now CAPTURES
//                            CLAP_EVENT_PARAM_VALUE output events (the current
//                            wclap-host.mjs DROPS them — fixed worklet-side here).
//   • destroy              — tears down the node + context wiring.
//
// WHAT STILL NEEDS SDK WORK (flagged, NOT adapter glue — see DECISION.md §Scope):
//   • scheduleMidi/sendSysex — the worklet host builds only clap_event_param_value
//     today; it needs clap_event_note / clap_event_midi / clap_event_midi_sysex
//     IN events (struct marshalling in the same synchronous process() path).
//   • getState/setState      — requires the CLAP `clap.state` extension (stream
//     read/write vtable). wclap-host.mjs has NO state ext today. New work.
//   • createSecondary        — the chained-synth voice pool: N plugin instances
//     in one worklet sharing the memory/host, or one processor per voice. The
//     worklet-resident model supports either; unbuilt in the PoC.
//   • onMidiOut              — same out_events path as onParamsChanged, filtered
//     for note/midi output events instead of param.
//
// The point of the spike: prove the ARCHITECTURE (real-time audio + params
// through the worklet) so this adapter can be finished with confidence.

const WORKLET_NAME = "wclap-rt";

export async function createWclapAdapter(audioContext, {
  wasmUrl,                         // URL of the WebCLAP .wasm
  workletUrl,                      // URL of wclap-worklet.js
  pluginIndex = 0,
  channelCount = 2,
} = {}) {
  // Bytes fetched on the main thread; the worklet compiles + instantiates them
  // (Chrome silently drops a posted WebAssembly.Module — transfer bytes).
  const bytes = await (await fetch(wasmUrl)).arrayBuffer();
  await audioContext.audioWorklet.addModule(workletUrl);

  const node = new AudioWorkletNode(audioContext, WORKLET_NAME, {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [channelCount],
    processorOptions: { useInternalTone: false }, // effect mode: process routed input
  });

  let descriptor = null;
  let paramInfo = [];                    // [{id,label,min,max,default,...}]
  const paramValues = new Map();         // id -> current value (mirror)
  let onParamsChangedCb = null;
  let onMidiOutCb = null;

  const ready = new Promise((resolve, reject) => {
    node.port.onmessage = (e) => {
      const m = e.data;
      switch (m.type) {
        case "ready":
          descriptor = {
            id: m.descriptor.id, name: m.descriptor.name,
            // WS-B descriptor flags — from the CLAP plugin descriptor/features.
            // PulpGain is an audio effect (has audio in+out, no MIDI).
            isInstrument: false, hasAudioInput: true,
            hasMidiInput: false, hasMidiOutput: false,
          };
          paramInfo = m.params.map((p) => ({
            id: p.id, label: p.name, type: "float",
            min: p.min, max: p.max, default: p.default, unit: "", step: 0,
          }));
          for (const p of m.params) paramValues.set(p.id, p.default);
          resolve();
          break;
        case "paramsChanged":            // host-visible output param changes
          for (const c of m.changes) paramValues.set(c.id, c.value);
          onParamsChangedCb && onParamsChangedCb(m.changes);
          break;
        case "midiOut":                  // (future) note/midi output events
          onMidiOutCb && onMidiOutCb(m.bytes);
          break;
        case "error":
          reject(new Error(m.message));
          break;
        // "log"/"meter" ignored by the adapter (PoC-only diagnostics).
      }
    };
  });

  node.port.postMessage({ type: "load", bytes, pluginIndex }, [bytes]);
  await ready;

  return {
    descriptor,
    audioNode: node,

    getParameterInfo() { return paramInfo; },
    getParameterValue(id) { return paramValues.get(id); },
    setParameterValue(id, value) {
      paramValues.set(id, value);
      node.port.postMessage({ type: "param", id, value });
    },

    // ── flagged as SDK work (see header) — signatures wired so the shell can
    //    bind them; the worklet host must grow the matching CLAP event/ext code.
    scheduleMidi(_bytes, _offsetFrames) {
      // TODO(sdk): build clap_event_note / clap_event_midi in the worklet's
      // in_events list (currently param-value only). Post {type:"midi",...}.
      throw new Error("wclap adapter: scheduleMidi needs CLAP note/midi IN events (SDK work)");
    },
    sendSysex(_bytes) {
      // TODO(sdk): clap_event_midi_sysex IN event.
      throw new Error("wclap adapter: sendSysex needs CLAP midi_sysex IN events (SDK work)");
    },
    async getState() {
      // TODO(sdk): CLAP clap.state extension (stream save). wclap-host.mjs has none.
      throw new Error("wclap adapter: getState needs the CLAP clap.state extension (SDK work)");
    },
    async setState(_bytes) {
      throw new Error("wclap adapter: setState needs the CLAP clap.state extension (SDK work)");
    },

    onParamsChanged(cb) { onParamsChangedCb = cb; },
    onMidiOut(cb) { onMidiOutCb = cb; },

    createSecondary() {
      // TODO(sdk): chained-synth voice pool — another plugin instance in the
      // worklet-resident host (share memory + host vtable), or a second
      // processor. Unbuilt in the PoC.
      throw new Error("wclap adapter: createSecondary (voice pool) is unbuilt (SDK work)");
    },

    destroy() {
      try { node.disconnect(); } catch {}
      node.port.postMessage({ type: "destroy" });
      node.port.onmessage = null;
    },
  };
}
