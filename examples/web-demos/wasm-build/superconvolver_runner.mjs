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
// The SAME modules the demo page uses to load an impulse response: the SDK's
// plugin-state container, and SuperConvolver's own SCv2 IR record. Asserting the
// upload path here means asserting the page's actual bytes, not a paraphrase.
import { parseContainer, buildContainer }
  from "../../../packages/pulp-web-player/src/state/plugin-state.js";
import { buildPcmIrBlob, buildSyntheticIrBlob, readIrBlobKind }
  from "../super-convolver-ui/ir-source.js";
// The SAME helper the browser's WebCLAP adapter uses to recover a display unit
// from clap_plugin_params.value_to_text (CLAP has no unit field), so the text
// asserted here is the text the demo page renders.
import { deriveDisplayUnit } from "../../../packages/pulp-web-player/src/vendor/pulp-wasm/wclap-abi.mjs";

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

// The display string a Pulp UI builds from a parameter's value + unit: two fixed
// decimals, a space, the unit — byte-for-byte what the SuperConvolver web UI's
// format_value() renders (super_convolver_web_ui.hpp) AND what the native CLAP
// entry's params_value_to_text() returns (clap_entry.hpp). That both formulas
// agree is the whole point: the WAM lane gets there from the descriptor's `unit`,
// the WebCLAP lane by calling the plugin's value_to_text — same pixels either way.
const formatParamText = (value, unit) => value.toFixed(2) + (unit ? " " + unit : "");

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
    // The WAM ABI reports the display unit in its parameter JSON; the page's UI
    // formats the number and appends it.
    params: JSON.parse(wam.parametersJson()).map((p) => ({
      id: p.id, label: p.label, min: p.minValue, max: p.maxValue, unit: p.unit || "",
    })),
    paramText(id, value) {
      const p = this.params.find((q) => q.id === id);
      return formatParamText(value, p ? p.unit : "");
    },
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
    // CLAP carries no unit on clap_param_info: the display unit is recovered from
    // the plugin's value_to_text probes, exactly as adapters/wclap.js does in the
    // browser. paramText() then goes STRAIGHT to value_to_text — the real CLAP
    // display call a native host makes — so this lane proves both halves agree.
    params: plugin.params().map((p) => ({
      id: p.id, label: p.name, min: p.min, max: p.max, unit: deriveDisplayUnit(p.textProbes),
    })),
    paramText: (id, value) => plugin.valueToText(id, value),
    setParam: (id, v) => { pending.push({ id, value: v }); values.set(id, v); },
    // Read the value back OUT OF THE PLUGIN (clap_plugin_params.get_value), not out
    // of the host's own mirror of what it last sent. The plugin rewrites its own
    // parameters on a state load, and a mirror cannot see that — which is exactly
    // what the "restored Mix / restored Size" checks below are testing.
    getParam: (id) => {
      const v = plugin.paramValue(id);
      return v === null ? values.get(id) : v;
    },
    latency: () => plugin.currentLatency(),
    reprepare: () => {},   // re-activation is a host-lifecycle op, not a render one
    process(inL, inR) {
      const o = plugin.process([inL, inR], FR, { paramEvents: pending });
      pending = [];
      return o;
    },
    // clap.state, driven with the ostream/istream a real CLAP host hands the
    // plugin. It yields the SAME "PLST" blob wam_read_state does, which is what
    // lets the checks below speak ONE state format to both ABIs — and is how the
    // demo page loads an impulse response without a per-ABI entry point.
    readState: () => plugin.getState(),
    writeState: (b) => plugin.setState(b),
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

// Settle a parameter change: the IR is rebuilt off the audio path — and, on these
// worker-less lanes, in BOUNDED SLICES driven by the host's non-realtime tick, one
// per render turn. So "settling" is not one block; the longest IR the Size knob
// reaches (4 s) takes ~120 render turns to rebuild, and the OLD IR keeps playing
// until it lands. 300 blocks is comfortably past that. This is the price of never
// glitching: the reverb follows the knob with ~300 ms of lag instead of stalling
// the render thread for 15 ms. (See SuperConvolverProcessor::kRebuildSliceItems —
// it is the knob that trades this lag against per-quantum headroom.)
const settle = (blocks = 300) => {
  inL.fill(0); inR.fill(0);
  for (let b = 0; b < blocks; b++) eng.process(inL, inR);
};

console.log(`[${eng.name}] abi=${eng.abi} params=${eng.params.length}`);
check("descriptor is SuperConvolver", eng.name === "SuperConvolver", eng.name);
check("Mix / Size / Gain parameters exposed",
      !!byLabel.Mix && !!byLabel.Size && !!byLabel.Gain,
      eng.params.map((p) => p.label).join(", "));

const MIX = idOf("Mix"), SIZE = idOf("Size"), GAIN = idOf("Gain");

// ── (0) parameter DISPLAY TEXT is identical across the two ABIs. ─────────────
// The demo pages mount the SAME shared player and the SAME Pulp web UI, so a
// parameter must read "35.00 %" on both. It did not: the WebCLAP adapter
// hardcoded an empty unit (CLAP has no unit field on clap_param_info) and the
// page showed a bare "35.00" while the WAM page showed "35.00 %". Both lanes now
// answer from the plugin: WAM from its descriptor unit, WebCLAP from the CLAP
// value_to_text call a native host uses. Golden strings, so a divergence in
// EITHER lane fails here rather than in someone's browser.
const UNITS = { Mix: "%", Size: "s", Gain: "dB" };
for (const [label, unit] of Object.entries(UNITS)) {
  check(`${label} reports its display unit ("${unit}")`, byLabel[label].unit === unit,
        `got "${byLabel[label].unit}"`);
}
for (const [id, value, expected] of [[MIX, 35, "35.00 %"], [SIZE, 1.5, "1.50 s"], [GAIN, 0, "0.00 dB"]]) {
  const text = eng.paramText(id, value);
  check(`parameter text "${expected}"`, text === expected, `got "${text}"`);
}

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

// ── (c2) THE SIZE KNOB IS GLITCH-FREE. ──────────────────────────────────────
// The web lanes have no worker thread, so the IR rebuild that a Size change
// triggers runs inside the host's non-realtime tick — which an AudioWorklet
// dispatches between render quanta ON THE RENDER THREAD. It used to be a blocking
// pass: 15.0 ms of IR synthesis + FFT re-partition in ONE render callback against
// a 2.667 ms quantum budget, i.e. a dropout every time the knob moved. It is now
// sliced (SlicedIrRebuild), so no single render call may run long.
//
// The primary assertion is BEHAVIOURAL and machine-independent — audio must keep
// coming out, at a steady level, through the whole sweep, with no silent block
// (which is what a dropout would be). The wall-clock check is a loose backstop
// that would still catch a regression to the blocking rebuild; it is deliberately
// generous, because a CI box's scheduler is not a real-time audio thread.
{
  const QUANTUM_MS = (FR / SR) * 1000;   // 2.667 ms at 48 kHz / 128 frames
  eng.setParam(MIX, 100);
  eng.setParam(SIZE, 0.05);
  settle();

  let worstMs = 0, silentBlocks = 0, measured = 0;
  const steps = 40;
  for (let s = 0; s <= steps; s++) {
    eng.setParam(SIZE, 0.05 + (3.95 * s) / steps);   // the drag
    for (let b = 0; b < 24; b++) {
      for (let f = 0; f < FR; f++) { const v = noise() * 0.5; inL[f] = v; inR[f] = v; }
      const t0 = process.hrtime.bigint();
      const out = eng.process(inL, inR);
      worstMs = Math.max(worstMs, Number(process.hrtime.bigint() - t0) / 1e6);
      let sq = 0;
      for (let f = 0; f < FR; f++) sq += out[0][f] * out[0][f];
      // Skip the first few blocks of the sweep (the reverb is still filling); after
      // that, EVERY block must carry audio. A rebuild that stalled the render
      // thread would show up as a block the host had to fill with silence.
      if (measured++ > 8 && Math.sqrt(sq / FR) < 1e-4) silentBlocks++;
    }
  }
  check("audio never drops out across a full Size sweep",
        silentBlocks === 0, `${silentBlocks} silent block(s) of ${measured}`);
  check("no render call runs long during a Size sweep (the rebuild is sliced)",
        worstMs < QUANTUM_MS,
        `worst=${worstMs.toFixed(3)} ms, quantum budget=${QUANTUM_MS.toFixed(3)} ms ` +
        `(was 15.0 ms with the blocking rebuild)`);
}

// ── (d) state round-trip: parameters + the plugin's own IR-source blob. ──────
// Both ABIs now: WAM through wam_read_state/wam_write_state, WebCLAP through the
// clap.state extension. Same "PLST" bytes either way — which is the whole reason
// the IR upload in (e) needs no per-ABI entry point.
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

// ── (e) LOAD YOUR OWN IMPULSE RESPONSE — the browser equivalent of the native
//    plugin's file dialog, on BOTH ABIs from ONE code path. ────────────────────
// The page decodes an audio file with the Web Audio API and writes the samples
// into the plugin's state as an SCv2 "Pcm" record (ir-source.js), spliced into the
// live state's plugin slot so the user's knob positions survive (plugin-state.js).
// Neither module is web-only — they are imported here and driven straight through
// both hosts, so the exact bytes the demo page sends are the bytes under test.
//
// The reverb must then BE that IR: a synthetic "space" whose energy is a single
// late echo produces a tail with an obvious gap the built-in IR does not have.
if (saved) {
  const IR_SR = SR, IR_FRAMES = 12000;   // 0.25 s
  // A deliberately un-reverb-like IR: a direct hit, then ONE discrete echo at
  // 0.2 s and nothing else. Convolving with it must reproduce that echo.
  const custom = new Float32Array(IR_FRAMES);
  custom[0] = 1.0;
  custom[Math.floor(0.2 * IR_SR)] = 0.9;

  const blob = buildPcmIrBlob(custom, IR_SR);
  check("the page's IR record is a valid SCv2 Pcm blob", readIrBlobKind(blob) === 2,
        "kind=" + readIrBlobKind(blob));

  // Splice it into the live state — parameters preserved — and write it back. This
  // IS adapter.setState(); the shell's HostAdapter makes the call identical on WAM
  // and WebCLAP.
  const before = eng.readState();
  const params = parseContainer(before).params;
  eng.setParam(MIX, 100);
  eng.setParam(SIZE, 4.0);   // longer than the IR: a shorter loaded base is left whole
  check("IR upload accepted (setState)", eng.writeState(buildContainer(params, blob)));
  settle(600);               // the rebuild is sliced — give the host its ticks

  // The uploaded IR is live: drive an impulse and look for the echo it defines.
  const impulse = () => {
    const acc = [];
    for (let b = 0; b < 200; b++) {
      for (let f = 0; f < FR; f++) {
        const v = (b === 0 && f === 0) ? 1.0 : 0.0;
        inL[f] = v; inR[f] = v;
      }
      const out = eng.process(inL, inR);
      for (let f = 0; f < FR; f++) acc.push(out[0][f]);
    }
    return acc.slice(eng.latency());   // read back from the reported latency
  };
  const resp = impulse();
  const at = Math.floor(0.2 * IR_SR);
  const echo = Math.max(...resp.slice(at - 8, at + 8).map(Math.abs));
  const between = Math.max(...resp.slice(2000, at - 2000).map(Math.abs));
  check("the uploaded IR is what the plugin convolves with",
        echo > 0.2 && echo > between * 8,
        `echo at 0.2 s = ${echo.toFixed(4)}, quiet zone before it = ${between.toFixed(4)}`);

  // It survives a save/restore, because the IR IS the state.
  const withIr = eng.readState();
  check("the uploaded IR is carried in the saved state",
        withIr.length > 4 * IR_FRAMES, `${withIr.length} bytes`);
  eng.writeState(buildContainer(parseContainer(withIr).params, buildSyntheticIrBlob()));
  settle(600);
  check("state restore brings the uploaded IR back", eng.writeState(withIr));
  settle(600);
  const restoredResp = impulse();
  const echo2 = Math.max(...restoredResp.slice(at - 8, at + 8).map(Math.abs));
  check("…and the restored plugin convolves with it again",
        Math.abs(echo2 - echo) < 0.05 * echo,
        `echo before=${echo.toFixed(4)} after=${echo2.toFixed(4)}`);

  // Reverting to the built-in synthetic reverb is the same call, "Synthetic" kind.
  eng.writeState(buildContainer(parseContainer(withIr).params, buildSyntheticIrBlob()));
  settle(1200);
  const back = impulse();
  const echo3 = Math.max(...back.slice(at - 8, at + 8).map(Math.abs));
  const dense = Math.max(...back.slice(2000, at - 2000).map(Math.abs));
  check("revert to the built-in reverb (a dense tail, not the uploaded echo)",
        dense > 1e-3 && echo3 < echo * 0.5,
        `built-in tail=${dense.toFixed(4)}, echo now=${echo3.toFixed(4)}`);
} else {
  console.log("  skip IR upload (no state helper on this ABI's host)");
}

console.log(fails.length ? `\nFAILURES (${fails.length}): ${fails.join(", ")}` : "\nALL CHECKS PASSED");
process.exit(fails.length ? 1 : 0);
