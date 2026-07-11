// Deterministic non-browser validation for an in-worklet WAM RACK.
//
// Drives a built rack .wasm (pulp-pluck → pulp-gain) through the shared bridge
// and asserts the behaviours only the chain path exercises: the descriptor
// exposes stage-qualified parameters, a "<stage>:<id>" write reaches the right
// stage, a MIDI note-on routed to stage 0 produces sound the later stage passes
// through, and the multi-stage "PWR1" state container round-trips.
//
// Usage: node wam_rack_runner.mjs <rack.wasm>

import { readFileSync } from "node:fs";
import { makeWasmImports, makeBridge, makeWamAudioPorts } from "../../../core/format/src/wasm/wam-runtime.mjs";

const wasmPath = process.argv[2];
if (!wasmPath) { console.error("usage: node wam_rack_runner.mjs <rack.wasm>"); process.exit(2); }

let instance;
instance = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(wasmPath)),
                                    makeWasmImports(() => instance.exports.memory));
const wam = makeBridge(instance.exports);
wam.callCtors();

const SR = 48000, FR = 128, CH = 2, N = CH * FR;
if (!wam.init(SR, FR)) throw new Error("wam_init failed");

const ex = instance.exports;
// Planar audio ports (per-channel wasm buffers + wam_process pointer arrays).
const ports = makeWamAudioPorts(wam, CH, FR);
const silenceIn = () => { for (let f = 0; f < FR; f++) { ports.setInputSample(0, f, 0); ports.setInputSample(1, f, 0); } };
const proc = () => {
  wam.process(ports.inPtr, ports.outPtr, CH, FR);
  let peak = 0, finite = true;
  for (let c = 0; c < CH; c++) for (let f = 0; f < FR; f++) {
    const v = ports.outputSample(c, f);
    if (!Number.isFinite(v)) finite = false; peak = Math.max(peak, Math.abs(v));
  }
  return { peak, finite };
};

const fails = [];
const check = (name, cond, detail = "") => {
  console.log((cond ? "  ok   " : "  FAIL ") + name + (detail ? " — " + detail : ""));
  if (!cond) fails.push(name);
};

// ── Descriptor + stage-qualified parameters ────────────────────────────────
const desc = JSON.parse(wam.descriptorJson());
check("rack descriptor parsed", !!desc.name);
check("descriptor names both stages", /pluck/i.test(desc.name) && /gain/i.test(desc.name), desc.name);

const params = JSON.parse(wam.parametersJson());
check("rack exposes parameters", params.length > 0, `${params.length} params`);
check("every id is stage-qualified '<stage>:<id>'", params.every((p) => /^\d+:\d+$/.test(p.id)),
      params.map((p) => p.id).join(","));
const stage0 = params.filter((p) => p.id.startsWith("0:"));
const stage1 = params.filter((p) => p.id.startsWith("1:"));
check("both stages contribute parameters", stage0.length > 0 && stage1.length > 0,
      `stage0=${stage0.length} stage1=${stage1.length}`);

// ── A stage-qualified write reaches exactly that stage ─────────────────────
const target = stage1[0];
const mid = (target.minValue + target.maxValue) / 2;
wam.setParam(target.id, mid);
check(`write to ${target.id} reads back`, Math.abs(wam.getParam(target.id) - mid) < 1e-3,
      `got ${wam.getParam(target.id)}`);

// ── MIDI note-on → stage 0 makes sound → stage 1 passes it through ─────────
silenceIn();
check("silent before note", proc().peak < 1e-4);
wam.midi(0x90, 60, 100, 0);        // note-on, routed to stage 0 (the instrument)
let peakOn = 0, allFinite = true;
for (let b = 0; b < 8; b++) { silenceIn(); const r = proc(); peakOn = Math.max(peakOn, r.peak); allFinite = allFinite && r.finite; }
check("note-on produces sound through the chain", peakOn > 0.01, `peak=${peakOn.toFixed(4)}`);
check("rack output finite", allFinite);

// ── "PWR1" multi-stage state container round-trips ─────────────────────────
const readState = () => { const sz = ex.wam_state_size(); const p = ex.malloc(sz); ex.wam_read_state(p);
  const b = new Uint8Array(instance.exports.memory.buffer, p, sz).slice(); ex.free(p); return b; };
const writeState = (b) => { const p = ex.malloc(b.length); new Uint8Array(instance.exports.memory.buffer).set(b, p);
  const ok = ex.wam_write_state(p, b.length) !== 0; ex.free(p); return ok; };

const s0 = readState();
check('rack state uses the "PWR1" container magic', String.fromCharCode(...s0.slice(0, 4)) === "PWR1",
      String.fromCharCode(...s0.slice(0, 4)));
wam.setParam(target.id, target.maxValue);
const s1 = readState();
check("rack state changed after a param edit", s1.length !== s0.length || !s1.every((v, i) => v === s0[i]));
check("writeState(s0) restores the earlier value",
      writeState(s0) && Math.abs(wam.getParam(target.id) - mid) < 1e-3, `got ${wam.getParam(target.id)}`);
check("writeState(s1) restores the edited value",
      writeState(s1) && Math.abs(wam.getParam(target.id) - target.maxValue) < 1e-3, `got ${wam.getParam(target.id)}`);
// A truncated container must be rejected without trapping the worklet.
check("truncated rack container rejected", writeState(s1.slice(0, 6)) === false);

console.log(fails.length ? `\n❌ ${fails.length} FAILED: ${fails.join(", ")}` : "\n✅ ALL RACK CHECKS PASSED");
process.exit(fails.length ? 1 : 0);
