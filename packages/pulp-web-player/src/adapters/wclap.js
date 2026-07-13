// adapters/wclap.js — the WebCLAP host adapter for @danielraffel/web-player.
//
// The WebCLAP twin of adapters/wam.js: it exposes the SAME host-adapter contract
// the shell drives (see adapter.d.ts) so the identical shared player can host a
// WebCLAP plugin. Where the WAM adapter wraps a main-thread WAM instance, this
// one drives the worklet-resident CLAP host (vendor/pulp-wasm/wclap-processor.js,
// Architecture A): the whole CLAP host runs INSIDE the AudioWorklet and renders
// in real time, because a CLAP plugin calls the host event vtable synchronously
// during process() and a worklet-side wasm cannot call back to the main thread.
// See examples/web-demos/wclap-build/realtime-spike/DECISION.md.
//
// Factory:  createWclapAdapter(ctx, urls, opts) → Promise<HostAdapter>
//   urls  — the shell's { dsp, processor } seam is honoured: `dsp` is the
//           WebCLAP .wasm URL, `processor` is the wclap-processor.js worklet URL.
//           (Aliases { wasmUrl, workletUrl } are also accepted.)
//   opts  — { pluginIndex, channelCount, diag, onDiag, gpuSab, gpuLatencyBlocks }
//           (all optional; the last two opt this instance into the GPU lane).
//
// This adapter implements the FULL contract on real CLAP machinery:
//   • audioNode / params / set/get              — real (worklet-resident host).
//   • onParamsChanged                           — real (out_events param capture).
//   • scheduleMidi / sendSysex                  — real (CLAP midi / midi_sysex IN).
//   • onMidiOut                                 — real (out_events note/midi capture).
//   • getState / setState                       — real WHEN the WebCLAP wasm
//     exposes the clap.state extension (Pulp's CLAP entry does; it serializes the
//     SAME PLST blob the shell's container speaks). If a wasm lacks clap.state,
//     getState resolves null / setState is a no-op and `descriptor.hasState` is
//     false — flag it as SDK C++ work rather than crash.
//   • createSecondary                           — real (another worklet-resident
//     instance on the same context; the chained-synth voice pool).

import { deriveDisplayUnit } from "../vendor/pulp-wasm/wclap-abi.mjs";

const WORKLET_NAME = "pulp-wclap";
const addedModules = new WeakMap(); // ctx.audioWorklet -> Set<workletUrl> already added

async function ensureModule(ctx, workletUrl) {
  const key = ctx.audioWorklet;
  let set = addedModules.get(key);
  if (!set) addedModules.set(key, (set = new Set()));
  if (set.has(workletUrl)) return;
  try {
    await ctx.audioWorklet.addModule(workletUrl);
  } catch (err) {
    // A concurrent first-time add of the same module registers the processor
    // name twice; that specific race is benign (the module is loaded either way).
    if (!/already .*registered|has already been/i.test(String(err && err.message))) throw err;
  }
  set.add(workletUrl);
}

/**
 * @param {BaseAudioContext} ctx
 * @param {{ dsp?: string, processor?: string, wasmUrl?: string, workletUrl?: string }} urls
 * @param {{ pluginIndex?: number, channelCount?: number, diag?: boolean, onDiag?: Function }} [opts]
 * @returns {Promise<import("./adapter.js").HostAdapter>}
 */
export async function createWclapAdapter(ctx, urls = {}, opts = {}) {
  const wasmUrl = urls.wasmUrl || urls.dsp;
  const workletUrl = urls.workletUrl || urls.processor;
  if (!wasmUrl) throw new Error("createWclapAdapter: a WebCLAP .wasm URL is required (urls.dsp / urls.wasmUrl)");
  if (!workletUrl) throw new Error("createWclapAdapter: the wclap-processor.js worklet URL is required (urls.processor / urls.workletUrl)");
  const pluginIndex = opts.pluginIndex ?? 0;
  const channelCount = opts.channelCount ?? 2;

  // Fetch the module bytes on the main thread; the worklet compiles + instantiates
  // them (posting a WebAssembly.Module INTO an AudioWorklet is silently dropped in
  // Chrome — transfer the raw bytes).
  const [bytes] = await Promise.all([
    fetch(wasmUrl).then((r) => {
      if (!r.ok) throw new Error(`fetch ${wasmUrl} → ${r.status}`);
      return r.arrayBuffer();
    }),
    ensureModule(ctx, workletUrl),
  ]);

  const node = new AudioWorkletNode(ctx, WORKLET_NAME, {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [channelCount],
    processorOptions: {
      useInternalTone: false, diag: !!opts.diag, pluginIndex,
      // Optional GPU lane. The PAGE owns the SharedArrayBuffer (it also hands the
      // same one to the WebGPU DedicatedWorker — see
      // examples/web-demos/gpu-audio/js/gpu-bridge.mjs) because an
      // AudioWorkletProcessor can neither touch navigator.gpu nor spawn a Worker.
      // Omit it and the worklet's GPU path is wholly inert.
      gpuSab: opts.gpuSab || null,
      gpuLatencyBlocks: opts.gpuLatencyBlocks || 0,
    },
  });

  let descriptor = null;
  let paramInfo = [];              // ParameterInfo[] in stable order
  const paramValues = new Map();   // id -> current value (mirror)
  let onParamsChangedCb = null;
  // The plugin's live impulse response, whenever it changes. Only a module that
  // exports one publishes it (see wclap-processor.js pollIr) — for everything else
  // this never fires.
  let onIrChangedCb = null;
  let lastIr = null;
  let onMidiOutCb = null;
  let onLatencyChangedCb = null;   // additive: (samples) => void
  let onStateDirtyCb = null;       // additive: () => void — plugin marked its state dirty
  const stateWaiters = new Map();  // token -> {resolve,reject}
  let stateToken = 0;

  const buildValues = () => paramInfo.map((p) => paramValues.get(p.id));

  const ready = new Promise((resolve, reject) => {
    node.port.onmessage = (e) => {
      const m = e.data;
      switch (m.type) {
        case "ready":
          descriptor = {
            name: m.descriptor.name, vendor: "Pulp",
            isInstrument: !!m.descriptor.isInstrument,
            hasAudioInput: !!m.descriptor.hasAudioInput,
            hasAudioOutput: m.descriptor.hasAudioOutput !== false,
            hasMidiInput: !!m.descriptor.hasMidiInput,
            hasMidiOutput: !!m.descriptor.hasMidiOutput,
            hasState: !!m.descriptor.hasState,
            // Plugin-reported delay-compensation latency, in samples (from the
            // clap.latency plugin extension). Mirrors WAM's descriptor.latencySamples
            // so the shell can compensate PDC identically across both backends.
            latencySamples: m.descriptor.latencySamples || 0,
            // True only when a GPU ring was actually attached in the worklet. The
            // page must gate its Engine=CPU/GPU toggle on this — never offer a
            // toggle that silently does nothing.
            gpuLane: !!m.descriptor.gpuLane,
            gpuLatencyBlocks: m.descriptor.gpuLatencyBlocks || 0,
            id: m.descriptor.id,
          };
          paramInfo = m.params.map((p) => ({
            id: p.id, label: p.name,
            type: p.boolean ? "boolean" : "float",
            // The display unit ("%", "s", "dB") the WAM ABI reports directly in
            // its descriptor JSON. CLAP has no unit field — it exposes display
            // text through clap_plugin_params.value_to_text — so the worklet host
            // probes value_to_text and we recover the suffix here. Without this
            // the identical shared player rendered "1.50" on the WebCLAP page and
            // "1.50 s" on the WAM page for the same plugin.
            unit: deriveDisplayUnit(p.textProbes),
            minValue: p.min, maxValue: p.max, defaultValue: p.default,
            step: p.stepped ? 1 : 0,
          }));
          for (const p of m.params) paramValues.set(p.id, p.default);
          resolve();
          break;
        case "paramsChanged":
          for (const c of m.changes) paramValues.set(c.id, c.value);
          onParamsChangedCb && onParamsChangedCb(buildValues(), paramInfo);
          break;
        case "irChanged":
          // Latched, because the consumer usually arrives AFTER the plugin has already
          // published: the first IR lands during activate, and the page only wires its
          // handler once the adapter resolves. Without the latch that first publish is
          // lost and the GPU worker sits with no kernel until the user happens to move
          // Size — which reads exactly like "the GPU engine is broken".
          lastIr = m.ir;
          onIrChangedCb && onIrChangedCb(lastIr);
          break;
        case "midiOut":
          onMidiOutCb && onMidiOutCb(m.events);
          break;
        case "latencyChanged":
          // Plugin changed its reported latency (clap_host_latency.changed). Keep
          // descriptor.latencySamples live so a shell re-reading it compensates PDC.
          if (descriptor) descriptor.latencySamples = m.latencySamples || 0;
          onLatencyChangedCb && onLatencyChangedCb(m.latencySamples || 0);
          break;
        case "stateDirty":
          // Plugin marked its state dirty (clap_host_state.mark_dirty) — the app
          // may want to re-snapshot getState(). Advisory; ignored if unhandled.
          onStateDirtyCb && onStateDirtyCb();
          break;
        // "tailChanged" / "restartRequested" are advisory host notifications the
        // shared shell does not act on today; ignored (no throw).
        case "stateResult": {
          const w = stateWaiters.get(m.token); if (!w) break; stateWaiters.delete(m.token);
          if (m.error) w.reject(new Error(m.error)); else w.resolve(m.bytes ? new Uint8Array(m.bytes) : null);
          break;
        }
        case "setStateResult": {
          const w = stateWaiters.get(m.token); if (!w) break; stateWaiters.delete(m.token);
          if (m.error) w.reject(new Error(m.error)); else w.resolve(m.ok);
          break;
        }
        case "meter":
          opts.onDiag && opts.onDiag(m);
          break;
        case "error":
          reject(new Error(m.message));
          break;
        // "log" is worklet diagnostics — ignored by the adapter.
      }
    };
  });

  node.port.postMessage({ type: "load", bytes, pluginIndex }, [bytes]);
  await ready;

  const requestState = (message, transfer = []) => new Promise((resolve, reject) => {
    const token = ++stateToken;
    stateWaiters.set(token, { resolve, reject });
    node.port.postMessage({ ...message, token }, transfer);
  });

  return {
    get descriptor() { return descriptor; },
    get audioNode() { return node; },

    getParameterInfo() { return Promise.resolve(paramInfo); },
    getParameterValue(id) { return Promise.resolve(paramValues.get(id)); },
    setParameterValue(id, value) {
      paramValues.set(id, value);
      node.port.postMessage({ type: "param", id, value });
    },

    // Raw channel-voice MIDI → CLAP_EVENT_MIDI (the CLAP adapter's fallback path
    // for note-on/off, CC, bend, PC, aftertouch). `offset` is block-quantized to
    // frame 0 (sub-quantum ~2.7 ms at 48 kHz) — inaudible for a keyboard demo.
    scheduleMidi(status, d1, d2, _offset = 0) {
      node.port.postMessage({ type: "midi", bytes: [status & 0xff, d1 & 0xff, d2 & 0xff] });
    },
    sendSysex(bytes, _offset = 0) {
      // Clone (structured-clone copy), do NOT transfer: transferring would detach
      // the CALLER's buffer — a nasty surprise if it reuses the array. Payloads
      // are small, so the copy is negligible.
      const u8 = bytes instanceof Uint8Array ? bytes.slice() : new Uint8Array(bytes);
      node.port.postMessage({ type: "sysex", bytes: u8 });
    },

    // Real clap.state round-trip when the wasm exposes the extension; otherwise a
    // graceful null / no-op with descriptor.hasState === false (SDK C++ work).
    async getState() {
      if (!descriptor.hasState) return new Uint8Array(0);
      const bytes = await requestState({ type: "getState" });
      return bytes || new Uint8Array(0);
    },
    async setState(bytes) {
      if (!descriptor.hasState) return;
      // Clone, do NOT transfer — see sendSysex: transferring detaches the
      // caller's buffer (the shell reuses its `saved` snapshot across load/save).
      const u8 = bytes instanceof Uint8Array ? bytes.slice() : new Uint8Array(bytes);
      await requestState({ type: "setState", bytes: u8 });
    },

    get onMidiOut() { return onMidiOutCb; },
    set onMidiOut(fn) { onMidiOutCb = fn; },
    get onParamsChanged() { return onParamsChangedCb; },
    set onParamsChanged(fn) { onParamsChangedCb = fn; },
    // The plugin's live IR. Assigning a handler replays the latest one immediately if
    // the plugin already published (see the latch above), so a late subscriber is not
    // punished for being late.
    get onIrChanged() { return onIrChangedCb; },
    set onIrChanged(fn) { onIrChangedCb = fn; if (fn && lastIr) fn(lastIr); },
    /// The last IR the plugin published, or null.
    get impulseResponse() { return lastIr; },
    // Additive (beyond the shared HostAdapter contract): notifications the
    // WebCLAP host forwards from the plugin. Safe no-ops if the shell ignores them.
    get onLatencyChanged() { return onLatencyChangedCb; },
    set onLatencyChanged(fn) { onLatencyChangedCb = fn; },
    get onStateDirty() { return onStateDirtyCb; },
    set onStateDirty(fn) { onStateDirtyCb = fn; },

    // Another worklet-resident instance on the SAME AudioContext (voice pool).
    // Deliberately NOT given the GPU ring: one ring has exactly one producer and
    // one consumer, so a second instance would corrupt the cursors.
    createSecondary(secondaryUrls) {
      return createWclapAdapter(ctx, {
        dsp: (secondaryUrls && (secondaryUrls.wasmUrl || secondaryUrls.dsp)) || wasmUrl,
        processor: (secondaryUrls && (secondaryUrls.workletUrl || secondaryUrls.processor)) || workletUrl,
      }, { pluginIndex, channelCount });
    },

    destroy() {
      try { node.disconnect(); } catch {}
      try { node.port.postMessage({ type: "destroy" }); } catch {}
      node.port.onmessage = null;
    },
  };
}

export default createWclapAdapter;
