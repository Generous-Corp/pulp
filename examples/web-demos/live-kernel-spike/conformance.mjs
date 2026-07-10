// Pulp Live Kernel — iteration 2 — per-node conformance (bit-exact null test).
//
// For EVERY node type in the registry, render a small graph two ways and assert
// they are bit-identical:
//   * kernel:    a real LKB0 blob (osc -> NODE, or NODE alone for sources) driven
//                through the full decode -> build_plan -> block executor path.
//   * AOT twin:  the same osc -> NODE chain hand-wired in aot_twin.cpp WITHOUT the
//                executor (no topo-sort / CSR gather / feedback capture).
// A per-node maxAbsDiff of 0 (residual -inf dBFS, gate <= -60) proves the
// executor's routing adds zero error for that node — the regression guard that
// keeps node breadth correct as the vocabulary grows (build-plan R4 / R3).
//
// Run: node conformance.mjs   (after build.sh has produced dist/*.wasm)

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { encodeLKB0, parsePatch } from "./lk-dsl.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const SR = 48000;
const BLOCK = 128;
const SECS = 1.0;
const NBLOCKS = Math.floor((SR * SECS) / BLOCK);

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

// Node-type wire ids (mirror experimental/live_kernel/codec.hpp).
const NT = {
  OSC: 0, GAIN: 1, BIQUAD: 2, LADDER: 3, ADSR: 4, DELAY: 5, MIXER: 6,
  SVF: 7, SHAPER: 8, DCBLOCK: 9, NOISE: 10, CHORUS: 11, REVERB: 12, COMP: 13,
};
const audioIns = (t) => (t === NT.OSC || t === NT.NOISE ? 0 : t === NT.MIXER ? 2 : 1);

// Each spec: the node type + the exact canonical (unit-normalized) params the
// blob will carry (verb-baked variant first, then knobs). The reference receives
// these same values, so any divergence is pure executor-routing error.
const SPECS = [
  { name: "gain",     type: NT.GAIN,    params: [[0, -3]] },
  { name: "biquad/peak", type: NT.BIQUAD, params: [[0, 5], [1, 2000], [2, 1], [3, 6]] },
  { name: "biquad/lowpass", type: NT.BIQUAD, params: [[0, 0], [1, 1200], [2, 0.707], [3, 0]] },
  // F2 iteration: the remaining biquad types, SVF modes, and shaper curves, so
  // every coefficient-derivation branch of the emitter is under the null test.
  { name: "biquad/highpass", type: NT.BIQUAD, params: [[0, 1], [1, 400], [2, 0.9], [3, 0]] },
  { name: "biquad/bandpass", type: NT.BIQUAD, params: [[0, 2], [1, 900], [2, 2], [3, 0]] },
  { name: "biquad/notch",    type: NT.BIQUAD, params: [[0, 3], [1, 660], [2, 1.5], [3, 0]] },
  { name: "biquad/allpass",  type: NT.BIQUAD, params: [[0, 4], [1, 1500], [2, 0.8], [3, 0]] },
  { name: "biquad/lowshelf", type: NT.BIQUAD, params: [[0, 6], [1, 300], [2, 0.9], [3, 4.5]] },
  { name: "biquad/highshelf", type: NT.BIQUAD, params: [[0, 7], [1, 4000], [2, 0.9], [3, -3]] },
  { name: "ladder",   type: NT.LADDER,  params: [[0, 1200], [1, 0.5]] },
  { name: "adsr",     type: NT.ADSR,    params: [[0, 0.001], [1, 0.5], [2, 0.8], [3, 0.1], [4, 1]] },
  { name: "delay",    type: NT.DELAY,   params: [[0, 0.18], [1, 0.35], [2, 0.4]] },
  { name: "mixer",    type: NT.MIXER,   params: [[0, 0.5]] },
  { name: "svf",      type: NT.SVF,     params: [[0, 0], [1, 1200], [2, 0.5]] },
  { name: "svf/high", type: NT.SVF,     params: [[0, 1], [1, 500], [2, 0.8]] },
  { name: "svf/band", type: NT.SVF,     params: [[0, 2], [1, 900], [2, 1.2]] },
  { name: "svf/notch", type: NT.SVF,    params: [[0, 3], [1, 700], [2, 0.7]] },
  { name: "shape/tanh", type: NT.SHAPER, params: [[0, 2], [1, 6]] },
  { name: "fold",     type: NT.SHAPER,  params: [[0, 3], [1, 3]] },
  { name: "clip",     type: NT.SHAPER,  params: [[0, 1], [1, 8]] },
  { name: "shape/soft", type: NT.SHAPER, params: [[0, 0], [1, 4]] },
  { name: "shape/sinefold", type: NT.SHAPER, params: [[0, 4], [1, 2.5]] },
  { name: "dcblock",  type: NT.DCBLOCK, params: [[0, 0.995]] },
  { name: "noise",    type: NT.NOISE,   params: [[0, 0.4], [1, 0.3]] },
  { name: "chorus",   type: NT.CHORUS,  params: [[0, 1], [1, 0.5], [2, 0.4], [3, 0.015]] },
  { name: "reverb",   type: NT.REVERB,  params: [[0, 2], [1, 0.4], [2, 0.4]] },
  { name: "comp",     type: NT.COMP,    params: [[0, -20], [1, 4], [2, 0.005], [3, 0.1]] },
  { name: "osc/saw",  type: NT.OSC,     params: [[0, 220], [1, 1], [2, 0.3]] },
  // F2 iteration: the remaining waveforms, exercising the emitter's per-sample
  // sinf (sine) and fmodf (square/triangle) bridge-import paths.
  { name: "osc/sine", type: NT.OSC,     params: [[0, 220], [1, 0], [2, 0.3]] },
  { name: "osc/square", type: NT.OSC,   params: [[0, 220], [1, 2], [2, 0.3]] },
  { name: "osc/tri",  type: NT.OSC,     params: [[0, 220], [1, 3], [2, 0.3]] },
];

// Build the LKB0 blob for one spec: osc(saw 220, 0.3) -> NODE, or NODE alone for
// a source (osc/noise). Reuses the demo's real encoder (encodeLKB0) so the
// encoder itself is under test.
function buildBlob(spec) {
  const src = { type: NT.OSC, params: [[0, 220], [1, 1], [2, 0.3]] };
  if (audioIns(spec.type) === 0) {
    return encodeLKB0({ nodes: [{ type: spec.type, params: spec.params }], edges: [], output: 0 });
  }
  const nodes = [src, { type: spec.type, params: spec.params }];
  const edges = [{ src: 0, dst: 1, dport: 0, fb: 0 }];
  if (audioIns(spec.type) === 2) edges.push({ src: 0, dst: 1, dport: 1, fb: 0 });
  return encodeLKB0({ nodes, edges, output: 1 });
}

const K = makeInstance("dist/lk_kernel.wasm");
const A = makeInstance("dist/aot_twin.wasm");
K.exports.lk_init(SR, BLOCK);
A.exports.aot_init(SR);
const kOut = K.exports.malloc(BLOCK * 4);
const aOut = A.exports.malloc(BLOCK * 4);
const blobPtr = K.exports.malloc(4096);
const kView = () => new Float32Array(K.exports.memory.buffer, kOut, BLOCK);
const aView = () => new Float32Array(A.exports.memory.buffer, aOut, BLOCK);

const dbfs = (x) => (x <= 0 ? -Infinity : 20 * Math.log10(x));

function runSpec(spec) {
  // kernel
  const blob = buildBlob(spec);
  new Uint8Array(K.exports.memory.buffer, blobPtr, blob.length).set(blob);
  const rc = K.exports.lk_load_plan(blobPtr, blob.length);
  if (rc !== 0) throw new Error(`${spec.name}: lk_load_plan rc=${rc}`);
  K.exports.lk_swap(0);
  // reference
  A.exports.aot_chain_setup(spec.type, SR);
  for (const [pid, v] of spec.params) A.exports.aot_chain_setparam(pid, v);

  let maxDiff = 0, sumSqDiff = 0, sumSqRef = 0, count = 0;
  for (let b = 0; b < NBLOCKS; b++) {
    K.exports.lk_process(kOut, BLOCK);
    A.exports.aot_chain_process(aOut, BLOCK);
    const kv = kView(), av = aView();
    for (let i = 0; i < BLOCK; i++) {
      const d = kv[i] - av[i];
      const ad = d < 0 ? -d : d;
      if (ad > maxDiff) maxDiff = ad;
      sumSqDiff += d * d; sumSqRef += av[i] * av[i]; count++;
    }
  }
  const residual = dbfs(Math.sqrt(sumSqDiff / count));
  return { maxDiff, residualDbfs: residual, refRmsDbfs: dbfs(Math.sqrt(sumSqRef / count)) };
}

// Also sanity-check that each shipped preset parses + encodes cleanly.
import { EXAMPLES } from "./lk-dsl.mjs";
const presetResults = Object.entries(EXAMPLES).map(([name, src]) => {
  const r = parsePatch(src);
  return { name, ok: r.ok, nodes: r.graph ? r.graph.nodes.length : 0,
           bytes: r.ok ? encodeLKB0(r.graph).length : 0,
           errors: r.errors.filter((e) => e.level === "error").map((e) => e.msg) };
});

const results = [];
let allPass = true;
for (const spec of SPECS) {
  const r = runSpec(spec);
  const pass = r.maxDiff === 0 || r.residualDbfs <= -60;
  if (!pass) allPass = false;
  results.push({ node: spec.name, maxAbsDiff: r.maxDiff, residualDbfs: r.residualDbfs, pass });
}

const presetsPass = presetResults.every((p) => p.ok);
allPass = allPass && presetsPass;

// ── F2 emitter conformance ────────────────────────────────────────────────────
// Same per-node oracle, third renderer: each supported spec's osc→NODE blob is
// compiled by the F2 graph→wasm emitter (f2-emitter.js) and null-tested against
// the SAME hand-wired AOT twin — proving "what you hear while editing
// (interpreter) == what ships compiled (F2)" node by node. Types the emitter
// does not cover (Chorus/Reverb/Comp) are listed as interpreter-fallback.
import { createRequire } from "node:module";
const F2 = createRequire(import.meta.url)("./f2-emitter.js");
const LIBM = F2.libmOf(K.exports);

function runSpecF2(spec) {
  const blob = buildBlob(spec);
  const res = F2.emit(blob, SR, LIBM);
  const inst = new WebAssembly.Instance(new WebAssembly.Module(res.bytes), F2.imports(LIBM));
  const dst = inst.exports.dst.value;
  const fView = new Float32Array(inst.exports.memory.buffer, dst, BLOCK);
  A.exports.aot_chain_setup(spec.type, SR);
  for (const [pid, v] of spec.params) A.exports.aot_chain_setparam(pid, v);
  let maxDiff = 0;
  for (let b = 0; b < NBLOCKS; b++) {
    inst.exports.process(dst, BLOCK);
    A.exports.aot_chain_process(aOut, BLOCK);
    const av = aView();
    for (let i = 0; i < BLOCK; i++) {
      const d = Math.abs(fView[i] - av[i]);
      if (d > maxDiff) maxDiff = d;
    }
  }
  return { maxDiff, moduleBytes: res.stats.moduleBytes };
}

const f2Results = [];
const f2Fallback = [];
for (const spec of SPECS) {
  const chainTypes = audioIns(spec.type) === 0 ? [spec.type] : [NT.OSC, spec.type];
  if (!chainTypes.every((t) => F2.SUPPORTED.has(t))) { f2Fallback.push(spec.name); continue; }
  const r = runSpecF2(spec);
  const pass = r.maxDiff === 0;
  if (!pass) allPass = false;
  f2Results.push({ node: spec.name, maxAbsDiff: r.maxDiff, moduleBytes: r.moduleBytes, pass });
}

console.log(JSON.stringify({ sampleRate: SR, blocks: NBLOCKS, perNode: results, presets: presetResults, f2: f2Results, f2InterpreterFallback: f2Fallback, pass: allPass }, null, 2));
console.log("\n── PER-NODE CONFORMANCE (interpreter vs AOT twin) ───────");
for (const r of results)
  console.log(`  ${r.node.padEnd(16)} maxAbsDiff=${r.maxAbsDiff}  residual ${r.residualDbfs === -Infinity ? "-inf" : r.residualDbfs.toFixed(1)} dBFS  -> ${r.pass ? "PASS" : "FAIL"}`);
console.log("── PER-NODE CONFORMANCE (F2 emitted vs AOT twin) ────────");
for (const r of f2Results)
  console.log(`  ${r.node.padEnd(16)} maxAbsDiff=${r.maxAbsDiff}  (${r.moduleBytes} B module)  -> ${r.pass ? "PASS" : "FAIL"}`);
if (f2Fallback.length)
  console.log(`  interpreter-fallback (not emitted): ${f2Fallback.join(", ")}`);
console.log("── PRESETS ──────────────────────────────────────────────");
for (const p of presetResults)
  console.log(`  ${p.name.padEnd(12)} ${p.ok ? `PASS  ${p.nodes} nodes, ${p.bytes} B` : `FAIL  ${p.errors.join("; ")}`}`);
console.log(`\nOVERALL: ${allPass ? "PASS" : "FAIL"}`);
process.exit(allPass ? 0 : 1);
