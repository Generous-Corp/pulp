// Oscilloscope triggering for Pulp web hosts.
//
// An AnalyserNode's getFloatTimeDomainData() snapshot begins at an arbitrary
// point in the signal's cycle, so a scope that plots from index 0 shows a
// waveform sliding left and right every animation frame. That is not noise, and
// smoothing or averaging frames does not fix it — it just blurs a sawtooth into
// mush while it keeps moving.
//
// A hardware scope solves this with a trigger: start drawing at the first rising
// zero-crossing. The trace then stands still for any periodic signal. This
// module is that trigger, kept out of the drawing code so it can be unit-tested
// without a canvas (see wam-scope.test.mjs).
//
// Pure JS, no browser APIs — it loads in Node, a worklet, and the main thread.

/**
 * Index of the first rising zero-crossing in `samples`, suitable as the start of
 * a `drawLength`-sample plotting window.
 *
 * Hysteresis: the signal must first dip below `-threshold` before a crossing
 * arms, so a noisy waveform riding on zero cannot retrigger on every sample.
 *
 * Returns 0 when no crossing is found — silence, DC, and sub-threshold noise
 * must leave the scope drawing (from the buffer start) rather than freezing or
 * blanking it.
 *
 * @param {Float32Array|number[]} samples  time-domain samples, nominally -1..1
 * @param {number} drawLength              samples the caller intends to plot
 * @param {{threshold?: number}} [options] hysteresis arming level (default 0.01)
 * @returns {number} start index, always within [0, max(0, samples.length - drawLength)]
 */
export function findTriggerIndex(samples, drawLength, options = {}) {
  const { threshold = 0.01 } = options;
  const length = samples?.length ?? 0;
  if (length === 0) return 0;

  // Never return a start that would run the plot off the end of the buffer.
  // A low-frequency waveform's first crossing can legitimately fall late.
  const limit = Math.max(0, length - Math.max(0, drawLength | 0));
  if (limit === 0) return 0;

  let armed = false;
  for (let i = 1; i < limit; i++) {
    const previous = samples[i - 1];
    const current = samples[i];
    if (current < -threshold) armed = true;
    else if (armed && previous <= 0 && current > 0) return i;
  }
  return 0;   // no crossing: silence, DC, or amplitude below the threshold
}

/**
 * Convenience: a `drawLength`-sample view starting at the trigger.
 * Returns a subarray for Float32Array input (no copy), else a sliced array.
 */
export function triggeredView(samples, drawLength, options = {}) {
  const start = findTriggerIndex(samples, drawLength, options);
  const end = start + drawLength;
  return typeof samples.subarray === "function"
    ? samples.subarray(start, end)
    : samples.slice(start, end);
}
