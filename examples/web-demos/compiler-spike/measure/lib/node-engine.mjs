// node-engine.mjs — the Node execution surface, mirroring page/measure.html.
//
// The harnesses run in headless system Chrome by default (--engine chrome), the
// deployment-representative path. This is the deterministic `--engine node`
// fallback: identical measure-core math, wasm loaded from the filesystem, no
// browser required. Handy in CI and for a quick local number. The two surfaces
// intentionally share measure-core.mjs so they cannot diverge.

import { readFileSync } from "node:fs";
import * as core from "./measure-core.mjs";
import { loadWamDsp } from "./dsp-adapter.mjs";

function loadFromFile(path, opts) {
  return loadWamDsp(new Uint8Array(readFileSync(path)), { name: path.split("/").pop(), ...opts });
}

function configure(adapter, cfg = {}) {
  if (cfg.params) for (const [id, v] of Object.entries(cfg.params)) adapter.setParam(id, v);
  if (cfg.midi && adapter.midi) for (const [s, d1, d2, o] of cfg.midi) adapter.midi(s, d1, d2, o || 0);
  if (cfg.settleQuanta) for (let i = 0; i < cfg.settleQuanta; i++) adapter.processQuantum();
}

export function cpu(path, opts = {}) {
  const a = loadFromFile(path, opts);
  const input = core.genSine(opts.freqHz || 220, a.sampleRate, a.frames, opts.amp || 0.25, a.channels);
  configure(a, opts);
  const r = core.measureCpu(a, { ...opts, input });
  a.dispose?.();
  return { engine: "node", ...r };
}

export function nullTest(cfg = {}) {
  const {
    pathA, pathB = cfg.pathA, sampleRate = 48000, frames = 128, channels = 2,
    freqHz = 220, seconds = 1.0, amp = 0.25, maxShift = 0, paramsA, paramsB, midiA, midiB,
    settleQuanta = 0,
  } = cfg;
  const opts = { sampleRate, frames, channels };
  const a = loadFromFile(pathA, opts);
  const b = loadFromFile(pathB, opts);
  const total = Math.floor(seconds * sampleRate);
  const input = core.genSine(freqHz, sampleRate, total, amp, channels);
  configure(a, { params: paramsA, midi: midiA, settleQuanta });
  configure(b, { params: paramsB, midi: midiB, settleQuanta });
  const outA = core.renderBuffer(a, input);
  const outB = core.renderBuffer(b, input);
  const r = core.nullTest(outA, outB, { channels, maxShift });
  a.dispose?.(); b.dispose?.();
  return { engine: "node", frames: outA.length / channels, ...r };
}
