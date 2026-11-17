// Runs the sample-region DSP module's authored-ID probe inside an
// AudioWorkletGlobalScope, where crypto and performance are absent, and posts
// the result (or the thrown error) back to the page.
import createDspModule from './wam-dsp.js';

class SampleRegionMintProbe extends AudioWorkletProcessor {
  constructor() {
    super();
    createDspModule().then(module => {
      const probe = module._sample_region_wam_authored_id_probe;
      if (typeof probe !== 'function') throw new Error('authored-ID probe export missing');
      this.port.postMessage({ result: probe() });
    }).catch(error => this.port.postMessage({ error: String(error && (error.stack || error)) }));
  }
  process() { return false; }
}
registerProcessor('sample-region-mint-probe', SampleRegionMintProbe);
