#!/usr/bin/env node
// harness1-aot-baseline.mjs — GATE: the AOT CPU reference number.
//
// Measures the steady-state per-quantum CPU cost of an EXISTING built Pulp WAM
// plugin (the AOT-compiled DSP). This is the reference the future kernel VM /
// interpreter's "<= 2x AOT CPU" gate (build plan S0 acceptance #2) compares
// against. Reports µs/quantum and % of the 128-frame real-time budget @ 48 kHz.
//
// Usage:
//   node harness1-aot-baseline.mjs --wasm <PulpGain.wasm> [options]
//   --engine chrome|node   (default chrome — the deployment-representative V8)
//   --sample-rate 48000  --frames 128  --channels 2
//   --freq 220           input sine frequency
//   --params '{"1":0}'   parameter map applied before timing
//   --midi-note 60       trigger a synth voice (note-on) before timing
//   --settle 64          quanta to run after triggering, before timing
//   --quanta 20000  --trials 5  --warmup 2000
//   --budget-max <pct>   optional: FAIL if budget% exceeds this (default: none)
//   --browser <path>  --headed
//
// Prints one PASS/measured-value line; exits non-zero on harness failure.
import { arg, flag, withChromePage, report } from "./lib/harness-util.mjs";
import { fmtBudget } from "./lib/fmt.mjs";
import { toServedUrl } from "./lib/serve.mjs";

const wasm = arg("--wasm");
if (!wasm) { console.error("FAIL: --wasm <path> is required"); process.exit(2); }
const engine = arg("--engine", "chrome");
const opts = {
  sampleRate: Number(arg("--sample-rate", 48000)),
  frames: Number(arg("--frames", 128)),
  channels: Number(arg("--channels", 2)),
  freqHz: Number(arg("--freq", 220)),
  quanta: Number(arg("--quanta", 20000)),
  trials: Number(arg("--trials", 5)),
  warmup: Number(arg("--warmup", 2000)),
  settleQuanta: Number(arg("--settle", 0)),
};
if (arg("--params")) opts.params = JSON.parse(arg("--params"));
if (arg("--midi-note")) opts.midi = [[0x90, Number(arg("--midi-note")), 100, 0]];
const budgetMax = arg("--budget-max") ? Number(arg("--budget-max")) : null;

try {
  let r;
  if (engine === "node") {
    const eng = await import("./lib/node-engine.mjs");
    r = eng.cpu(wasm, opts);
  } else {
    const url = toServedUrl(wasm);
    r = await withChromePage("http://localhost:8794" + "/examples/web-demos/compiler-spike/measure/page/measure.html",
      (page) => page.evaluate((a) => window.MeasureAPI.cpu(a.url, a.opts), { url, opts }),
      { headed: flag("--headed") });
  }
  const ok = Number.isFinite(r.usPerQuantumMedian) && r.usPerQuantumMedian > 0 &&
    (budgetMax === null || r.budgetPctMedian <= budgetMax);
  console.log(`AOT baseline (${r.engine}) ${wasm.split("/").pop()} @ ${r.sampleRate/1000}kHz/${r.frames}:`);
  console.log(`  median ${r.usPerQuantumMedian.toFixed(3)} µs/quantum  = ${fmtBudget(r.budgetPctMedian)} of budget`);
  console.log(`  min    ${r.usPerQuantumMin.toFixed(3)} µs/quantum  = ${fmtBudget(r.budgetPctMin)} of budget`);
  console.log(`  (quantum budget ${r.quantumBudgetUs.toFixed(1)} µs; ${r.trials} trials × ${r.quanta} quanta)`);
  report(ok, `AOT ${r.usPerQuantumMedian.toFixed(2)} µs/quantum (${r.budgetPctMedian.toFixed(2)}% of budget)` +
    (budgetMax !== null ? ` [gate <= ${budgetMax}%]` : ""));
} catch (e) {
  report(false, "harness error: " + (e?.message || e));
}
