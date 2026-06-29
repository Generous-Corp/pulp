// Node host harness for a WebCLAP module: drives the full CLAP lifecycle and
// proves the plugin actually renders audio AND responds to parameter changes —
// not just that the module is contract-correct (that is wclap_probe.mjs's job).
//
// It loads the .wasm, creates the plugin, activates it, then renders two blocks:
//   1. at default parameters (passthrough for PulpGain → output ≈ input), and
//   2. with a +N dB input-gain parameter event (output RMS must rise).
// The RMS rising in (2) is the proof that parameter control flows end to end.
//
// This is the WebCLAP analogue of wam_node_runner.mjs. It needs
// `WebAssembly.Function`, so run with:
//   node --experimental-wasm-type-reflection \
//     core/format/src/wasm/wclap_host_runner.mjs <path-to.wasm> [--gain-db 6]
//
// Exit 0 = PASS, non-zero = a specific failure (printed).
import { readFileSync } from "node:fs";
import { WebClapHost } from "./wclap-host.mjs";

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}

const wasmPath = process.argv[2];
if (!wasmPath || wasmPath.startsWith("--")) {
  console.error("usage: node --experimental-wasm-type-reflection wclap_host_runner.mjs <wasm> [--gain-db 6]");
  process.exit(2);
}
const gainDb = Number(arg("--gain-db", "6"));
const FRAMES = 128, SR = 48000, INVAL = 0.25;

const rms = (a) => {
  let s = 0;
  for (let i = 0; i < a.length; i++) {
    if (!Number.isFinite(a[i])) return NaN;
    s += a[i] * a[i];
  }
  return Math.sqrt(s / a.length);
};

const host = new WebClapHost({ onLog: (fd, t) => process.stderr.write(`  [wasm fd${fd}] ${t.replace(/\n$/, "")}\n`) });
await host.instantiate(readFileSync(wasmPath));

const plugin = host.createPlugin(0);
console.log(`plugin: id="${plugin.descriptor.id}" name="${plugin.descriptor.name}" (${plugin.descriptor.count} in factory)`);
plugin.init();
plugin.activate(SR, 1, FRAMES);

const params = plugin.params();
console.log(`params (${params.length}):`);
for (const p of params) console.log(`  #${p.id} "${p.name}" [${p.min}..${p.max}] default=${p.default}`);

// A stereo input block at a known level.
const input = [new Float32Array(FRAMES).fill(INVAL), new Float32Array(FRAMES).fill(INVAL)];

// 1) default render (PulpGain default = unity → passthrough).
const out0 = plugin.process(input, FRAMES);
const rms0 = rms(out0[0]);
if (!Number.isFinite(rms0)) { console.error("FAIL: default-render output not finite"); process.exit(1); }
console.log(`render @default: in rms=${INVAL} → out rms=${rms0.toFixed(4)}`);

// 2) render with an input-gain parameter event. Pick the first param whose name
// mentions "gain" (PulpGain: "Input Gain", in dB), set it to +gainDb.
const gainParam = params.find((p) => /gain/i.test(p.name)) ?? params[0];
if (!gainParam) { console.error("FAIL: no parameter to drive"); process.exit(1); }
const target = Math.min(gainParam.max, gainDb);
const out1 = plugin.process(input, FRAMES, { paramEvents: [{ id: gainParam.id, value: target }] });
const rms1 = rms(out1[0]);
if (!Number.isFinite(rms1)) { console.error("FAIL: gain-render output not finite"); process.exit(1); }
const deltaDb = 20 * Math.log10((rms1 || 1e-9) / (rms0 || 1e-9));
console.log(`render @"${gainParam.name}"=${target}: out rms=${rms1.toFixed(4)} (Δ ${deltaDb.toFixed(2)} dB vs default)`);

plugin.destroy();

// PASS criteria: both renders finite, and the gain event raised the output level
// by a clearly-audible amount (parameter control actually took effect).
if (rms1 <= rms0 * 1.1) {
  console.error(`FAIL: parameter event did not change output (rms ${rms0.toFixed(4)} → ${rms1.toFixed(4)})`);
  process.exit(1);
}
console.log(`PASS: WebCLAP plugin rendered audio and responded to a parameter change (+${deltaDb.toFixed(1)} dB)`);
