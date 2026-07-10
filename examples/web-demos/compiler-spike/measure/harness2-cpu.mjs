#!/usr/bin/env node
// harness2-cpu.mjs — GENERIC CPU rig + the interpreter-vs-AOT ratio gate.
//
// Given ANY DSP module (built WAM plugin today; the kernel-VM / emitted-wasm
// module tomorrow — both satisfy the dsp-adapter contract) it measures
// steady-state per-process() cost. Given TWO modules it reports the ratio B/A
// and applies the "<= 2x AOT CPU" gate (build plan S0 acceptance #2, target
// <= 1.5x). Pass A = the AOT plugin and B = the interpreter to gate the spike.
//
// Usage:
//   node harness2-cpu.mjs --a <aot.wasm> [--b <candidate.wasm>] [options]
//   --engine chrome|node   (default chrome)
//   --ratio-max 2.0        gate: FAIL if B/A exceeds this (default 2.0)
//   --params-a / --params-b '{"1":0}'   per-module param maps
//   --midi-note-a / --midi-note-b 60    per-module note trigger (synths)
//   --settle 32  --freq 220  --quanta 20000 --trials 5 --warmup 2000
//   --sample-rate 48000 --frames 128 --channels 2  --browser <path> --headed
//
// Prints a PASS/measured-value line; exits non-zero on failure or a blown gate.
import { arg, flag, withChromePage, report } from "./lib/harness-util.mjs";
import { fmtBudget } from "./lib/fmt.mjs";
import { toServedUrl } from "./lib/serve.mjs";

const a = arg("--a");
const b = arg("--b");
if (!a) { console.error("FAIL: --a <wasm> is required"); process.exit(2); }
const engine = arg("--engine", "chrome");
const ratioMax = Number(arg("--ratio-max", 2.0));
const common = {
  sampleRate: Number(arg("--sample-rate", 48000)),
  frames: Number(arg("--frames", 128)),
  channels: Number(arg("--channels", 2)),
  freqHz: Number(arg("--freq", 220)),
  quanta: Number(arg("--quanta", 20000)),
  trials: Number(arg("--trials", 5)),
  warmup: Number(arg("--warmup", 2000)),
  settleQuanta: Number(arg("--settle", 0)),
};
function optsFor(suffix) {
  const o = { ...common };
  if (arg("--params-" + suffix)) o.params = JSON.parse(arg("--params-" + suffix));
  if (arg("--midi-note-" + suffix)) o.midi = [[0x90, Number(arg("--midi-note-" + suffix)), 100, 0]];
  return o;
}

async function measure(wasm, opts) {
  if (engine === "node") { const eng = await import("./lib/node-engine.mjs"); return eng.cpu(wasm, opts); }
  const url = toServedUrl(wasm);
  return withChromePage(
    "http://localhost:8794/examples/web-demos/compiler-spike/measure/page/measure.html",
    (page) => page.evaluate((x) => window.MeasureAPI.cpu(x.url, x.opts), { url, opts }),
    { headed: flag("--headed") });
}

try {
  const ra = await measure(a, optsFor("a"));
  console.log(`A (${ra.engine}) ${a.split("/").pop()}: ${ra.usPerQuantumMedian.toFixed(3)} µs/quantum = ${fmtBudget(ra.budgetPctMedian)} of budget`);
  if (!b) {
    report(Number.isFinite(ra.usPerQuantumMedian) && ra.usPerQuantumMedian > 0,
      `CPU ${ra.usPerQuantumMedian.toFixed(2)} µs/quantum (${ra.budgetPctMedian.toFixed(2)}% of budget)`);
  } else {
    const rb = await measure(b, optsFor("b"));
    const ratio = rb.usPerQuantumMedian / ra.usPerQuantumMedian;
    console.log(`B (${rb.engine}) ${b.split("/").pop()}: ${rb.usPerQuantumMedian.toFixed(3)} µs/quantum = ${fmtBudget(rb.budgetPctMedian)} of budget`);
    console.log(`  ratio B/A = ${ratio.toFixed(3)}×  (gate <= ${ratioMax}×, target <= 1.5×)`);
    report(Number.isFinite(ratio) && ratio <= ratioMax,
      `interpreter/AOT CPU ratio ${ratio.toFixed(3)}× (gate <= ${ratioMax}×)`);
  }
} catch (e) { report(false, "harness error: " + (e?.message || e)); }
