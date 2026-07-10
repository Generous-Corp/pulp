#!/usr/bin/env node
// adapter-seam.test.mjs — proves @danielraffel/web-player's shell is host-agnostic and
// the WAM adapter is the sole backend seam. Static (fast, zero-dependency, runs
// in any Node). A runtime pack-install smoke lives in scripts/pack-smoke.mjs.
//
//   Run:  node test/adapter-seam.test.mjs   (or: npm test)

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = join(HERE, "..", "src");
const read = (p) => readFileSync(join(SRC, p), "utf8");

let failed = 0;
const ok = (cond, msg) => { console.log(`${cond ? "  ok  " : "FAIL  "}${msg}`); if (!cond) failed++; };

// ——— 1. The shell imports and names NO plugin backend.
const shell = read("shell.js");
ok(!/wam-plugin/.test(shell), "shell.js does not import wam-plugin.js");
ok(!/\bPulpWAM\b/.test(shell), "shell.js contains no PulpWAM reference");
ok(!/\bcreateWamAdapter\b/.test(shell), "shell.js defines/uses no createWamAdapter");
ok(/const makeAdapter = opts\.createAdapter;/.test(shell),
   "shell requires opts.createAdapter (no hardwired backend default)");
ok(/throw new Error\(\s*\n?\s*"mountDemo: opts\.createAdapter is required/.test(shell)
   || /opts\.createAdapter is required/.test(shell),
   "shell throws when no adapter factory is supplied");
ok(/S\.wam\s*=\s*await\s+makeAdapter\(/.test(shell),
   "main instance is created via the injected adapter seam");
ok(/await\s+S\.wam\.createSecondary\(/.test(shell),
   "voice pool is created via adapter.createSecondary");
// Skinnability: the shell reads token/font/theme options rather than hardcoding.
ok(/injectStyles\(\{ tokensHref: opts\.tokensHref, fontHref: opts\.fontHref, theme: opts\.theme \}\)/.test(shell),
   "shell forwards skin options (tokensHref/fontHref/theme) to injectStyles");
ok(!/setAttribute\("data-theme", "dark"\)/.test(shell),
   "shell no longer force-sets dark theme");
// Hex-leak fixes.
ok(/background:var\(--key-white\)/.test(shell) && /background:var\(--key-black\)/.test(shell),
   "keyboard key fills use --key-white / --key-black tokens (no hardcoded hex)");
ok(!/\|\|\s*"#16dac2"/.test(shell), "scope fallback no longer hardcodes #16dac2");

// ——— 2. The WAM adapter is the sole backend, exposing the complete contract.
const wam = read("adapters/wam.js");
ok(/import PulpWAM from "\.\.\/vendor\/pulp-wasm\/wam-plugin\.js"/.test(wam),
   "adapters/wam.js imports the vendored wam-plugin.js backend");
ok(/PulpWAM\s*\.\s*createInstance/.test(wam), "adapters/wam.js constructs a PulpWAM instance");
const CONTRACT = [
  "descriptor", "audioNode", "getParameterInfo", "setParameterValue",
  "getParameterValue", "scheduleMidi", "sendSysex", "getState", "setState",
  "onMidiOut", "onParamsChanged", "createSecondary", "destroy",
];
for (const member of CONTRACT) {
  ok(wam.includes(member), `WAM adapter exposes \`${member}\``);
}

// ——— 3. The package entry injects the WAM adapter as the default.
const index = read("index.js");
ok(/opts\.createAdapter\s*\|\|\s*createWamAdapter/.test(index),
   "index.js defaults opts.createAdapter to createWamAdapter");
ok(/export function mountDemo/.test(index) && /export \{ createWamAdapter \}/.test(index),
   "index.js exports mountDemo + createWamAdapter");

console.log(failed ? `\n${failed} assertion(s) FAILED` : "\nadapter seam intact — all assertions passed");
process.exit(failed ? 1 : 0);
