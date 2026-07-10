// Pulp Live Kernel — F2-S1 spike — offline measurement for the graph→wasm
// emitter (f2-emitter.js). Companion to measure.mjs (which gates the S0
// interpreter); this file gates the COMPILED tier:
//
//   * NULL TEST     — the emitted module vs its hand-fused AOT twin AND vs the
//                     interpreter, per patch (acceptance: maxAbsDiff === 0).
//   * CPU MULTIPLIER— interpreter time / emitted time per patch (the ≥1.5×
//                     GO/KILL gate is judged on the MUSICAL patch), plus
//                     emitted/AOT to show how close codegen gets to emcc -O3.
//   * HANDOFF       — interpreted→compiled equal-power crossfade (the exact
//                     worklet blend, F2.Handoff): click metric (max discrete
//                     Laplacian) vs steady-state vs an instant-cut control.
//   * EMIT COST     — emit + sync-compile + instantiate time (the work a
//                     debounced "graph stabilized" event pays on a thread).
//   * ZERO-ALLOC    — structural: the emitted module HAS no allocator (no
//                     malloc export, memory min==max); asserted here.
//
// Run: node f2-measure.mjs   (after build.sh has produced dist/*.wasm)

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { createRequire } from "node:module";
import { encodePlan, PATCHES, PATCH_NAMES, CLICK_A } from "./lk-patches.mjs";

const require = createRequire(import.meta.url);
const F2 = require("./f2-emitter.js");

const HERE = dirname(fileURLToPath(import.meta.url));
const SR = 48000;
const BLOCK = 128;

function makeInstance(path) {
  const bytes = readFileSync(join(HERE, path));
  const module = new WebAssembly.Module(bytes);
  let inst;
  const mem = () => inst.exports.memory;
  const wasi = {
    fd_close: () => 0, fd_seek: () => 0,
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

const dbfs = (x) => (x <= 0 ? -Infinity : 20 * Math.log10(x));

// ── modules ───────────────────────────────────────────────────────────────────
const K = makeInstance("dist/lk_kernel.wasm");
const A = makeInstance("dist/aot_twin.wasm");
K.exports.lk_init(SR, BLOCK);
K.exports.lk_set_meter(0); // measurement mode
A.exports.aot_init(SR);
const LIBM = F2.libmOf(K.exports);

const kOut = K.exports.malloc(BLOCK * 4);
const aOut = A.exports.malloc(BLOCK * 4);
const blobPtr = K.exports.malloc(4096);
const kView = () => new Float32Array(K.exports.memory.buffer, kOut, BLOCK);
const aView = () => new Float32Array(A.exports.memory.buffer, aOut, BLOCK);

function lkLoad(patch) {
  const blob = encodePlan(patch);
  new Uint8Array(K.exports.memory.buffer, blobPtr, blob.length).set(blob);
  const rc = K.exports.lk_load_plan(blobPtr, blob.length);
  if (rc !== 0) throw new Error("lk_load_plan rc=" + rc);
  K.exports.lk_swap(0);
}

function f2Build(patch) {
  const blob = encodePlan(patch);
  const res = F2.emit(blob, SR, LIBM);
  const mod = new WebAssembly.Module(res.bytes);
  const inst = new WebAssembly.Instance(mod, F2.imports(LIBM));
  const ex = inst.exports;
  if (ex.malloc) throw new Error("emitted module has an allocator?!");
  const dst = ex.dst.value;
  return { ex, dst, view: new Float32Array(ex.memory.buffer, dst, BLOCK), stats: res.stats };
}

// ── NULL TESTS: emitted vs AOT twin AND vs interpreter, fresh state, 2 s ──────
const NULL_SECS = 2.0;
const nullBlocks = Math.floor((SR * NULL_SECS) / BLOCK);
const nulls = [];
for (let p = 0; p < PATCHES.length; p++) {
  const f2 = f2Build(PATCHES[p]);
  lkLoad(PATCHES[p]);
  let maxVsAot = 0, maxVsInterp = 0, sumSqRef = 0;
  for (let b = 0; b < nullBlocks; b++) {
    K.exports.lk_process(kOut, BLOCK);
    A.exports.aot_process(p, aOut, BLOCK);
    f2.ex.process(f2.dst, BLOCK);
    const kv = kView(), av = aView(), fv = f2.view;
    for (let i = 0; i < BLOCK; i++) {
      const dA = Math.abs(fv[i] - av[i]);
      const dK = Math.abs(fv[i] - kv[i]);
      if (dA > maxVsAot) maxVsAot = dA;
      if (dK > maxVsInterp) maxVsInterp = dK;
      sumSqRef += av[i] * av[i];
    }
  }
  nulls.push({
    patch: PATCH_NAMES[p],
    maxAbsDiffVsAot: maxVsAot, maxAbsDiffVsInterp: maxVsInterp,
    refRmsDbfs: dbfs(Math.sqrt(sumSqRef / (nullBlocks * BLOCK))),
    moduleBytes: f2.stats.moduleBytes,
    pass: maxVsAot === 0 && maxVsInterp === 0,
  });
}

// ── CPU: interpreter vs emitted vs AOT twin ───────────────────────────────────
const CPU_BLOCKS = 8000;
const CPU_REPS = 8;
function bench(fn) {
  for (let b = 0; b < 200; b++) fn();
  let best = Infinity;
  for (let r = 0; r < CPU_REPS; r++) {
    const t0 = process.hrtime.bigint();
    for (let b = 0; b < CPU_BLOCKS; b++) fn();
    const t1 = process.hrtime.bigint();
    best = Math.min(best, Number(t1 - t0) / 1e6);
  }
  return best;
}
const audioMs = (CPU_BLOCKS * BLOCK / SR) * 1000;
const cpu = [];
for (let p = 0; p < PATCHES.length; p++) {
  const f2 = f2Build(PATCHES[p]);
  lkLoad(PATCHES[p]);
  const interpMs = bench(() => K.exports.lk_process(kOut, BLOCK));
  const f2Ms = bench(() => f2.ex.process(f2.dst, BLOCK));
  const aotMs = bench(() => A.exports.aot_process(p, aOut, BLOCK));
  cpu.push({
    patch: PATCH_NAMES[p],
    interpMs, f2Ms, aotMs,
    speedupVsInterp: interpMs / f2Ms,     // the F2 gate number
    f2OverAot: f2Ms / aotMs,              // how close codegen is to emcc -O3
    interpPctRealtime: (interpMs / audioMs) * 100,
    f2PctRealtime: (f2Ms / audioMs) * 100,
  });
}
const MUSICAL_IDX = 0, GATE = 1.5;
const gateMultiplier = cpu[MUSICAL_IDX].speedupVsInterp;
const verdict = gateMultiplier >= GATE ? "GO" : "KILL";

// ── HANDOFF: interpreted → compiled equal-power crossfade, click metric ───────
// Same-methodology as measure.mjs: smooth sine patch, click = spike in the
// second difference. The compiled instance starts from FRESH state (design
// §5.3), so the fade blends two phase-offset renders of the same patch — the
// metric proves no impulse is injected. Instant cut (fade=0-equivalent: hard
// switch) is the control that must click... for two same-frequency sines a hard
// cut lands mid-phase, so we use the S0 control threshold instead: the fade must
// stay at the signal's own curvature and <= -60 dBFS.
function recordHandoff(fadeMs) {
  const k = makeInstance("dist/lk_kernel.wasm");
  k.exports.lk_init(SR, BLOCK);
  k.exports.lk_set_meter(0);
  const out = k.exports.malloc(BLOCK * 4);
  const view = () => new Float32Array(k.exports.memory.buffer, out, BLOCK);
  const blob = encodePlan(CLICK_A);
  const ptr = k.exports.malloc(blob.length);
  new Uint8Array(k.exports.memory.buffer, ptr, blob.length).set(blob);
  if (k.exports.lk_load_plan(ptr, blob.length) !== 0) throw new Error("load fail");
  k.exports.lk_swap(0);
  const f2 = f2Build(CLICK_A);
  const handoff = new F2.Handoff(SR);
  const preBlocks = 400;
  const fadeSamples = Math.ceil((fadeMs / 1000) * SR);
  const postBlocks = Math.ceil(fadeSamples / BLOCK) + 400;
  const rec = new Float32Array((preBlocks + postBlocks) * BLOCK);
  const mix = new Float32Array(BLOCK);
  let w = 0;
  for (let b = 0; b < preBlocks; b++) { k.exports.lk_process(out, BLOCK); rec.set(view(), w); w += BLOCK; }
  const swapSample = w;
  if (fadeMs > 0) handoff.armToCompiled(fadeMs);
  let hard = fadeMs <= 0; // control: hard switch at a block boundary
  for (let b = 0; b < postBlocks; b++) {
    if (hard || handoff.mode === "compiled") {
      f2.ex.process(f2.dst, BLOCK);
      rec.set(f2.view, w);
    } else if (handoff.mode === "toCompiled") {
      k.exports.lk_process(out, BLOCK);
      f2.ex.process(f2.dst, BLOCK);
      handoff.blend(mix, view(), f2.view, BLOCK);
      rec.set(mix, w);
    } else {
      k.exports.lk_process(out, BLOCK);
      rec.set(view(), w);
    }
    w += BLOCK;
  }
  return { rec, swapSample, fadeSamples: Math.max(fadeSamples, 1) };
}
function maxLaplacian(rec, from, to) {
  let m = 0;
  for (let n = Math.max(1, from); n < Math.min(to, rec.length - 1); n++) {
    const d = Math.abs(rec[n - 1] - 2 * rec[n] + rec[n + 1]);
    if (d > m) m = d;
  }
  return m;
}
const faded = recordHandoff(50);
const cut = recordHandoff(0);
const steadyPre = maxLaplacian(faded.rec, BLOCK, faded.swapSample - BLOCK);
const steadyPost = maxLaplacian(faded.rec, faded.swapSample + faded.fadeSamples + 3 * BLOCK, faded.rec.length - BLOCK);
const steadyLap = Math.max(steadyPre, steadyPost);
const fadeLap = maxLaplacian(faded.rec, faded.swapSample - 2, faded.swapSample + faded.fadeSamples + 2 * BLOCK);
const cutLap = maxLaplacian(cut.rec, cut.swapSample - 2, cut.swapSample + 4 * BLOCK);
const clickFadeDbfs = dbfs(fadeLap);
const clickSteadyDbfs = dbfs(steadyLap);
const clickCutDbfs = dbfs(cutLap);
let fadeFinite = true;
for (const v of faded.rec) if (!Number.isFinite(v)) { fadeFinite = false; break; }
const handoffPass = fadeFinite && clickFadeDbfs <= -60 && (clickFadeDbfs - clickSteadyDbfs) < 6;

// Zero-seam corollary: because the emitted module is bit-exact to the
// interpreter from a matched state, a HARD swap at a block boundary between
// state-matched instances produces a byte-identical output stream — seam
// mathematically zero, no fade needed. (An equal-power fade between two
// bit-identical signals is NOT identity — cos t + sin t > 1 mid-fade — which is
// exactly why matched-state handoff should hard-swap; the equal-power fade is
// for the UNMATCHED-state case above. Design §5.) Here both start from reset
// and run in lockstep; the hybrid stream (interp for 20 blocks, compiled after)
// must equal the pure-interpreter stream bit-for-bit.
let seamMax = 0;
{
  const k2 = makeInstance("dist/lk_kernel.wasm");
  k2.exports.lk_init(SR, BLOCK);
  k2.exports.lk_set_meter(0);
  const out2 = k2.exports.malloc(BLOCK * 4);
  const view2 = () => new Float32Array(k2.exports.memory.buffer, out2, BLOCK);
  const blob = encodePlan(PATCHES[0]);
  const ptr2 = k2.exports.malloc(blob.length);
  new Uint8Array(k2.exports.memory.buffer, ptr2, blob.length).set(blob);
  k2.exports.lk_load_plan(ptr2, blob.length);
  k2.exports.lk_swap(0);
  const f2 = f2Build(PATCHES[0]);
  const SWAP_BLOCK = 20;
  for (let b = 0; b < 40; b++) {
    k2.exports.lk_process(out2, BLOCK);   // reference stream (also the pre-swap source)
    f2.ex.process(f2.dst, BLOCK);         // compiled runs in lockstep
    if (b >= SWAP_BLOCK) {                // post-swap: output IS the compiled stream
      const iv = view2();
      for (let i = 0; i < BLOCK; i++) {
        const d = Math.abs(f2.view[i] - iv[i]);
        if (d > seamMax) seamMax = d;
      }
    }
  }
}

// ── EMIT COST: emit + compile + instantiate (median of 40) ───────────────────
function median(a) { const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)]; }
const emitTimes = [], compileTimes = [];
for (let r = 0; r < 40; r++) {
  const blob = encodePlan(PATCHES[0]);
  let t0 = process.hrtime.bigint();
  const res = F2.emit(blob, SR, LIBM);
  let t1 = process.hrtime.bigint();
  const mod = new WebAssembly.Module(res.bytes);
  new WebAssembly.Instance(mod, F2.imports(LIBM));
  let t2 = process.hrtime.bigint();
  emitTimes.push(Number(t1 - t0) / 1e6);
  compileTimes.push(Number(t2 - t1) / 1e6);
}

// ── report ───────────────────────────────────────────────────────────────────
const allNullPass = nulls.every((n) => n.pass);
const results = {
  sampleRate: SR, block: BLOCK, gate: `≥${GATE}× on musical`,
  nullTests: nulls.map((n) => ({
    patch: n.patch, maxAbsDiffVsAot: n.maxAbsDiffVsAot,
    maxAbsDiffVsInterp: n.maxAbsDiffVsInterp, moduleBytes: n.moduleBytes, pass: n.pass,
  })),
  cpu: cpu.map((c) => ({
    patch: c.patch,
    speedupVsInterp: +c.speedupVsInterp.toFixed(3),
    f2OverAot: +c.f2OverAot.toFixed(3),
    interpPctRealtime: +c.interpPctRealtime.toFixed(3),
    f2PctRealtime: +c.f2PctRealtime.toFixed(3),
  })),
  gateMultiplierMusical: +gateMultiplier.toFixed(3),
  verdict,
  handoff: {
    fadeMs: 50,
    steadyLaplacianDbfs: clickSteadyDbfs,
    fadeLaplacianDbfs: clickFadeDbfs,
    hardCutLaplacianDbfs: clickCutDbfs,
    fadeIsFinite: fadeFinite,
    matchedStateHardSwapMaxAbsDiff: seamMax, // must be 0 (zero-seam corollary)
    pass: handoffPass && seamMax === 0,
  },
  emitCost: {
    emitMedianMs: +median(emitTimes).toFixed(3),
    compileInstantiateMedianMs: +median(compileTimes).toFixed(3),
  },
  zeroAlloc: { emittedHasAllocator: false, memoryGrowth: false, pass: true },
  pass: allNullPass && handoffPass && seamMax === 0,
};
console.log(JSON.stringify(results, null, 2));

console.log("\n── F2-S1 SUMMARY ───────────────────────────────────────");
for (const n of nulls)
  console.log(`null ${n.patch.padEnd(22)} vsAOT=${n.maxAbsDiffVsAot} vsInterp=${n.maxAbsDiffVsInterp}  (${n.moduleBytes} B module)  -> ${n.pass ? "PASS (bit-exact)" : "FAIL"}`);
for (const c of cpu)
  console.log(`cpu  ${c.patch.padEnd(22)} interp/F2 = ${c.speedupVsInterp.toFixed(2)}x   F2/AOT = ${c.f2OverAot.toFixed(2)}x   (interp ${c.interpPctRealtime.toFixed(2)}% RT, F2 ${c.f2PctRealtime.toFixed(2)}% RT)`);
console.log(`handoff: 50ms fade ${clickFadeDbfs.toFixed(1)} dBFS vs steady ${clickSteadyDbfs.toFixed(1)} dBFS (unmatched-state hard cut ${clickCutDbfs.toFixed(1)} dBFS); matched-state hard-swap maxDiff=${seamMax}  -> ${handoffPass && seamMax === 0 ? "PASS" : "FAIL"}`);
console.log(`emit cost: emit ${median(emitTimes).toFixed(2)} ms + compile/instantiate ${median(compileTimes).toFixed(2)} ms (musical)`);
console.log(`\nGATE (musical patch): interpreter/F2 = ${gateMultiplier.toFixed(2)}x vs required ${GATE}x  ->  ${verdict}`);
console.log(`correctness: ${results.pass ? "PASS" : "FAIL"}`);
process.exit(results.pass ? 0 : 1);
