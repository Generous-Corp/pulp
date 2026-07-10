#!/usr/bin/env node
// harness3-nulltest.mjs — GATE: correctness null test (interpreter ≈ AOT).
//
// Renders the SAME input through two DSP modules (or two param configs of one
// module) and measures:
//   - residual (difference-signal A-B) peak/RMS in dBFS — the null test proper,
//     backing "interpreter matches AOT to <= -60 dBFS" (build plan S0 #4 / F2
//     conformance). Two bit-identical renders → -inf.
//   - level delta (level(B)-level(A)) in dB — a sanity meter; a +6 dB gain diff
//     reads as +6 dB.
//
// SELF-CHECK (--self-check, needs only --a): proves the meter itself before it
// is trusted on real modules —
//   * identical render        → residual = -inf, level delta = 0 dB   (PASS)
//   * +6 dB output-gain diff   → level delta = +6 dB                   (PASS)
//
// Usage:
//   node harness3-nulltest.mjs --self-check --a <PulpGain.wasm> [--engine ..]
//   node harness3-nulltest.mjs --a <aot.wasm> --b <interp.wasm> [--gate-db -60]
//   --params-a/-b '{"2":0}'  --midi-note-a/-b 60  --settle 0
//   --seconds 1.0 --freq 220 --amp 0.25 --max-shift 0
//   --sample-rate 48000 --frames 128 --channels 2  --browser <path> --headed
//
// Prints PASS/measured-value; exits non-zero on failure or a blown gate.
import { arg, flag, withChromePage, report } from "./lib/harness-util.mjs";
import { fmtDb, fmtDelta } from "./lib/fmt.mjs";
import { toServedUrl } from "./lib/serve.mjs";

const engine = arg("--engine", "chrome");
const a = arg("--a");
if (!a) { console.error("FAIL: --a <wasm> is required"); process.exit(2); }
const base = {
  sampleRate: Number(arg("--sample-rate", 48000)),
  frames: Number(arg("--frames", 128)),
  channels: Number(arg("--channels", 2)),
  freqHz: Number(arg("--freq", 220)),
  amp: Number(arg("--amp", 0.25)),
  seconds: Number(arg("--seconds", 1.0)),
  maxShift: Number(arg("--max-shift", 0)),
  settleQuanta: Number(arg("--settle", 0)),
};

// Run one null test (two configs). In chrome, both loads happen in one page.
async function runNull(cfg) {
  if (engine === "node") {
    const eng = await import("./lib/node-engine.mjs");
    return eng.nullTest({ pathA: cfg.urlA, pathB: cfg.urlB, ...base, paramsA: cfg.paramsA, paramsB: cfg.paramsB, midiA: cfg.midiA, midiB: cfg.midiB });
  }
  const wcfg = { urlA: toServedUrl(cfg.urlA), urlB: toServedUrl(cfg.urlB), ...base, paramsA: cfg.paramsA, paramsB: cfg.paramsB, midiA: cfg.midiA, midiB: cfg.midiB };
  return withChromePage(
    "http://localhost:8794/examples/web-demos/compiler-spike/measure/page/measure.html",
    (page) => page.evaluate((c) => window.MeasureAPI.nullTest(c), wcfg),
    { headed: flag("--headed") });
}

try {
  if (flag("--self-check")) {
    // (1) identical → residual must be -inf (bit-identical deterministic render).
    const same = await runNull({ urlA: a, urlB: a });
    // (2) +6 dB output gain on B (param "2") → level delta must be +6 dB.
    const gain = await runNull({ urlA: a, urlB: a, paramsA: { "2": 0 }, paramsB: { "2": 6 } });
    console.log(`self-check (${same.engine}) on ${a.split("/").pop()}:`);
    console.log(`  identical:  residual peak ${fmtDb(same.residualPeakDb)}, rms ${fmtDb(same.residualRmsDb)}; level delta ${fmtDelta(same.levelDeltaRmsDb)}`);
    console.log(`  +6dB gain:  level delta ${fmtDelta(gain.levelDeltaRmsDb)} (rms), ${fmtDelta(gain.levelDeltaPeakDb)} (peak); residual ${fmtDb(gain.residualPeakDb)}`);
    const identOk = same.residualPeakDb <= -120 && Math.abs(same.levelDeltaRmsDb) < 0.01;
    const gainOk = Math.abs(gain.levelDeltaRmsDb - 6) < 0.2 && gain.residualPeakDb > -60;
    report(identOk && gainOk,
      `meter valid — identical→${fmtDb(same.residualPeakDb)} residual, +6dB→${fmtDelta(gain.levelDeltaRmsDb)} level delta`);
  } else {
    const b = arg("--b") || a;
    const cfg = { urlA: a, urlB: b };
    if (arg("--params-a")) cfg.paramsA = JSON.parse(arg("--params-a"));
    if (arg("--params-b")) cfg.paramsB = JSON.parse(arg("--params-b"));
    if (arg("--midi-note-a")) cfg.midiA = [[0x90, Number(arg("--midi-note-a")), 100, 0]];
    if (arg("--midi-note-b")) cfg.midiB = [[0x90, Number(arg("--midi-note-b")), 100, 0]];
    const gateDb = Number(arg("--gate-db", -60));
    const r = await runNull(cfg);
    console.log(`null test (${r.engine}) ${a.split("/").pop()} vs ${b.split("/").pop()} (${r.frames} frames${r.alignShift ? `, shift ${r.alignShift}` : ""}):`);
    console.log(`  residual: peak ${fmtDb(r.residualPeakDb)}, rms ${fmtDb(r.residualRmsDb)}`);
    console.log(`  level:    A ${fmtDb(r.rmsADb)} rms, B ${fmtDb(r.rmsBDb)} rms, delta ${fmtDelta(r.levelDeltaRmsDb)}`);
    report(r.residualPeakDb <= gateDb,
      `null residual ${fmtDb(r.residualPeakDb)} peak (gate <= ${gateDb} dBFS)`);
  }
} catch (e) { report(false, "harness error: " + (e?.message || e)); }
