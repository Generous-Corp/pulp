// Sample-exact capture tap for the browser GPU-audio proof.
//
// Sits AFTER the plugin node and copies every rendered frame of channel 0 into a
// SharedArrayBuffer the page drains. No MediaRecorder, no encoding, no
// resampling: the proof compares the plugin's samples against a float64
// convolution oracle, so anything that touches the samples on the way out would
// be measuring itself.
//
// The write index is an Int32Array[0] published with Atomics.store AFTER the
// frames are written, so the page never reads a partially written quantum.
// Capture stops when the buffer is full — a real-time AudioContext keeps
// calling process() forever, and a wrapping ring would silently overwrite the
// window under test.

class TapProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        const opts = (options && options.processorOptions) || {};
        this.samples = new Float32Array(opts.samples);
        this.index = new Int32Array(opts.index);
        this.capacity = this.samples.length;
    }

    process(inputs) {
        const input = inputs[0];
        if (!input || !input.length) return true;
        const channel = input[0];
        if (!channel) return true;

        const written = Atomics.load(this.index, 0);
        if (written >= this.capacity) return true;   // full: stop, do not wrap

        const n = Math.min(channel.length, this.capacity - written);
        this.samples.set(channel.subarray(0, n), written);
        Atomics.store(this.index, 0, written + n);
        return true;
    }
}

registerProcessor("pulp-tap", TapProcessor);
