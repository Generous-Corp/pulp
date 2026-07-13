#!/usr/bin/env node
// gpu-ring-parity.test.mjs — drift + inertness guards for the GPU-audio SAB ring
// that the WebCLAP worklet INLINES. Zero-dependency; runs in any Node.
//
//   Run:  node test/gpu-ring-parity.test.mjs
//
// Proves:
//   1. The gpu-ring core block inlined into vendor/pulp-wasm/wclap-processor.js is
//      BYTE-IDENTICAL to its module source
//      (examples/web-demos/gpu-audio/js/gpu-ring.mjs). The worklet is a CLASSIC
//      script and cannot import it, so the copy is the only option — and the two
//      must never silently drift (same contract as wclap-abi ↔ wclap-processor).
//   2. Nothing on the audio-thread path can call Atomics.wait.
//   3. env.pulp_gpu_xfer really moves audio through the ring: dry in, the
//      latency-delayed wet out, a miss when the worker has not produced.
//   4. INERTNESS: with no `gpuSab` in processorOptions the GPU path does not
//      exist — every other demo on this player is byte-for-byte unaffected.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import vm from "node:vm";

import { allocate } from "../../../examples/web-demos/gpu-audio/js/gpu-ring.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, "..", "..", "..");
const WORKLET_PATH = join(HERE, "..", "src", "vendor", "pulp-wasm", "wclap-processor.js");
const RING_PATH = join(REPO, "examples", "web-demos", "gpu-audio", "js", "gpu-ring.mjs");

let failed = 0;
const ok = (cond, msg) => { console.log(`${cond ? "  ok  " : "FAIL  "}${msg}`); if (!cond) failed++; };

const worklet = readFileSync(WORKLET_PATH, "utf8");
const ringSrc = readFileSync(RING_PATH, "utf8");

// ——— 1. Byte-identical inlined core block.
const BEGIN = "/* ── BEGIN gpu-ring core";
const END = "/* ── END gpu-ring core";
const slice = (src, what) => {
  const b = src.indexOf(BEGIN);
  const e = src.indexOf(END);
  ok(b >= 0 && e > b, `${what} contains the gpu-ring core markers`);
  if (b < 0 || e < b) return null;
  return src.slice(b, src.indexOf("\n", e) + 1);
};
const fromModule = slice(ringSrc, "gpu-ring.mjs");
const fromWorklet = slice(worklet, "wclap-processor.js");
ok(fromModule != null && fromModule === fromWorklet,
   "the worklet's inlined gpu-ring core is byte-identical to gpu-ring.mjs");
ok(fromModule != null && !/\bexport\b/.test(fromModule),
   "the core block declares no ES exports (it must be legal in a classic script)");

// ——— 2. No blocking — and no locks — on the audio thread.
ok(!/Atomics\s*\.\s*wait\s*\(/.test(worklet), "the worklet bundle contains no Atomics.wait(");
// Atomics.notify does not block, but it DOES take V8's shared waiter-list lock.
// The audio thread takes no locks: the worker polls on a bounded timer instead.
ok(!/Atomics\s*\.\s*notify\s*\(/.test(worklet), "the worklet bundle takes no waiter-list lock");

// ——— 3 + 4. Run the classic worklet bundle in a sandbox and drive the real code.
const AudioWorkletProcessorStub = class {
  constructor() { this.port = { onmessage: null, postMessage() {} }; }
};
const sandbox = {
  AudioWorkletProcessor: AudioWorkletProcessorStub,
  registerProcessor: () => {},
  sampleRate: 48000,
  currentTime: 0,
  console,
  SharedArrayBuffer, Atomics, WebAssembly,
};
vm.createContext(sandbox);
// Top-level class/const declarations live in the script's lexical scope, not on
// the context object — the trailing expression hands them back.
const X = vm.runInContext(
  worklet + "\n;({ WclapProcessor, WorkletWclapHost, gpuRingAttach });",
  sandbox, { filename: "wclap-processor.js" });
ok(!!(X && X.WclapProcessor && X.WorkletWclapHost && X.gpuRingAttach),
   "the worklet bundle evaluates as a classic script and defines the GPU seam");

// INERTNESS — no gpuSab.
const inert = new X.WclapProcessor({ processorOptions: {} });
ok(inert.gpuRing === null, "no gpuSab → no ring is attached");
ok(inert.gpuSab === null && inert.gpuLatencyBlocks === 0, "no gpuSab → the GPU options stay at their off defaults");
const out = [new Float32Array(128), new Float32Array(128)];
ok(inert.process([[]], [out]) === true, "an un-loaded processor still renders (returns true)");
ok(out[0].every((v) => v === 0), "…and outputs silence, exactly as before the GPU lane existed");
const inertHost = new X.WorkletWclapHost(null, {}, null);
ok(inertHost._gpuXfer(0, 0, 512, 2) === 0, "pulp_gpu_xfer with no ring is a miss (0) — never a throw");

// THE REAL TRANSFER PATH — a ring, the worklet's own _gpuXfer, and a mock worker.
const BLOCK = 512, CH = 2, L = 2;
const ring = allocate({ blockSize: BLOCK, channels: CH, slots: 4, latencyBlocks: L }).prime();
const host = new X.WorkletWclapHost(null, {}, X.gpuRingAttach(ring.sab));
const heap = host._f32;
const IN_PTR = 0x10000, OUT_PTR = 0x40000;      // two disjoint scratch areas in the wasm heap
const N = BLOCK * CH;

ok(host._gpuXfer(IN_PTR, OUT_PTR, 128, CH) === 0, "pulp_gpu_xfer refuses a block that is not blockSize frames");
// The channel count is the dimension that can OVERRUN the plugin's staging buffer
// (the ring writes channels * blockSize floats back into the wasm heap), so it is
// carried in the ABI and checked — not assumed.
ok(host._gpuXfer(IN_PTR, OUT_PTR, BLOCK, 4) === 0, "pulp_gpu_xfer refuses a block whose channel count is not the ring's");

const dry = (n) => { for (let i = 0; i < N; i++) heap[(IN_PTR >> 2) + i] = n * 1000 + i; };
const wet = (v) => v * 2 + 1;
const pump = () => {
  const s = new Float32Array(N), w = new Float32Array(N);
  for (;;) {
    if (ring.outputFull()) break;
    const seq = ring.takeInput(s, 0);
    if (seq === null) break;
    for (let i = 0; i < N; i++) w[i] = wet(s[i]);
    ring.publishOutput(w, 0, seq);
  }
};

let startupMisses = 0, delivered = 0;
for (let n = 0; n < 8; n++) {
  pump();                                        // the GPU worker runs between audio blocks
  dry(n);
  const got = host._gpuXfer(IN_PTR, OUT_PTR, BLOCK, CH);
  const base = OUT_PTR >> 2;
  if (n < L) {
    // No wet can exist for a block that was never pushed. It MISSES, and the
    // plugin substitutes its own latency-aligned CPU wet — no primed silence, so no
    // 21 ms dropout at the start of the stream.
    ok(got === 0, `block ${n}: miss (nothing was pushed ${L} blocks ago)`);
    startupMisses++;
  } else {
    ok(got === 1, `block ${n}: pulp_gpu_xfer delivered a block`);
    let match = true;
    for (let i = 0; i < N; i++) if (heap[base + i] !== wet((n - L) * 1000 + i)) match = false;
    ok(match, `block ${n}: the wet of dry block ${n - L} — latency is exactly ${L * BLOCK} samples`);
    delivered++;
  }
}
ok(startupMisses === L && delivered === 6, "the transfer ran the full start-up + steady-state sequence");

// A stalled worker MISSES; the audio thread does not block and the plugin's CPU
// path covers the block. (A miss is normal, not exceptional.)
for (let n = 8; n < 8 + L; n++) { dry(n); host._gpuXfer(IN_PTR, OUT_PTR, BLOCK, CH); }  // drain what is queued
dry(99);
const missed = host._gpuXfer(IN_PTR, OUT_PTR, BLOCK, CH);
ok(missed === 0, "a starved output ring returns 0 (miss) instead of blocking");
ok(ring.workletCounters().miss >= 1, "the miss is counted for the page's stats readout");

console.log(failed ? `\n${failed} check(s) FAILED` : "\nAll gpu-ring parity + inertness checks passed.");
process.exit(failed ? 1 : 0);
