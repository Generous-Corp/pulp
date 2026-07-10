// measure-core.mjs — environment-agnostic measurement primitives.
//
// Pure JS, no Node- or browser-specific APIs, so the SAME code runs in Node and
// in a headless-Chrome page (the two execution surfaces the harnesses use). It
// contains the actual math for all four gates in the "own real-time compiler"
// build plan (planning/2026-07-09-own-realtime-compiler-build-plan.md §5):
//
//   - measureCpu()  → per-quantum CPU cost (AOT baseline + interpreter ratio)
//   - nullTest()    → residual (difference-signal) level in dBFS + level delta
//   - renderBuffer()→ deterministic block-by-block render of a DSP adapter
//
// `performance.now()` is a global in modern Node (>=16) and in browsers, so the
// timing loop is identical in both. Nothing here imports fs/http/playwright.

export const FULL_SCALE = 1.0; // dBFS reference (peak of a normalized signal).

export function linToDb(lin) {
  if (!(lin > 0)) return -Infinity;
  return 20 * Math.log10(lin / FULL_SCALE);
}

export function rms(arr) {
  let s = 0;
  for (let i = 0; i < arr.length; i++) s += arr[i] * arr[i];
  return Math.sqrt(s / arr.length);
}

export function peak(arr) {
  let m = 0;
  for (let i = 0; i < arr.length; i++) { const a = Math.abs(arr[i]); if (a > m) m = a; }
  return m;
}

// Interleaved stereo (or mono) sine, length = channels*totalFrames.
export function genSine(freqHz, sampleRate, totalFrames, amp = 0.25, channels = 2) {
  const out = new Float32Array(channels * totalFrames);
  const w = (2 * Math.PI * freqHz) / sampleRate;
  for (let f = 0; f < totalFrames; f++) {
    const v = amp * Math.sin(w * f);
    for (let c = 0; c < channels; c++) out[f * channels + c] = v;
  }
  return out;
}

// Render an interleaved input through a DSP adapter, quantum by quantum, and
// return the interleaved output (same length, truncated to whole quanta). The
// adapter contract is documented in dsp-adapter.mjs. This is deterministic:
// same adapter + same input + same params → bit-identical output every run,
// which is what makes the null test's self-check exact.
export function renderBuffer(adapter, inputInterleaved) {
  const { channels, frames, inBuf, outBuf } = adapter;
  const quantumLen = channels * frames;
  const quanta = Math.floor(inputInterleaved.length / quantumLen);
  const out = new Float32Array(quanta * quantumLen);
  for (let q = 0; q < quanta; q++) {
    const off = q * quantumLen;
    inBuf.set(inputInterleaved.subarray(off, off + quantumLen));
    adapter.processQuantum();
    out.set(outBuf, off);
  }
  return out;
}

// Steady-state per-quantum CPU cost. Warms the JIT, then times `quanta` calls of
// processQuantum() per trial across `trials` trials. Reports the MEDIAN (headline)
// and MIN (least-noisy true cost) microseconds/quantum, plus the % of the
// real-time budget (frames/sampleRate seconds per quantum) each represents.
//
// The input is written into adapter.inBuf ONCE before timing (steady state, not
// per-call marshalling overhead). Optionally re-fills each quantum if
// `refillEachQuantum` is set (kept off by default; measures pure DSP).
export function measureCpu(adapter, opts = {}) {
  const {
    sampleRate = adapter.sampleRate || 48000,
    warmup = 2000,
    quanta = 20000,
    trials = 5,
    input = null,
  } = opts;
  const { channels, frames, inBuf } = adapter;
  const quantumLen = channels * frames;

  if (input) inBuf.set(input.subarray(0, quantumLen));

  for (let i = 0; i < warmup; i++) adapter.processQuantum();

  const perTrialUs = [];
  for (let t = 0; t < trials; t++) {
    const t0 = performance.now();
    for (let i = 0; i < quanta; i++) adapter.processQuantum();
    const dtMs = performance.now() - t0;
    perTrialUs.push((dtMs * 1000) / quanta);
  }
  perTrialUs.sort((a, b) => a - b);
  const median = perTrialUs[Math.floor(perTrialUs.length / 2)];
  const min = perTrialUs[0];

  const quantumBudgetUs = (frames / sampleRate) * 1e6; // e.g. 2666.67 µs @ 48k/128
  return {
    usPerQuantumMedian: median,
    usPerQuantumMin: min,
    quantumBudgetUs,
    budgetPctMedian: (median / quantumBudgetUs) * 100,
    budgetPctMin: (min / quantumBudgetUs) * 100,
    frames, sampleRate, quanta, trials,
    perTrialUs,
  };
}

// Find the integer sample shift (|s| <= maxShift, applied to `b`) that minimizes
// the residual RMS against `a`. Returns { shift, aAligned, bAligned } trimmed to
// the common region. maxShift=0 → exact alignment (the self-check path).
export function bestAlign(a, b, channels, maxShift) {
  if (!maxShift) return { shift: 0, aAligned: a, bAligned: b };
  let best = { shift: 0, err: Infinity };
  for (let s = -maxShift; s <= maxShift; s++) {
    const off = s * channels;
    let sum = 0, n = 0;
    const start = Math.max(0, off), end = Math.min(a.length, b.length + off);
    for (let i = start; i < end; i++) { const d = a[i] - b[i - off]; sum += d * d; n++; }
    if (n > 0) { const err = sum / n; if (err < best.err) best = { shift: s, err }; }
  }
  const off = best.shift * channels;
  const start = Math.max(0, off), end = Math.min(a.length, b.length + off);
  return {
    shift: best.shift,
    aAligned: a.subarray(start, end),
    bAligned: b.subarray(start - off, end - off),
  };
}

// Two measurements on a pair of equal-length interleaved renders A and B:
//
//   residual*  — level of the DIFFERENCE signal (A-B) in dBFS. This is the null
//                test proper: it backs the "interpreter matches AOT to <= -60
//                dBFS" correctness gate. Two identical renders → -Infinity.
//   levelDelta*— level(B) - level(A) in dB. A sanity meter: a +6 dB gain change
//                between A and B reads as +6 dB; identical renders read 0 dB.
//
// Reporting BOTH is what the self-check validates ("identical → -inf; +6 dB → +6
// dB") — they exercise the two independent halves of the meter.
export function nullTest(a, b, opts = {}) {
  const { channels = 2, maxShift = 0 } = opts;
  const { aAligned, bAligned, shift } = bestAlign(a, b, channels, maxShift);
  const n = Math.min(aAligned.length, bAligned.length);
  const diff = new Float32Array(n);
  for (let i = 0; i < n; i++) diff[i] = aAligned[i] - bAligned[i];

  const rmsA = rms(aAligned.subarray(0, n));
  const rmsB = rms(bAligned.subarray(0, n));
  const peakA = peak(aAligned.subarray(0, n));
  const peakB = peak(bAligned.subarray(0, n));

  return {
    alignShift: shift,
    residualPeakDb: linToDb(peak(diff)),
    residualRmsDb: linToDb(rms(diff)),
    levelDeltaRmsDb: linToDb(rmsB) - linToDb(rmsA),
    levelDeltaPeakDb: linToDb(peakB) - linToDb(peakA),
    rmsADb: linToDb(rmsA),
    rmsBDb: linToDb(rmsB),
  };
}

export function fmtDb(x) {
  if (x === -Infinity) return "-inf";
  return x.toFixed(2) + " dB";
}
