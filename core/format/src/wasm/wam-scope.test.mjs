// Tests for wam-scope.mjs (oscilloscope triggering).
//
// Usage: node wam-scope.test.mjs      (exit 0 = PASS)
//
// No browser, no wasm: the trigger is pure arithmetic over a sample buffer, so
// the cases that actually break a scope in the wild — silence, DC, a crossing
// that falls past the plot window, noise on zero — are all testable here.

import { findTriggerIndex, triggeredView } from "./wam-scope.mjs";

let failures = 0;
const check = (name, condition, detail = "") => {
  if (!condition) failures++;
  console.log(`${condition ? "ok  " : "FAIL"}  ${name}${detail ? " — " + detail : ""}`);
};

const sine = (length, periodSamples, phase = 0, amplitude = 1) =>
  Float32Array.from({ length }, (_, i) =>
    amplitude * Math.sin(2 * Math.PI * ((i / periodSamples) + phase)));

const DRAW = 1024;

// ── the point of the whole exercise ──────────────────────────────────────
// The same waveform captured at different phases must trigger such that the
// plotted window starts at the same point in the cycle every time.
{
  const period = 128;
  const starts = [];
  for (let p = 0; p < 1; p += 0.05) {
    const buf = sine(4096, period, p);
    const start = findTriggerIndex(buf, DRAW);
    starts.push(buf[start]);                 // value AT the trigger
  }
  const allNearZeroAndRising = starts.every((v) => Math.abs(v) < 0.05);
  check("trigger lands on a zero-crossing regardless of capture phase",
        allNearZeroAndRising,
        `max |v| = ${Math.max(...starts.map(Math.abs)).toFixed(4)}`);

  // And the plotted window is phase-locked: sample the same x each time.
  const atX = [];
  for (let p = 0; p < 1; p += 0.05) {
    const buf = sine(4096, period, p);
    atX.push(triggeredView(buf, DRAW)[period >> 2]);   // quarter period in
  }
  const spread = Math.max(...atX) - Math.min(...atX);
  check("plotted waveform is stable across capture phases", spread < 0.05,
        `spread = ${spread.toFixed(4)}`);
}

// ── rising, not falling ──────────────────────────────────────────────────
{
  const buf = sine(4096, 128);
  const i = findTriggerIndex(buf, DRAW);
  check("trigger is a RISING crossing", buf[i] > 0 && buf[i - 1] <= 0,
        `prev=${buf[i - 1].toFixed(4)} cur=${buf[i].toFixed(4)}`);
}

// ── degenerate inputs must not freeze or blank the scope ────────────────
{
  check("all-zero buffer returns 0", findTriggerIndex(new Float32Array(4096), DRAW) === 0);
  check("constant DC returns 0", findTriggerIndex(new Float32Array(4096).fill(0.5), DRAW) === 0);
  check("negative DC returns 0", findTriggerIndex(new Float32Array(4096).fill(-0.5), DRAW) === 0);
  check("empty buffer returns 0", findTriggerIndex(new Float32Array(0), DRAW) === 0);
  check("buffer shorter than the draw window returns 0",
        findTriggerIndex(sine(256, 64), DRAW) === 0);
}

// ── the off-by-one that runs the plot off the end ───────────────────────
{
  // A very low frequency: the first rising crossing falls late in the buffer.
  const buf = sine(4096, 3000, 0.5);
  const i = findTriggerIndex(buf, DRAW);
  check("never returns a start that overruns the buffer", i <= 4096 - DRAW,
        `start=${i} limit=${4096 - DRAW}`);
  check("triggered view is exactly drawLength samples",
        triggeredView(buf, DRAW).length === DRAW);
}

// ── hysteresis: noise sitting on zero must not retrigger every sample ────
{
  // Amplitude well below the arming threshold: treat as silence, draw from 0.
  const tiny = sine(4096, 128, 0, 0.001);
  check("sub-threshold signal returns 0 (treated as silence)",
        findTriggerIndex(tiny, DRAW) === 0);

  // A real signal with noise riding on it still triggers on the real crossing.
  const noisy = sine(4096, 128);
  for (let i = 0; i < noisy.length; i++) noisy[i] += (i % 7 - 3) * 0.002;
  const i = findTriggerIndex(noisy, DRAW);
  check("noise on zero does not defeat the trigger", i > 0 && Math.abs(noisy[i]) < 0.1,
        `start=${i} v=${noisy[i].toFixed(4)}`);
}

// ── threshold is tunable ─────────────────────────────────────────────────
{
  const quiet = sine(4096, 128, 0, 0.005);
  check("a lower threshold triggers on a quiet signal",
        findTriggerIndex(quiet, DRAW, { threshold: 0.001 }) > 0);
}

// ── plain arrays work too (not just Float32Array) ────────────────────────
{
  const arr = Array.from(sine(4096, 128));
  check("accepts a plain number[]", findTriggerIndex(arr, DRAW) > 0);
  check("triggeredView slices a plain array", triggeredView(arr, DRAW).length === DRAW);
}

console.log(`\n${failures === 0 ? "✅ ALL CHECKS PASSED" : `❌ ${failures} FAILED`}`);
process.exit(failures === 0 ? 0 : 1);
