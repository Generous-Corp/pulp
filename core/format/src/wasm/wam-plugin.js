// PulpWAM — the main-thread WebAudioModule half of a Pulp WAMv2 plugin.
//
// Conforms to the @webaudiomodules/api WebAudioModule interface. The DSP runs in
// an AudioWorkletProcessor (wam-processor.js) fed by a SINGLE_FILE Emscripten
// module; this class wires the worklet up, relays the descriptor the worklet
// reports, and exposes parameter/state/event control over the worklet port.
//
// Loading model (see wam-processor.js for why): the DSP module BASE64-embeds its
// wasm and is added to the worklet BEFORE the processor module, so the processor
// can read globalThis.Module synchronously. No fetch happens in worklet scope.

let instanceCounter = 0;

export default class PulpWAM {
  static get isWebAudioModuleConstructor() { return true; }

  // urls: { dsp, processor } — URLs of the SINGLE_FILE DSP module and the
  // processor module. Defaults assume both sit next to this file.
  static async createInstance(audioContext, initialState, urls) {
    const wam = new PulpWAM(audioContext, urls);
    await wam.initialize(initialState);
    return wam;
  }

  constructor(audioContext, urls = {}) {
    this._audioContext = audioContext;
    this._audioNode = null;
    this._initialized = false;
    this._instanceId = `pulp-wam-${++instanceCounter}`;
    this._descriptor = null;
    this._dspUrl = urls.dsp || new URL("./PulpGainWorklet.js", import.meta.url).href;
    this._processorUrl = urls.processor || new URL("./wam-processor.js", import.meta.url).href;
    this._pending = new Map(); // id -> resolver, for request/response over the port
    this._reqId = 0;
  }

  get isWebAudioModule() { return true; }
  get audioContext() { return this._audioContext; }
  get audioNode() { return this._audioNode; }
  get initialized() { return this._initialized; }
  get moduleId() { return "com.pulp.wam"; }
  get instanceId() { return this._instanceId; }
  get descriptor() { return this._descriptor; }
  get name() { return this._descriptor?.name || "Pulp Plugin"; }
  get vendor() { return this._descriptor?.vendor || "Pulp"; }

  async initialize(state) {
    // One addModule: the processor module imports the SINGLE_FILE DSP factory
    // (served alongside as ./wam-dsp.js) and awaits it at top level, so the
    // Module is ready before the AudioWorkletProcessor constructs.
    await this._audioContext.audioWorklet.addModule(this._processorUrl);

    const node = new AudioWorkletNode(this._audioContext, "pulp-wam-processor", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [2],
    });

    // The worklet reports its descriptor (and answers state/param requests).
    // Race it against a short timeout: in an OfflineAudioContext the worklet may
    // not run its async DSP init until rendering starts, so we must not block
    // initialize() on the descriptor — it fills in via _onMessage when it
    // arrives (during render).
    const descriptorReady = new Promise((resolve) => { this._resolveDescriptor = resolve; });
    node.port.onmessage = (e) => this._onMessage(e.data);
    this._audioNode = node;

    await Promise.race([descriptorReady, new Promise((r) => setTimeout(r, 1500))]);
    this._initialized = true;
    if (state) await this.setState(state);
    return this;
  }

  _onMessage(msg) {
    switch (msg?.type) {
      case "descriptor":
        try { this._descriptor = JSON.parse(msg.json); } catch { this._descriptor = {}; }
        this._resolveDescriptor?.(this._descriptor);
        break;
      case "error":
        // Surface a worklet-side failure rather than silently timing out.
        console.error("PulpWAM worklet error:", msg.error);
        this._lastError = msg.error;
        break;
      case "state":
      case "paramValue": {
        const r = this._pending.get(msg.reqId);
        if (r) { this._pending.delete(msg.reqId); r(msg.type === "state" ? msg.data : msg.value); }
        break;
      }
    }
  }

  // Request/response over the port. reqId correlates the reply; any other fields
  // (e.g. paramId) are payload and must not collide with reqId.
  _request(type, extra) {
    const reqId = ++this._reqId;
    return new Promise((resolve) => {
      this._pending.set(reqId, resolve);
      this._audioNode.port.postMessage({ type, reqId, ...extra });
    });
  }

  setParameterValue(id, value) {
    this._audioNode?.port.postMessage({ type: "param", id, value });
  }
  async getParameterValue(id) { return this._request("getParam", { paramId: id }); }

  scheduleMidi(status, data1, data2, offset = 0) {
    this._audioNode?.port.postMessage({ type: "midi", status, data1, data2, offset });
  }

  async getState() { return this._request("getState", {}); }
  async setState(data) { this._audioNode?.port.postMessage({ type: "setState", data }); }

  async createGui() {
    // Minimal generated GUI from the descriptor. The full generated-controls
    // component (bound to the param bridge) is the Phase 1B deliverable.
    const el = document.createElement("div");
    el.style.cssText = "padding:16px;background:#1e1e2e;color:#cdd6f4;font-family:system-ui;border-radius:8px";
    el.innerHTML = `<h3 style="margin:0 0 4px">${this.name}</h3><p style="margin:0;opacity:.7">WAMv2 · ${this.vendor}</p>`;
    return el;
  }
  destroyGui(el) { el?.parentNode?.removeChild(el); }

  destroy() { this._audioNode?.disconnect(); }
}
