// An INDEPENDENT convolution oracle for the browser GPU-audio proof.
//
// Deliberately NOT Pulp's convolver, and deliberately not the wasm: this is a
// direct time-domain convolution in float64, written here, so that "the GPU
// output matches the oracle" is a claim about the audio, not a claim that two
// copies of the same code agree. Every helper is pure — no DOM, no audio, no
// wasm — so it also runs in plain node.
//
// The proof's alignment scheme (see validate-gpu.mjs) makes the truncated
// kernel EXACT rather than approximate: the response to a lone unit impulse is
// the plugin's impulse response h, and by causality the first N samples of
// (x * h) depend only on h[0..N), so comparing the first N samples of the
// captured audio against a convolution with the first N taps of h is exact —
// no windowing, no tail error.

/// Deterministic PRNG (xorshift32) — the test signal must be byte-identical
/// across runs and across machines, or Run B's sample-wise comparison is noise.
export function makeNoise(length, seed = 0x9e3779b9) {
    let s = seed >>> 0;
    const out = new Float32Array(length);
    for (let i = 0; i < length; i++) {
        s ^= s << 13; s >>>= 0;
        s ^= s >>> 17;
        s ^= s << 5;  s >>>= 0;
        // [-0.5, 0.5): loud enough to be well above float32 noise, quiet enough
        // that the convolution's gain cannot clip the output.
        out[i] = (s / 4294967296) - 0.5;
    }
    return out;
}

/// y[n] = sum_k x[n-k] * h[k], accumulated in float64, for n in [0, count).
export function convolve(x, h, count) {
    const y = new Float64Array(count);
    const taps = h.length;
    for (let n = 0; n < count; n++) {
        let acc = 0;
        const kMax = Math.min(taps - 1, n);
        for (let k = 0; k <= kMax; k++) acc += x[n - k] * h[k];
        y[n] = acc;
    }
    return y;
}

export function rms(v, count = v.length) {
    let acc = 0;
    for (let i = 0; i < count; i++) acc += v[i] * v[i];
    return Math.sqrt(acc / Math.max(1, count));
}

/// ||a - b|| / ||b||. The gate for "the GPU reproduced the convolution": an
/// FP32 FFT convolution against a float64 direct convolution differs by
/// rounding, not by structure, so this stays small and does not drift with
/// signal level.
export function relativeRmsError(a, b, count) {
    let num = 0, den = 0;
    for (let i = 0; i < count; i++) {
        const d = a[i] - b[i];
        num += d * d;
        den += b[i] * b[i];
    }
    if (den === 0) return num === 0 ? 0 : Infinity;
    return Math.sqrt(num / den);
}

/// Largest |a[i] - scale * b[i]| over the first `count` samples. Run B asserts
/// the tampered kernel scaled the audio SAMPLE-WISE, which no statistical
/// summary would prove.
export function maxScaledDeviation(a, b, scale, count) {
    let worst = 0;
    for (let i = 0; i < count; i++) {
        const d = Math.abs(a[i] - scale * b[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

/// Index of the first sample whose magnitude clears `threshold`, or -1.
export function firstOnset(v, threshold) {
    for (let i = 0; i < v.length; i++) if (Math.abs(v[i]) > threshold) return i;
    return -1;
}
