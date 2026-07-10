// Pulp Live Kernel — S0 spike — offline (headless, no browser) measurement.
//
// Instantiates both standalone wasm modules the SAME way the worklet does
// (byte-transfer + synchronous new WebAssembly.Module + minimal WASI shim,
// internal memory) and measures, in Node's V8 wasm engine:
//   * NULL TEST  — kernel-VM musical render vs the AOT twin (execution
//                  equivalence; acceptance: residual <= -60 dBFS).
//   * CPU RATIO  — kernel time / AOT time on all three patches.
//   * ZERO-ALLOC — lk_alloc_count() delta across a long render AND across a
//                  load_plan + equal-power swap-fade (acceptance: 0).
//
// The browser harness (validate.mjs) additionally proves REAL-TIME and
// edit->sound latency; this file proves the DSP + RT-safety numbers headlessly.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { encodePlan, PATCHES, PATCH_NAMES, CLICK_A, CLICK_B } from "./lk-patches.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const SR = 48000;
const BLOCK = 128;

function makeInstance(path) {
  const bytes = readFileSync(join(HERE, path));
  const module = new WebAssembly.Module(bytes); // synchronous, as in the worklet
  let inst;
  const mem = () => inst.exports.memory;
  const wasi = {
    fd_close: () => 0,
    fd_seek: () => 0,
    fd_write: (fd, iov, cnt, pw) => {
      const dv = new DataView(mem().buffer);
      let total = 0;
      for (let i = 0; i < cnt; i++) total += dv.getUint32(iov + i * 8 + 4, true);
      dv.setUint32(pw, total, true);
      return 0;
    },
  };
  inst = new WebAssembly.Instance(module, { wasi_snapshot_preview1: wasi });
  inst.exports._initialize();
  return inst;
}

function loadBlob(k, patch) {
  const blob = encodePlan(patch);
  const ptr = k.exports.malloc(blob.length);
  new Uint8Array(k.exports.memory.buffer, ptr, blob.length).set(blob);
  const rc = k.exports.lk_load_plan(ptr, blob.length);
  k.exports.free(ptr);
  if (rc !== 0) throw new Error("lk_load_plan failed rc=" + rc);
}

// ── set up both modules ──────────────────────────────────────────────────────
const K = makeInstance("dist/lk_kernel.wasm");
const A = makeInstance("dist/aot_twin.wasm");
K.exports.lk_init(SR, BLOCK);
A.exports.aot_init(SR);
const kOut = K.exports.malloc(BLOCK * 4);
const aOut = A.exports.malloc(BLOCK * 4);
const kView = () => new Float32Array(K.exports.memory.buffer, kOut, BLOCK);
const aView = () => new Float32Array(A.exports.memory.buffer, aOut, BLOCK);

// ── NULL TEST: kernel musical vs AOT patch 0 (fresh state, from sample 0) ─────
loadBlob(K, PATCHES[0]);
K.exports.lk_swap(0); // instant activate (no fade) so both render from block 0

const NULL_SECS = 2.0;
const nullBlocks = Math.floor((SR * NULL_SECS) / BLOCK);
let maxDiff = 0, sumSqDiff = 0, sumSqRef = 0, count = 0;
for (let b = 0; b < nullBlocks; b++) {
  K.exports.lk_process(kOut, BLOCK);
  A.exports.aot_process(0, aOut, BLOCK);
  const kv = kView(), av = aView();
  for (let i = 0; i < BLOCK; i++) {
    const d = kv[i] - av[i];
    const ad = Math.abs(d);
    if (ad > maxDiff) maxDiff = ad;
    sumSqDiff += d * d; sumSqRef += av[i] * av[i]; count++;
  }
}
const rmsDiff = Math.sqrt(sumSqDiff / count);
const rmsRef = Math.sqrt(sumSqRef / count);
const dbfs = (x) => (x <= 0 ? -Infinity : 20 * Math.log10(x));
const nullResidualDbfs = dbfs(rmsDiff);
const peakDiffDbfs = dbfs(maxDiff);

// ── CPU RATIO on all three patches ───────────────────────────────────────────
function timeKernel(patchIndex, blocks, reps) {
  loadBlob(K, PATCHES[patchIndex]);
  K.exports.lk_swap(0);
  // warm
  for (let b = 0; b < 200; b++) K.exports.lk_process(kOut, BLOCK);
  let best = Infinity;
  for (let r = 0; r < reps; r++) {
    const t0 = process.hrtime.bigint();
    for (let b = 0; b < blocks; b++) K.exports.lk_process(kOut, BLOCK);
    const t1 = process.hrtime.bigint();
    best = Math.min(best, Number(t1 - t0) / 1e6);
  }
  return best;
}
function timeTwin(patchIndex, blocks, reps) {
  for (let b = 0; b < 200; b++) A.exports.aot_process(patchIndex, aOut, BLOCK);
  let best = Infinity;
  for (let r = 0; r < reps; r++) {
    const t0 = process.hrtime.bigint();
    for (let b = 0; b < blocks; b++) A.exports.aot_process(patchIndex, aOut, BLOCK);
    const t1 = process.hrtime.bigint();
    best = Math.min(best, Number(t1 - t0) / 1e6);
  }
  return best;
}

const CPU_BLOCKS = 8000; // ~21 s of audio per rep
const CPU_REPS = 8;
const cpu = [];
for (let p = 0; p < PATCHES.length; p++) {
  const kms = timeKernel(p, CPU_BLOCKS, CPU_REPS);
  const ams = timeTwin(p, CPU_BLOCKS, CPU_REPS);
  const audioMs = (CPU_BLOCKS * BLOCK / SR) * 1000;
  cpu.push({
    name: PATCH_NAMES[p],
    kernelMs: kms, aotMs: ams, ratio: kms / ams,
    kernelRtPct: (kms / audioMs) * 100, aotRtPct: (ams / audioMs) * 100,
  });
}

// ── ZERO-ALLOC: process() delta, then load_plan + swap-fade delta ────────────
const allocBeforeProc = K.exports.lk_alloc_count();
for (let b = 0; b < 20000; b++) K.exports.lk_process(kOut, BLOCK);
const allocAfterProc = K.exports.lk_alloc_count();

const allocBeforeSwap = K.exports.lk_alloc_count();
loadBlob(K, PATCHES[0]);            // build a new plan into the inactive slot
K.exports.lk_swap(50);             // arm a 50 ms equal-power crossfade
const fadeBlocks = Math.ceil((0.050 * SR) / BLOCK) + 20;
for (let b = 0; b < fadeBlocks; b++) K.exports.lk_process(kOut, BLOCK); // render through the fade
const allocAfterSwap = K.exports.lk_alloc_count();

// ── PLAN-BUILD COST on the audio thread (F0.4 one-quantum budget = 2.67 ms).
//    Precise timing in Node (AudioWorkletGlobalScope has no performance.now, so
//    the browser can't time this; same V8 wasm engine here). ────────────────────
function timeLoad(patch, reps) {
  const blob = encodePlan(patch);
  const ptr = K.exports.malloc(blob.length);
  new Uint8Array(K.exports.memory.buffer, ptr, blob.length).set(blob);
  for (let i = 0; i < 100; i++) K.exports.lk_load_plan(ptr, blob.length); // warm
  let best = Infinity;
  for (let r = 0; r < reps; r++) {
    const t0 = process.hrtime.bigint();
    K.exports.lk_load_plan(ptr, blob.length);
    const t1 = process.hrtime.bigint();
    best = Math.min(best, Number(t1 - t0));
  }
  K.exports.free(ptr);
  return best / 1000; // microseconds
}
// A near-max 64-node chain (osc + 63 gains) to stress plan-build.
const CHAIN64 = {
  nodes: [{ type: 0, params: [[0, 110], [1, 1], [2, 0.3]] },
    ...Array.from({ length: 63 }, () => ({ type: 1, params: [[0, 0.0]] }))],
  edges: Array.from({ length: 63 }, (_, i) => ({ src: i, dst: i + 1 })),
  output: 63,
};
const planBuildMusicalUs = timeLoad(PATCHES[0], 4000);
const planBuild64Us = timeLoad(CHAIN64, 4000);

// ── CLICK-FREE crossfade: record a real equal-power swap between two SMOOTH
//    (sine) patches; a click is an impulsive discontinuity, detected as a spike
//    in the second difference (discrete Laplacian). Compare a 50 ms equal-power
//    fade against an instant (fade=0) cut control that MUST click. ─────────────
function recordSwap(fadeMs) {
  const k = makeInstance("dist/lk_kernel.wasm");
  k.exports.lk_init(SR, BLOCK);
  const out = k.exports.malloc(BLOCK * 4);
  const view = () => new Float32Array(k.exports.memory.buffer, out, BLOCK);
  loadBlobObj(k, CLICK_A); k.exports.lk_swap(0);
  const preBlocks = 400;
  const fadeSamples = Math.ceil((fadeMs / 1000) * SR);
  const postBlocks = Math.ceil(fadeSamples / BLOCK) + 400;
  const rec = new Float32Array((preBlocks + postBlocks) * BLOCK);
  let w = 0;
  for (let b = 0; b < preBlocks; b++) { k.exports.lk_process(out, BLOCK); rec.set(view(), w); w += BLOCK; }
  const swapSample = w;
  loadBlobObj(k, CLICK_B); k.exports.lk_swap(fadeMs);
  for (let b = 0; b < postBlocks; b++) { k.exports.lk_process(out, BLOCK); rec.set(view(), w); w += BLOCK; }
  return { rec, swapSample, fadeSamples };
}
function loadBlobObj(k, patch) { return loadBlob(k, patch); }
function maxLaplacian(rec, from, to) {
  let m = 0;
  for (let n = from + 1; n < to - 1; n++) {
    const d = Math.abs(rec[n - 1] - 2 * rec[n] + rec[n + 1]);
    if (d > m) m = d;
  }
  return m;
}
function anyNonFinite(rec, from, to) {
  for (let n = from; n < to; n++) if (!Number.isFinite(rec[n])) return true;
  return false;
}

const faded = recordSwap(50);
const cut   = recordSwap(0);
// Intrinsic curvature baseline = the LOUDER of the two endpoints (pre-swap 220 Hz
// and post-swap 330 Hz), since the fade window blends both; anything above this
// is an injected click.
const steadyPre  = maxLaplacian(faded.rec, BLOCK, faded.swapSample - BLOCK);
const steadyPost = maxLaplacian(faded.rec, faded.swapSample + faded.fadeSamples + 3 * BLOCK, faded.rec.length - BLOCK);
const steadyLap = Math.max(steadyPre, steadyPost);
const fadeLap   = maxLaplacian(faded.rec, faded.swapSample - 2, faded.swapSample + faded.fadeSamples + 2 * BLOCK);
const cutLap    = maxLaplacian(cut.rec, cut.swapSample - 2, cut.swapSample + 4 * BLOCK);
const clickFadeDbfs = dbfs(fadeLap);
const clickSteadyDbfs = dbfs(steadyLap);
const clickCutDbfs = dbfs(cutLap);
const clickFinite = !anyNonFinite(faded.rec, 0, faded.rec.length);

// ── report ───────────────────────────────────────────────────────────────────
const results = {
  sampleRate: SR, block: BLOCK,
  nullTest: {
    seconds: NULL_SECS,
    residualRmsDbfs: nullResidualDbfs,
    peakDiffDbfs: peakDiffDbfs,
    refRmsDbfs: dbfs(rmsRef),
    maxAbsDiff: maxDiff,
    pass: nullResidualDbfs <= -60,
  },
  cpu: cpu.map((c) => ({
    patch: c.name,
    ratioKernelOverAot: +c.ratio.toFixed(3),
    kernelPctRealtime: +c.kernelRtPct.toFixed(2),
    aotPctRealtime: +c.aotRtPct.toFixed(2),
  })),
  zeroAlloc: {
    processDelta: allocAfterProc - allocBeforeProc,
    swapFadeDelta: allocAfterSwap - allocBeforeSwap,
    totalAllocationsSinceInit: allocAfterSwap,
    pass: (allocAfterProc - allocBeforeProc) === 0 && (allocAfterSwap - allocBeforeSwap) === 0,
  },
  planBuild: {
    musical10NodeUs: +planBuildMusicalUs.toFixed(2),
    chain64NodeUs: +planBuild64Us.toFixed(2),
    oneQuantumBudgetUs: 2667,
    pass: planBuild64Us < 2667,
  },
  clickFree: {
    fadeMs: 50,
    steadyLaplacianDbfs: clickSteadyDbfs,
    fadeLaplacianDbfs: clickFadeDbfs,       // 50 ms equal-power fade
    instantCutLaplacianDbfs: clickCutDbfs,  // control: fade=0 (must click)
    fadeMinusSteadyDb: clickFadeDbfs - clickSteadyDbfs,
    fadeIsFinite: clickFinite,
    // click-free: the fade injects no impulse above the signal's own curvature
    // (fade ~= steady, both far below the instant-cut control) and stays <= -60.
    pass: clickFinite && clickFadeDbfs <= -60 &&
          (clickFadeDbfs - clickSteadyDbfs) < 6 &&
          (clickCutDbfs - clickFadeDbfs) > 30,
  },
};

console.log(JSON.stringify(results, null, 2));

// human summary
console.log("\n── SUMMARY ─────────────────────────────────────────────");
console.log(`null test:   residual ${nullResidualDbfs.toFixed(1)} dBFS, peak ${peakDiffDbfs.toFixed(1)} dBFS  -> ${results.nullTest.pass ? "PASS (<= -60)" : "FAIL"}`);
for (const c of cpu)
  console.log(`cpu ${c.name.padEnd(20)} kernel/AOT = ${c.ratio.toFixed(2)}x   (kernel ${c.kernelRtPct.toFixed(1)}% RT, AOT ${c.aotRtPct.toFixed(1)}% RT)`);
console.log(`plan-build:  musical(10n)=${planBuildMusicalUs.toFixed(1)}us  chain(64n)=${planBuild64Us.toFixed(1)}us  (budget 2667us)  -> ${results.planBuild.pass ? "PASS" : "FAIL"}`);
console.log(`zero-alloc:  process delta=${results.zeroAlloc.processDelta}, swap-fade delta=${results.zeroAlloc.swapFadeDelta}  -> ${results.zeroAlloc.pass ? "PASS (0)" : "FAIL"}`);
console.log(`click-free:  50ms-fade ${clickFadeDbfs.toFixed(1)} dBFS vs steady ${clickSteadyDbfs.toFixed(1)} dBFS vs instant-cut ${clickCutDbfs.toFixed(1)} dBFS  -> ${results.clickFree.pass ? "PASS (<= -60, no injected impulse)" : "FAIL"}`);
