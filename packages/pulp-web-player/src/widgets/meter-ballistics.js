// widgets/meter-ballistics.js — pure, DOM-free meter ballistics.
//
// A faithful port of the native MeterBallistics::update
// (core/view/include/pulp/view/audio_bridge.hpp:22-63). No `document`, no
// canvas — just the envelope math — so it is unit-testable in plain Node and
// shared by meter.js. Time is in SECONDS; feed it real `dt` deltas from a
// clock (performance.now), never a fixed per-frame decrement, so the decay is
// frame-rate independent.
//
// Durations are design tokens, NOT approximations:
//   motion.duration.meter_decay = 0.30 s  (release time constant τ)
//   motion.duration.peak_hold   = 1.50 s
// NOTE: tokens.css emits these as "0.3px" / "1.5px" because the exporter
// appends `px` to every dimension. They are SECONDS. Parse the number; never
// feed it to CSS as a length. We hardcode them here from theme_presets.cpp
// (lines 118-119) to keep this module free of any CSS/DOM dependency.
export const ATTACK = 0.001;   // near-instant attack (s) — native default
export const DECAY = 0.30;     // motion.duration.meter_decay — release τ (s)
export const PEAK_HOLD = 1.50; // motion.duration.peak_hold (s)

// Stepped fill-colour selector (NOT a gradient). Matches visualizers.cpp:533-536:
//   level > 0.9  → red;  level > 0.7 → yellow;  else green.
// The comparisons are strict, exactly as native — so 0.7 is still green and
// 0.9 is still yellow; the switch happens the instant the level exceeds each.
export function meterColorStop(level) {
  if (level > 0.9) return "red";
  if (level > 0.7) return "yellow";
  return "green";
}

export function createBallistics() {
  return { displayRms: 0, displayPeak: 0, heldPeak: 0, holdCounter: 0 };
}

// Advance the envelope by `dt` seconds given instantaneous inputs rms/peak
// (0..1). Instantaneous attack, exponential release with the DECAY time
// constant, and a peak that holds for PEAK_HOLD before it starts to fall.
// Mutates and returns `b`.
export function updateBallistics(b, rms, peak, dt,
                                 attack = ATTACK, release = DECAY, holdTime = PEAK_HOLD) {
  if (dt < 0) dt = 0;
  const attackK = 1 - Math.exp(-dt / attack);
  const releaseK = 1 - Math.exp(-dt / release);

  // Attack/release envelope on both the RMS body and the peak line.
  b.displayPeak += (peak - b.displayPeak) * (peak > b.displayPeak ? attackK : releaseK);
  b.displayRms  += (rms  - b.displayRms)  * (rms  > b.displayRms  ? attackK : releaseK);

  // Peak hold: latch the max, hold it for holdTime, then decay toward 0 at the
  // release rate (visualizers.cpp / audio_bridge.hpp:48-57).
  if (peak >= b.heldPeak) {
    b.heldPeak = peak;
    b.holdCounter = holdTime;
  } else {
    b.holdCounter -= dt;
    if (b.holdCounter <= 0) b.heldPeak += (0 - b.heldPeak) * releaseK;
  }

  // Floor tiny residuals so a decayed meter reads as truly empty.
  if (b.displayPeak < 1e-6) b.displayPeak = 0;
  if (b.displayRms < 1e-6) b.displayRms = 0;
  if (b.heldPeak < 1e-6) b.heldPeak = 0;
  return b;
}
