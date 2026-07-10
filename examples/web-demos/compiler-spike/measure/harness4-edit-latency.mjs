#!/usr/bin/env node
// harness4-edit-latency.mjs — GATE: edit→sound latency (<= 30 ms).
//
// Chrome-only (needs a real AudioContext + AudioWorklet + AnalyserNode). Runs a
// live oscillator → [WAM DSP in worklet] → analyser → out graph and times, in
// wall-clock, from "apply an edit" to "the change is audible in the output"
// (detected via the analyser). Backs build plan S0 acceptance #3 (structural
// edit round-trip <= 30 ms; param edit <= 5 ms).
//
// Two edits are measured, over N trials each:
//   param   — raise input gain silent→audible (the common live-coding edit)
//   swap    — structural-swap STUB: transfer fresh wasm bytes, sync-compile a new
//             slot in the worklet, time swap-post→audible (the teardown-free
//             mechanism the kernel VM will use).
//
// Usage:
//   node harness4-edit-latency.mjs --wasm <PulpGain.wasm> [options]
//   --trials 10  --gate-ms 30  --param-gate-ms 30
//   --sample-rate 48000  --browser <path>  --headed
//
// Prints PASS/measured-value; exits non-zero on failure or a blown gate.
import { arg, flag, withChromePage, report } from "./lib/harness-util.mjs";
import { toServedUrl } from "./lib/serve.mjs";

const wasm = arg("--wasm");
if (!wasm) { console.error("FAIL: --wasm <path> is required"); process.exit(2); }
const trials = Number(arg("--trials", 10));
const gateMs = Number(arg("--gate-ms", 30));
const paramGateMs = Number(arg("--param-gate-ms", gateMs));
const sampleRate = Number(arg("--sample-rate", 48000));
const url = toServedUrl(wasm);

const median = (xs) => { const s = [...xs].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; };
const max = (xs) => xs.reduce((m, x) => Math.max(m, x), -Infinity);

try {
  const out = await withChromePage(
    "http://localhost:8794/examples/web-demos/compiler-spike/measure/page/latency.html",
    async (page) => {
      await page.evaluate((a) => window.LatencyAPI.init(a.url, a.sampleRate), { url, sampleRate });
      const param = [], swap = [], audio = [];
      for (let i = 0; i < trials; i++) {
        const r = await page.evaluate(() => window.LatencyAPI.paramEdit({ toDb: 0 }));
        param.push(r.wallMs);
        if (r.audioApplyToNowMs != null) audio.push(r.audioApplyToNowMs);
      }
      for (let i = 0; i < trials; i++) {
        const r = await page.evaluate((a) => window.LatencyAPI.structuralSwap({ wasmUrl: a.url }), { url });
        swap.push(r.wallMs);
      }
      return { param, swap, audio };
    },
    { headed: flag("--headed") });

  const pMed = median(out.param), pMax = max(out.param);
  const sMed = median(out.swap), sMax = max(out.swap);
  console.log(`edit→sound latency (chrome, ${trials} trials each) ${wasm.split("/").pop()} @ ${sampleRate/1000}kHz:`);
  console.log(`  param edit:       median ${pMed.toFixed(2)} ms, max ${pMax.toFixed(2)} ms  (gate <= ${paramGateMs} ms)`);
  console.log(`  structural swap:  median ${sMed.toFixed(2)} ms, max ${sMax.toFixed(2)} ms  (gate <= ${gateMs} ms)`);
  if (out.audio.length) console.log(`  (audio-clock apply→audible: median ${median(out.audio).toFixed(2)} ms — the in-buffer floor)`);
  const ok = Number.isFinite(pMed) && pMed <= paramGateMs && Number.isFinite(sMed) && sMed <= gateMs;
  report(ok, `param ${pMed.toFixed(2)} ms (<= ${paramGateMs}), swap ${sMed.toFixed(2)} ms (<= ${gateMs})`);
} catch (e) { report(false, "harness error: " + (e?.message || e)); }
