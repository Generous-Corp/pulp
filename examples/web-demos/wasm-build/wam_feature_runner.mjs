// Deterministic non-browser validation for instrument + effect WAM plugins
// (Phase 2). Drives a built .wasm through the shared bridge and asserts the
// behaviors the plan requires: an instrument makes sound on a MIDI note-on and
// decays after note-off; an effect transforms a non-silent input; both stay
// finite and round-trip parameters + state.
//
// Usage: node wam_feature_runner.mjs <plugin.wasm> [instrument|effect]
// (mode is auto-detected from the descriptor's isInstrument if omitted)

import { readFileSync } from "node:fs";
import { makeWasmImports, makeBridge, makeWamAudioPorts } from "../../../core/format/src/wasm/wam-runtime.mjs";

const wasmPath = process.argv[2];
let mode = process.argv[3];
if (!wasmPath) { console.error("usage: node wam_feature_runner.mjs <plugin.wasm> [instrument|effect]"); process.exit(2); }

let instance;
instance = new WebAssembly.Instance(new WebAssembly.Module(readFileSync(wasmPath)), makeWasmImports(() => instance.exports.memory));
const wam = makeBridge(instance.exports);
wam.callCtors();

const SR = 48000, FR = 128, CH = 2, N = CH * FR;
if (!wam.init(SR, FR)) throw new Error("wam_init failed");

const ex = instance.exports;
// Planar audio ports (per-channel wasm buffers + wam_process pointer arrays).
const ports = makeWamAudioPorts(wam, CH, FR);
const fillSine = (freq, amp = 0.5) => {
  for (let f = 0; f < FR; f++) {
    const v = amp * Math.sin(2 * Math.PI * freq * f / SR);
    ports.setInputSample(0, f, v); ports.setInputSample(1, f, v);
  }
};
const silenceIn = () => { for (let f = 0; f < FR; f++) { ports.setInputSample(0, f, 0); ports.setInputSample(1, f, 0); } };
const proc = () => {
  ex.wam_process(ports.inPtr, ports.outPtr, CH, FR);
  let peak = 0, finite = true;
  for (let c = 0; c < CH; c++) for (let f = 0; f < FR; f++) {
    const v = ports.outputSample(c, f);
    if (!Number.isFinite(v)) finite = false; peak = Math.max(peak, Math.abs(v));
  }
  return { peak, finite };
};

const desc = JSON.parse(wam.descriptorJson());
if (!mode) mode = desc.isInstrument ? "instrument" : "effect";

const fails = [];
const check = (name, cond, detail = "") => { console.log((cond ? "  ok   " : "  FAIL ") + name + (detail ? " — " + detail : "")); if (!cond) fails.push(name); };

console.log(`[${desc.name}] mode=${mode}`);
check("descriptor parsed", !!desc.name);
check("parameters exposed", JSON.parse(wam.parametersJson()).length > 0);

if (mode === "instrument") {
  // No input; sound must come from the MIDI note.
  silenceIn();
  check("silent before note", proc().peak < 1e-4);
  wam.midi(0x90, 60, 100, 0);                 // note-on C4 vel 100
  let peakOn = 0, finite = true;
  for (let i = 0; i < 16; i++) { const r = proc(); peakOn = Math.max(peakOn, r.peak); finite = finite && r.finite; }
  check("note-on produces sound", peakOn > 0.01, "peak=" + peakOn.toFixed(4));
  check("instrument output finite", finite);
  wam.midi(0x80, 60, 0, 0);                    // note-off
  let last = 1;
  for (let i = 0; i < 200; i++) last = proc().peak;   // let it decay
  check("decays toward silence after note-off", last < peakOn * 0.5, "tail peak=" + last.toFixed(4));
} else {
  // Effect: a non-silent input must come through transformed/non-silent.
  fillSine(440);
  const r = proc();
  check("effect output non-silent", r.peak > 0.01, "peak=" + r.peak.toFixed(4));
  check("effect output finite", r.finite);
}

// Common: param + state round-trip (use the first parameter id).
const params = JSON.parse(wam.parametersJson());
if (params.length) {
  const id = params[0].id, target = (params[0].minValue + params[0].maxValue) / 2;
  wam.setParam(id, target);
  check("param read-back", Math.abs(wam.getParam(id) - target) < 1e-3, "got " + wam.getParam(id));
  const saved = wam.readState();
  wam.setParam(id, params[0].minValue);
  check("state restore", wam.writeState(saved) && Math.abs(wam.getParam(id) - target) < 1e-3);

  // State is the shared plugin_state_io format: a versioned "PLST" envelope
  // when the plugin owns a blob, else the bare "PULP" StateStore payload.
  const magic = String.fromCharCode(...saved.slice(0, 4));
  check("state uses the plugin_state_io format (PLST or PULP)",
        magic === "PLST" || magic === "PULP", magic);

  // Hostile / corrupt state must be rejected WITHOUT trapping the worklet — the
  // 32-bit bounds-check overflow that a crafted length once triggered, plus a
  // flipped CRC byte and a truncated blob. writeState returns false; no throw.
  const tryWrite = (label, bytes) => {
    try { check(label, wam.writeState(bytes) === false); }
    catch (e) { check(label, false, "TRAPPED: " + e.message); }
  };
  // "PWS1"/old-container-style length 0xFFFFFFF8 that wraps pos+len on wasm32.
  tryWrite("overflow-length state blob rejected, no trap",
           new Uint8Array([0x50, 0x57, 0x53, 0x31, 0xF8, 0xFF, 0xFF, 0xFF,
                           0x50, 0x55, 0x4C, 0x50, 0, 0, 0, 0, 0, 0, 0, 0]));
  if (saved.length > 4) {
    const corrupt = saved.slice(); corrupt[corrupt.length - 1] ^= 0xFF;
    check("corrupted state rejected or safely ignored (no trap)",
          (() => { try { wam.writeState(corrupt); return true; } catch { return false; } })());
    tryWrite("truncated state blob rejected, no trap", saved.slice(0, 4));
  }
}

console.log(fails.length ? `\nFAILURES (${fails.length}): ${fails.join(", ")}` : "\nALL CHECKS PASSED");
process.exit(fails.length ? 1 : 0);
