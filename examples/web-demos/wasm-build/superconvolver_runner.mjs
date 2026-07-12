// Deterministic non-browser validation for SuperConvolver — through BOTH web
// ABIs from ONE set of assertions.
//
// SuperConvolver is the first web demo whose desktop build carries a GPU engine,
// a file-backed impulse-response loader, and a native GPU editor. The web build
// compiles all three out (PULP_WASM / PULP_HEADLESS) and runs the CPU
// PartitionedConvolver against the BUILT-IN synthetic IR — which is the desktop
// default engine too. What must hold afterwards: it makes wet sound with zero
// file I/O, Size and Mix audibly change the output, the reported latency is the
// fixed kInternalBlock (512) the host's PDC needs, and its state round-trips.
//
// The SAME checks run against the WAM module and the WebCLAP module, so a
// behavioural divergence between the two ABIs fails CI instead of surfacing as a
// "the WebCLAP demo sounds different" bug report. The ABI is detected from the
// module's exports (clap_entry ⇒ WebCLAP), so there is no flag to keep in sync.
//
// Usage: node superconvolver_runner.mjs <SuperConvolver.wasm>

import { readFileSync } from "node:fs";
import { makeWasmImports, makeBridge, makeWamAudioPorts } from "../../../core/format/src/wasm/wam-runtime.mjs";
import { WebClapHost } from "../../../core/format/src/wasm/wclap-host.mjs";

const wasmPath = process.argv[2];
if (!wasmPath) { console.error("usage: node superconvolver_runner.mjs <SuperConvolver.wasm>"); process.exit(2); }

const SR = 48000, FR = 128, CH = 2;
// The processor re-blocks the host stream into fixed 512-sample chunks and
// reports that as its latency (plus the GPU transport's, which is 0 with no GPU
// device — i.e. always, in a browser).
const INTERNAL_BLOCK = 512;

const bytes = readFileSync(wasmPath);
const isWclap = WebAssembly.Module.exports(new WebAssembly.Module(bytes))
  .some((e) => e.name === "clap_entry");

// ── One engine interface over the two ABIs. Everything below this point is
//    ABI-agnostic, which is what makes the parity claim mean something.
async function openWam() {
  let instance;
  instance = new WebAssembly.Instance(new WebAssembly.Module(bytes),
                                      makeWasmImports(() => instance.exports.memory));
  const wam = makeBridge(instance.exports);
  wam.callCtors();
  if (!wam.init(SR, FR)) throw new Error("wam_init failed");
  wam.prepare(SR, FR);
  const ex = instance.exports;
  const ports = makeWamAudioPorts(wam, CH, FR);
  const out = [new Float32Array(FR), new Float32Array(FR)];
  return {
    abi: "WAM",
    name: JSON.parse(wam.descriptorJson()).name,
    params: JSON.parse(wam.parametersJson()).map((p) => ({ id: p.id, label: p.label, min: p.minValue, max: p.maxValue })),
    setParam: (id, v) => wam.setParam(id, v),
    getParam: (id) => wam.getParam(id),
    latency: () => wam.latencySamples(),
    reprepare: (blockSize) => wam.prepare(SR, blockSize),
    process(inL, inR) {
      for (let f = 0; f < FR; f++) { ports.setInputSample(0, f, inL[f]); ports.setInputSample(1, f, inR[f]); }
      ex.wam_process(ports.inPtr, ports.outPtr, CH, FR);
      for (let c = 0; c < CH; c++) for (let f = 0; f < FR; f++) out[c][f] = ports.outputSample(c, f);
      return out;
    },
    readState: () => wam.readState(),
    writeState: (b) => wam.writeState(b),
  };
}

async function openWclap() {
  const host = new WebClapHost();
  await host.instantiate(bytes);
  const plugin = host.createPlugin(0);
  plugin.init();
  plugin.activate(SR, 1, FR);
  // CLAP takes parameter changes as events on the next process() block; queue
  // them the way a host does rather than reaching into the plugin.
  let pending = [];
  const values = new Map();
  return {
    abi: "WebCLAP",
    name: plugin.descriptor.name,
    params: plugin.params().map((p) => ({ id: p.id, label: p.name, min: p.min, max: p.max })),
    setParam: (id, v) => { pending.push({ id, value: v }); values.set(id, v); },
    getParam: (id) => values.get(id),
    latency: () => plugin.currentLatency(),
    reprepare: () => {},   // re-activation is a host-lifecycle op, not a render one
    process(inL, inR) {
      const o = plugin.process([inL, inR], FR, { paramEvents: pending });
      pending = [];
      return o;
    },
    readState: () => null,   // no state helper on WebClapPlugin yet (see caveats)
    writeState: () => false,
  };
}

const eng = isWclap ? await openWclap() : await openWam();

const fails = [];
const check = (name, cond, detail = "") => {
  console.log((cond ? "  ok   " : "  FAIL ") + name + (detail ? " — " + detail : ""));
  if (!cond) fails.push(name);
};

// Deterministic pseudo-noise (the LCG shape the C++ IR builder uses), so every
// energy number below is reproducible run to run and identical across the ABIs.
const SEED = 0x2545f491;
let lcg = SEED;
const noise = () => { lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0; return (lcg >>> 8) / 8388608 - 1; };

const byLabel = Object.fromEntries(eng.params.map((p) => [p.label, p]));
const idOf = (label) => {
  const p = byLabel[label];
  if (!p) throw new Error(`SuperConvolver exposes no "${label}" parameter (got: ${eng.params.map((q) => q.label).join(", ")})`);
  return p.id;
};

const inL = new Float32Array(FR), inR = new Float32Array(FR);

// One pass: `driveFrames` of noise, then `gapFrames` of silence that are NOT
// measured, then `tailFrames` of silence that are. The unmeasured gap is what
// makes `tailRms` mean "reverb tail" and nothing else: the dry path is delayed
// by the reported latency, so without a gap the delayed dry burst lands in the
// tail window and swamps it (that is the whole reason a gap exists here).
function run(driveFrames, gapFrames, tailFrames) {
  let driveSq = 0, driveN = 0, tailSq = 0, tailN = 0, peak = 0, finite = true;
  const pass = (frames, feed, measure) => {
    for (let done = 0; done < frames; done += FR) {
      for (let f = 0; f < FR; f++) { const v = feed ? noise() : 0; inL[f] = v; inR[f] = v; }
      const out = eng.process(inL, inR);
      for (let c = 0; c < CH; c++) for (let f = 0; f < FR; f++) {
        const v = out[c][f];
        if (!Number.isFinite(v)) finite = false;
        peak = Math.max(peak, Math.abs(v));
        if (!measure) continue;
        if (feed) { driveSq += v * v; driveN++; } else { tailSq += v * v; tailN++; }
      }
    }
  };
  pass(driveFrames, true, true);
  pass(gapFrames, false, false);
  pass(tailFrames, false, true);
  return {
    driveRms: driveN ? Math.sqrt(driveSq / driveN) : 0,
    tailRms: tailN ? Math.sqrt(tailSq / tailN) : 0,
    peak, finite,
  };
}

// Settle a parameter change: the IR is rebuilt off the audio path, so give the
// processor idle blocks to pick the new IR up before measuring.
const settle = (blocks = 64) => {
  inL.fill(0); inR.fill(0);
  for (let b = 0; b < blocks; b++) eng.process(inL, inR);
};

console.log(`[${eng.name}] abi=${eng.abi} params=${eng.params.length}`);
check("descriptor is SuperConvolver", eng.name === "SuperConvolver", eng.name);
check("Mix / Size / Gain parameters exposed",
      !!byLabel.Mix && !!byLabel.Size && !!byLabel.Gain,
      eng.params.map((p) => p.label).join(", "));

const MIX = idOf("Mix"), SIZE = idOf("Size");

// ── (a) the built-in synthetic IR convolves — wet sound, no file I/O. ────────
// Fully wet (Mix=100) so the output IS the convolution: a noise burst followed
// by silence must leave a decaying reverb tail behind it.
eng.setParam(MIX, 100);
eng.setParam(SIZE, 2.0);
settle();
lcg = SEED;
const wet = run(4096, 2048, 8192);
check("wet output non-silent with the built-in IR (no file loaded)",
      wet.driveRms > 1e-3, "drive rms=" + wet.driveRms.toFixed(5));
check("built-in IR leaves a reverb tail after the input stops",
      wet.tailRms > 1e-4, "tail rms=" + wet.tailRms.toFixed(6));
check("wet output finite", wet.finite);
check("wet output does not clip (peak-response-normalized IR)",
      wet.peak <= 1.5, "peak=" + wet.peak.toFixed(3));

// ── (b) Size and Mix audibly change the output. ──────────────────────────────
// Size is the IR length: a 0.05 s IR (2400 samples) is long dead by the LATE
// window below, while a 4 s one is still ringing loudly in it. Both IRs are
// peak-response-normalized, so the early energy is comparable — only the late
// window separates them, which is exactly what the knob does to the ear.
const LATE_GAP = 8192, LATE_WIN = 16000;
eng.setParam(SIZE, 0.05);
settle();
lcg = SEED;
const shortIr = run(2048, LATE_GAP, LATE_WIN);
eng.setParam(SIZE, 4.0);
settle();
lcg = SEED;
const longIr = run(2048, LATE_GAP, LATE_WIN);
check("Size changes the output (a long IR rings longer than a short one)",
      longIr.tailRms > shortIr.tailRms * 2,
      `short=${shortIr.tailRms.toFixed(6)} long=${longIr.tailRms.toFixed(6)}`);

// Mix is the dry/wet balance. At Mix=0 the output is the (delay-compensated) dry
// input only — a burst with NO tail — so the tail is pure wet, the cleanest
// probe that Mix is actually mixing.
eng.setParam(SIZE, 2.0);
eng.setParam(MIX, 0);
settle();
lcg = SEED;
const dry = run(2048, 4096, 8192);
eng.setParam(MIX, 100);
settle();
lcg = SEED;
const fullWet = run(2048, 4096, 8192);
check("Mix changes the output (wet tail present at 100%, absent at 0%)",
      fullWet.tailRms > 10 * Math.max(dry.tailRms, 1e-7),
      `mix0 tail=${dry.tailRms.toExponential(2)} mix100 tail=${fullWet.tailRms.toExponential(2)}`);
check("Mix=0 still passes the dry signal", dry.driveRms > 1e-3,
      "dry rms=" + dry.driveRms.toFixed(5));

// ── (c) reported latency: fixed kInternalBlock, stable across everything. ────
// A browser build has no GPU device, so gpu_extra_ is 0 and the total is exactly
// the re-block FIFO. It must not move when a parameter changes or on re-prepare
// — a latency that moves mid-session desyncs the host's PDC.
const lat = eng.latency();
check("latency == kInternalBlock (512) + gpu_extra_ (0 in the browser)",
      lat === INTERNAL_BLOCK, "got " + lat);
eng.setParam(SIZE, 0.5);
eng.setParam(MIX, 50);
settle(8);
check("latency stable across parameter changes", eng.latency() === lat, "got " + eng.latency());
eng.reprepare(256);
check("latency stable across re-prepare (block-size change)", eng.latency() === lat, "got " + eng.latency());
eng.reprepare(FR);

// ── (d) state round-trip: parameters + the plugin's own IR-source blob. ──────
// WAM only: WebClapPlugin has no clap.state helper yet, so the WebCLAP lane
// proves audio + parameters + latency and the WAM lane proves state.
eng.setParam(MIX, 100);
eng.setParam(SIZE, 2.0);
settle();
const saved = eng.readState();
if (saved) {
  const magic = String.fromCharCode(...saved.slice(0, 4));
  check("state uses the plugin_state_io envelope with a plugin blob (PLST)",
        magic === "PLST", magic);
  // SuperConvolver's own blob is the versioned "SCv<n>" IR-source record. With
  // no file loaded it carries the empty path — i.e. "use the built-in IR".
  const ascii = String.fromCharCode(...saved);
  check("state carries SuperConvolver's versioned IR-source blob (SCv…)",
        /SCv\d/.test(ascii), ascii.slice(0, 32).replace(/[^\x20-\x7e]/g, "."));

  eng.setParam(MIX, 0);
  eng.setParam(SIZE, 0.05);
  settle();
  check("state restore", eng.writeState(saved));
  check("restored Mix", Math.abs(eng.getParam(MIX) - 100) < 1e-3, "got " + eng.getParam(MIX));
  check("restored Size", Math.abs(eng.getParam(SIZE) - 2.0) < 1e-3, "got " + eng.getParam(SIZE));
  settle();
  // The restored plugin must be the same reverb it was: same built-in IR, same
  // Size ⇒ the same deterministic input must produce the same tail energy as the
  // (a) pass, which used the identical Mix/Size and input.
  lcg = SEED;
  const restored = run(4096, 2048, 8192);
  const rel = Math.abs(restored.tailRms - wet.tailRms) / Math.max(wet.tailRms, 1e-9);
  check("restored state reproduces the built-in-IR reverb (same tail energy)",
        rel < 0.05,
        `before=${wet.tailRms.toFixed(6)} after=${restored.tailRms.toFixed(6)} rel=${rel.toFixed(4)}`);
} else {
  console.log("  skip state round-trip (no state helper on this ABI's host)");
}

console.log(fails.length ? `\nFAILURES (${fails.length}): ${fails.join(", ")}` : "\nALL CHECKS PASSED");
process.exit(fails.length ? 1 : 0);
