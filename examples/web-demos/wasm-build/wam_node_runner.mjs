// Deterministic non-browser WAM runner (Phase 1A, R6).
//
// Instantiates a Pulp WAM `.wasm` directly with stubbed WASI/env imports — no
// browser, no AudioWorklet, no Emscripten JS glue — and drives the wam_*
// exports to prove the DSP actually runs in WebAssembly:
//   - non-silent, finite (no NaN/Inf) output for a known input
//   - unity passthrough at default 0 dB
//   - dB-accurate gain parameter response
//   - stereo channel layout (interleaved L/R preserved, not summed/swapped)
//   - bypass is an exact passthrough
//   - malformed parameter ids do not abort the module (from_chars guard)
//   - state size/read/write round-trip
//
// Usage: node wam_node_runner.mjs <PulpGain.wasm>
// Exit 0 on all-pass, 1 on any failure, 2 on bad usage.
//
// PulpGain parameter ids (see examples/pulp-gain/pulp_gain.hpp):
//   "1" = Input Gain (dB),  "2" = Output Gain (dB),  "3" = Bypass (boolean)

import { readFileSync } from "node:fs";
import { makeWasmImports, makeBridge } from "../../../core/format/src/wasm/wam-runtime.mjs";

const wasmPath = process.argv[2];
if (!wasmPath) {
  console.error("usage: node wam_node_runner.mjs <PulpGain.wasm>");
  process.exit(2);
}

let instance;
const mod = new WebAssembly.Module(readFileSync(wasmPath));
instance = new WebAssembly.Instance(mod, makeWasmImports(() => instance.exports.memory));
const wam = makeBridge(instance.exports);
const ex = instance.exports;
wam.callCtors();

const setParam = (id, v) => wam.setParam(id, v);
const getParam = (id) => wam.getParam(id);

const SR = 48000, FR = 128, CH = 2, N = CH * FR;
if (!wam.init(SR, FR)) throw new Error("wam_init returned 0");

const inPtr = ex.malloc(N * 4), outPtr = ex.malloc(N * 4);
const fillInput = (fn) => {
  const h = wam.f32();
  for (let f = 0; f < FR; f++) for (let c = 0; c < CH; c++) h[(inPtr >> 2) + f * CH + c] = fn(f, c);
};
const readOutput = () => {
  const h = wam.f32(), o = new Array(N);
  for (let i = 0; i < N; i++) o[i] = h[(outPtr >> 2) + i];
  return o;
};
const rms = (a) => Math.sqrt(a.reduce((s, x) => s + x * x, 0) / a.length);
const proc = () => { wam.process(inPtr, outPtr, CH, FR); return readOutput(); };

const failures = [];
const check = (name, cond, detail = "") => {
  if (cond) console.log("  ok   " + name);
  else { failures.push(name + (detail ? " — " + detail : "")); console.log("  FAIL " + name + (detail ? " — " + detail : "")); }
};

// 1. Unity passthrough at defaults, with distinct L/R DC.
fillInput((f, c) => (c === 0 ? 0.5 : -0.5));
let out = proc();
check("output finite", out.every(Number.isFinite));
check("output non-silent", rms(out) > 1e-6, "rms=" + rms(out));
check("unity passthrough rms ~0.5", Math.abs(rms(out) - 0.5) < 0.01, "rms=" + rms(out));

// 2. Stereo channel layout: L stays L (0.5), R stays R (-0.5).
check("stereo L preserved", Math.abs(out[0] - 0.5) < 0.01, "L0=" + out[0]);
check("stereo R preserved", Math.abs(out[1] + 0.5) < 0.01, "R0=" + out[1]);

// 3. Output gain +6 dB ~ 2x.
setParam("2", 6.0);
check("+6dB output ~2x", Math.abs(rms(proc()) - 1.0) < 0.02, "rms=" + rms(proc()));
setParam("2", 0.0);

// 4. Input gain -6 dB ~ 0.5x (numeric dB parity).
setParam("1", -6.0);
check("-6dB input ~0.5x", Math.abs(rms(proc()) - 0.25) < 0.01, "rms=" + rms(proc()));
setParam("1", 0.0);

// 5. Bypass is an exact passthrough.
setParam("3", 1.0);
out = proc();
check("bypass exact passthrough", Math.abs(out[0] - 0.5) < 1e-6 && Math.abs(out[1] + 0.5) < 1e-6, "out0=" + out[0] + " out1=" + out[1]);
setParam("3", 0.0);

// 6. Parameter read-back.
setParam("1", 3.5);
check("param round-trip", Math.abs(getParam("1") - 3.5) < 1e-4, "got " + getParam("1"));

// 7. State size/read/write round-trip (via the shared bridge helpers).
setParam("1", 7.0);
const saved = wam.readState();
check("state size > 0", saved.length > 0, "size=" + saved.length);
setParam("1", -12.0);
check("state restore ok", wam.writeState(saved));
check("state restored value", Math.abs(getParam("1") - 7.0) < 1e-3, "got " + getParam("1"));

// 8. Malformed id must not abort the module (from_chars guard).
setParam("not_a_number", 1.0);
getParam("");
check("malformed id no-crash", true);

console.log(failures.length ? `\nFAILURES (${failures.length}): ${failures.join("; ")}` : "\nALL CHECKS PASSED");
process.exit(failures.length ? 1 : 0);
