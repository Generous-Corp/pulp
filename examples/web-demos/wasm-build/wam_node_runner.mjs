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

const wasmPath = process.argv[2];
if (!wasmPath) {
  console.error("usage: node wam_node_runner.mjs <PulpGain.wasm>");
  process.exit(2);
}

let instance;
const mem = () => instance.exports.memory;
const u8 = () => new Uint8Array(mem().buffer);
const f32 = () => new Float32Array(mem().buffer);
const dv = () => new DataView(mem().buffer);

const imports = {
  env: {
    _abort_js: () => { throw new Error("wasm called abort_js"); },
    _tzset_js: () => {},
    // Our buffers are tiny and the initial heap is 16 MB; no growth expected.
    emscripten_resize_heap: () => false,
  },
  wasi_snapshot_preview1: {
    environ_get: () => 0,
    environ_sizes_get: (cntPtr, sizePtr) => {
      dv().setUint32(cntPtr, 0, true);
      dv().setUint32(sizePtr, 0, true);
      return 0;
    },
    fd_close: () => 0,
    fd_seek: () => 0,
    // Discard writes (runtime::log_info) but report them fully consumed.
    fd_write: (fd, iovPtr, iovCnt, nwrittenPtr) => {
      let total = 0;
      for (let i = 0; i < iovCnt; i++) total += dv().getUint32(iovPtr + i * 8 + 4, true);
      dv().setUint32(nwrittenPtr, total, true);
      return 0;
    },
  },
};

instance = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(wasmPath)), imports);
const ex = instance.exports;
// --no-entry build: run static constructors (constructs the global WamBridge).
if (ex.__wasm_call_ctors) ex.__wasm_call_ctors();

const writeCStr = (s) => {
  const bytes = new TextEncoder().encode(s + "\0");
  const p = ex.malloc(bytes.length);
  u8().set(bytes, p);
  return p;
};
const setParam = (id, v) => { const p = writeCStr(id); ex.wam_set_param(p, v); ex.free(p); };
const getParam = (id) => { const p = writeCStr(id); const v = ex.wam_get_param(p); ex.free(p); return v; };

const SR = 48000, FR = 128, CH = 2, N = CH * FR;
if (!ex.wam_init(SR, FR)) throw new Error("wam_init returned 0");

const inPtr = ex.malloc(N * 4), outPtr = ex.malloc(N * 4);
const fillInput = (fn) => {
  const h = f32();
  for (let f = 0; f < FR; f++) for (let c = 0; c < CH; c++) h[(inPtr >> 2) + f * CH + c] = fn(f, c);
};
const readOutput = () => {
  const h = f32(), o = new Array(N);
  for (let i = 0; i < N; i++) o[i] = h[(outPtr >> 2) + i];
  return o;
};
const rms = (a) => Math.sqrt(a.reduce((s, x) => s + x * x, 0) / a.length);
const proc = () => { ex.wam_process(inPtr, outPtr, CH, FR); return readOutput(); };

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

// 7. State size/read/write round-trip.
setParam("1", 7.0);
const sz = ex.wam_state_size();
check("state size > 0", sz > 0, "size=" + sz);
const sp = ex.malloc(sz);
ex.wam_read_state(sp);
const saved = u8().slice(sp, sp + sz);
setParam("1", -12.0);
const wp = ex.malloc(sz);
u8().set(saved, wp);
check("state restore ok", ex.wam_write_state(wp, sz) === 1);
check("state restored value", Math.abs(getParam("1") - 7.0) < 1e-3, "got " + getParam("1"));

// 8. Malformed id must not abort the module (from_chars guard).
setParam("not_a_number", 1.0);
getParam("");
check("malformed id no-crash", true);

console.log(failures.length ? `\nFAILURES (${failures.length}): ${failures.join("; ")}` : "\nALL CHECKS PASSED");
process.exit(failures.length ? 1 : 0);
