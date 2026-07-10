#!/usr/bin/env node
// wclap-adapter.test.mjs — static + runtime guards for the WebCLAP adapter and
// the worklet-resident host bundle. Zero-dependency; runs in any Node.
//
//   Run:  node test/wclap-adapter.test.mjs
//
// Proves:
//   1. adapters/wclap.js exposes the COMPLETE host-adapter contract (same set
//      adapters/wam.js does) so the shell can drive it unchanged.
//   2. The worklet bundle is a CLASSIC script (no import/export — addModule only
//      runs classic scripts) and registers the "pulp-wclap" processor.
//   3. The CLAP ABI the worklet INLINES is byte-identical to the single source of
//      truth wclap-abi.mjs (the two cannot silently drift).
//   4. The new clap.state stream trampoline ("iiI->I") compiles and round-trips
//      an i64 (BigInt) — the funcref the clap_ostream.write / clap_istream.read
//      vtable needs.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import * as ABI from "../src/vendor/pulp-wasm/wclap-abi.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = join(HERE, "..", "src");
const read = (p) => readFileSync(join(SRC, p), "utf8");

let failed = 0;
const ok = (cond, msg) => { console.log(`${cond ? "  ok  " : "FAIL  "}${msg}`); if (!cond) failed++; };

// ——— 1. The WebCLAP adapter exposes the full contract.
const wclap = read("adapters/wclap.js");
ok(/createWclapAdapter/.test(wclap), "adapters/wclap.js exports createWclapAdapter");
ok(/new AudioWorkletNode\(ctx, WORKLET_NAME/.test(wclap), "adapter builds the AudioWorkletNode host");
const CONTRACT = [
  "descriptor", "audioNode", "getParameterInfo", "setParameterValue",
  "getParameterValue", "scheduleMidi", "sendSysex", "getState", "setState",
  "onMidiOut", "onParamsChanged", "createSecondary", "destroy",
];
for (const member of CONTRACT) ok(wclap.includes(member), `wclap adapter exposes \`${member}\``);
ok(/hasState/.test(wclap), "wclap adapter degrades getState/setState on missing clap.state (hasState flag)");
ok(/onParamsChangedCb\(buildValues\(\), paramInfo\)/.test(wclap),
   "onParamsChanged is called (values, params) like the WAM adapter");

// ——— 2. The worklet bundle is a classic script that registers pulp-wclap.
const worklet = read("vendor/pulp-wasm/wclap-processor.js");
ok(/registerProcessor\("pulp-wclap"/.test(worklet), "worklet registers the pulp-wclap processor");
ok(!/^\s*import\s/m.test(worklet), "worklet has no ES `import` (classic script, addModule-safe)");
ok(!/^\s*export\s/m.test(worklet), "worklet has no ES `export` (classic script)");
ok(/extends AudioWorkletProcessor/.test(worklet), "worklet defines an AudioWorkletProcessor");
ok(/CLAP_EXT_STATE/.test(worklet) && /getState\(\)/.test(worklet) && /setState\(/.test(worklet),
   "worklet implements clap.state save/load");
ok(/CLAP_EVENT_MIDI\b/.test(worklet) && /CLAP_EVENT_MIDI_SYSEX/.test(worklet),
   "worklet builds CLAP midi + midi_sysex IN events");

// ——— 3. ABI parity: every single-line `export const` in wclap-abi.mjs appears
//        verbatim (sans `export `) in the worklet's inlined block.
const abiSrc = readFileSync(join(SRC, "vendor/pulp-wasm/wclap-abi.mjs"), "utf8");
const singleLine = [...abiSrc.matchAll(/^export const (\w+) = (.+);$/gm)];
ok(singleLine.length >= 20, `found ${singleLine.length} single-line ABI constants to check`);
for (const [, name, rhs] of singleLine) {
  ok(worklet.includes(`const ${name} = ${rhs};`), `worklet inlines ${name} identical to wclap-abi.mjs`);
}
// TRAMPOLINES base64 set parity (multi-line object).
const b64s = (src) => [...src.matchAll(/"([A-Za-z0-9+/=]{20,})"/g)].map((m) => m[1]);
const abiTramps = Object.values(ABI.TRAMPOLINES).sort();
const wkTramps = b64s(worklet).filter((s) => abiTramps.includes(s)).sort();
ok(abiTramps.every((t) => wkTramps.includes(t)), "worklet inlines all four host trampolines (incl. iiI->I)");

// ——— 4. The clap.state stream trampoline compiles and round-trips an i64.
const streamB64 = ABI.TRAMPOLINES["iiI->I"];
ok(!!streamB64, "wclap-abi.mjs defines the iiI->I stream trampoline");
try {
  const bytes = Buffer.from(streamB64, "base64");
  const mod = new WebAssembly.Module(bytes);
  let seen = null;
  const inst = new WebAssembly.Instance(mod, { h: { f: (ctx, buf, size) => { seen = { ctx, buf, size }; return size + 1n; } } });
  const r = inst.exports.fn(3, 5, 100n);
  ok(seen && seen.ctx === 3 && seen.buf === 5 && seen.size === 100n && r === 101n,
     "iiI->I trampoline forwards (i32,i32,i64)->i64 as BigInt");
} catch (e) {
  ok(false, "iiI->I trampoline compiles + runs: " + (e && e.message));
}

console.log(failed ? `\n${failed} check(s) FAILED` : "\nAll WebCLAP adapter checks passed.");
process.exit(failed ? 1 : 0);
