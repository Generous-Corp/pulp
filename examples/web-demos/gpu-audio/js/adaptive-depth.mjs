// Adaptive GPU pipeline depth — the pure decision function.
//
// The optimal pipeline depth (latencyBlocks) is DEVICE-DEPENDENT: a fast desktop GPU
// lands each convolution block well inside one block-period and needs a depth of ~2;
// a jittery mobile/Safari GPU whose submit→readback round trip EXCEEDS the real-time
// budget needs a deeper pipeline so a spike does not miss. Measured on real Safari
// (2026-07-16): depth 2 → 37% CPU-covered, depth 6 → 27%; headless Chrome here → 0%
// at depth 2. So no static default is right; pick from the measured round trip.
//
// This is the pure math, split out so it is unit-testable with no browser and no GPU.
// The worker feeds it the live EWMA round trip; the page applies the result.

/**
 * @param {number} avgRoundTripUs   EWMA submit→readback round trip, microseconds.
 * @param {number} blockPeriodUs    Real-time budget for one internal block, µs
 *                                  (blockSize / sampleRate * 1e6; 512/48000 ≈ 10667).
 * @param {{min?:number, max?:number, headroom?:number}} [opts]
 *   min/max clamp the depth; headroom is the multiple of the round trip to cover so a
 *   spike above the average still lands in time (1.5 = cover 150% of the mean).
 * @returns {number} recommended latencyBlocks (integer in [min, max]).
 */
export function recommendLatencyBlocks(avgRoundTripUs, blockPeriodUs, opts = {}) {
  const min = opts.min ?? 2;
  const max = opts.max ?? 8;
  const headroom = opts.headroom ?? 1.5;
  if (!(avgRoundTripUs > 0) || !(blockPeriodUs > 0)) return min;
  // Blocks of latency needed to cover `headroom`× the mean round trip: a block must be
  // ready within depth×blockPeriod of its submit, so depth ≥ headroom·roundTrip/period.
  const needed = Math.ceil((headroom * avgRoundTripUs) / blockPeriodUs);
  return Math.max(min, Math.min(max, needed));
}

/**
 * Debounced target so the applied depth does not chase noise: only report a NEW depth
 * once the raw recommendation has held (>= `hold` consecutive samples) away from the
 * current one. Returns the depth to use now (unchanged until the hold is satisfied).
 */
export class DepthController {
  constructor(current, { hold = 4 } = {}) {
    this.current = current;
    this.hold = hold;
    this._pending = current;
    this._count = 0;
  }
  /** Feed one raw recommendation; returns the debounced depth to use. */
  update(recommended) {
    if (recommended === this.current) { this._count = 0; this._pending = this.current; return this.current; }
    if (recommended === this._pending) { this._count++; }
    else { this._pending = recommended; this._count = 1; }
    if (this._count >= this.hold) { this.current = this._pending; this._count = 0; }
    return this.current;
  }
}
