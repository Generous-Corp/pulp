// adapters/wam.js — the default host adapter for @danielraffel/web-player.
//
// Wraps a Pulp WAMv2 instance (the vendored main-thread host, wam-plugin.js)
// behind the host-adapter interface the shell drives (see adapter.d.ts). This is
// the ONE module in the package that imports a plugin backend; the shell never
// does. A future WebCLAP adapter (adapters/wclap.js) would live beside this and
// expose the same shape over wclap-host.mjs instead.
//
// Factory:  createWamAdapter(ctx, { dsp, processor }) → Promise<HostAdapter>

import PulpWAM from "../vendor/pulp-wasm/wam-plugin.js";

/**
 * @param {BaseAudioContext} ctx
 * @param {{ dsp?: string, processor?: string }} urls  SINGLE_FILE DSP + processor module URLs
 * @returns {Promise<import("./adapter.js").HostAdapter>}
 */
export function createWamAdapter(ctx, urls = {}) {
  return PulpWAM
    .createInstance(ctx, null, { dsp: urls.dsp, processor: urls.processor })
    .then((wam) => wrapWamInstance(ctx, wam));
}

function wrapWamInstance(ctx, wam) {
  return {
    get descriptor() { return wam.descriptor; },
    get audioNode() { return wam.audioNode; },
    getParameterInfo: () => wam.getParameterInfo(),
    setParameterValue: (id, value) => wam.setParameterValue(id, value),
    getParameterValue: (id) => wam.getParameterValue(id),
    scheduleMidi: (status, d1, d2, offset = 0) => wam.scheduleMidi(status, d1, d2, offset),
    sendSysex: (bytes, offset = 0) => wam.sendSysex(bytes, offset),
    getState: () => wam.getState(),
    setState: (bytes) => wam.setState(bytes),
    // onMidiOut / onParamsChanged are handler PROPERTIES the instance reads back
    // (`this.onMidiOut?.(…)`), so forward assignment straight through.
    get onMidiOut() { return wam.onMidiOut; },
    set onMidiOut(fn) { wam.onMidiOut = fn; },
    get onParamsChanged() { return wam.onParamsChanged; },
    set onParamsChanged(fn) { wam.onParamsChanged = fn; },
    // Another instance on the SAME AudioContext (the chained-synth voice pool).
    createSecondary: (secondaryUrls) => createWamAdapter(ctx, secondaryUrls),
    destroy: () => wam.destroy(),
  };
}

export default createWamAdapter;
