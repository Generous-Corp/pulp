// THE PROOF: SuperConvolver's FFT convolution, running as WGSL compute shaders
// on the browser's WebGPU device, is what produced the audio you hear.
//
// Neither existing browser fixture is the right host for this claim:
// super-convolver-ui/browser-test/validate.mjs is a PIXEL fixture with a mock
// HostAdapter and no audio at all, and wasm-build/browser-test/validate.mjs
// drives an OfflineAudioContext — which renders FASTER than wall clock and so
// starves the async GPU transport into permanent misses (with the CPU net killed
// that is silence: a guaranteed false FAIL). This fixture reuses their
// scaffolding (a local COOP/COEP server, a system Chrome through playwright-core,
// window.__result, --screenshot, exit 0 = PASS) and nothing else.
//
// It runs a REAL-TIME AudioContext. Measured, headless, with no audio device:
// state=running, currentTime advanced 1.4933 s over 1.5004 s wall, 500+ quanta,
// sampleRate 48000 — with only --autoplay-policy=no-user-gesture-required.
//
// THREE RUNS
//   A  Engine=GPU with the CPU net KILLED ("GPU only" = 1). The audio must match
//      an INDEPENDENT float64 direct time-domain convolution (oracle.mjs) — not
//      Pulp's convolver, not the wasm. If the GPU never produced a block this run
//      is SILENT and FAILS. This is the backbone.
//   B  THE TAMPER RUN. Read the ACTUAL WGSL the module hands to Dawn out of the
//      module (pulp_gpu_kernel_source), scale its output store by 0.5 IN
//      JAVASCRIPT, push it back through pulp_gpu_override_kernel BEFORE
//      pulp_gpu_init, and assert the audio is Run A x 0.5 SAMPLE-WISE and != Run
//      A. A JS/wasm impostor is unaffected by an edit to a shader it never runs.
//      Reading the source out of the module (rather than pasting a copy here)
//      makes the test drift-proof.
//   C  Engine=CPU. Matches the oracle, and the GPU stats show it never
//      dispatched: produced == 0 and queueSubmits == 0.
//
// REPORTED, NEVER GATED: adapter info, the counter invariants, and gpuNsLast.
// NEVER assert gpu_ns > 0 — MEASURED: timestamp-query IS grantable in headless
// Chrome but returned 0 ns for a 256-element dispatch and 1,376,256 ns for a 1M
// one. Chrome quantizes; a 0 for a 512-point FFT block does NOT mean the GPU
// idled. "The device and the pipelines exist" is likewise a DIAGNOSTIC, not
// proof: it passes falsely when nothing ever dispatches and the CPU net covers.
//
// ALIGNMENT (why the truncated oracle kernel is EXACT, not approximate): the
// source buffer is [PREROLL silence][unit impulse][D zeros][noise burst][gap][unit
// impulse]. The plugin's response to the lone impulse IS its impulse response h, so
// the first non-silent captured sample is a marker that removes the plugin's
// latency from both the kernel and the signal window. With D > N the impulse's own
// response has left the window under test, and by causality the first N samples of
// (x * h) depend only on h[0..N) — so comparing against a convolution with the
// first N taps of the MEASURED h is exact. h is measured on the CPU engine (Run C),
// so the kernel Run A is judged against never came from the GPU.
//
// THE PREROLL AND THE SECOND IMPULSE — the oracle's whole argument assumes the
// engine is LINEAR TIME-INVARIANT across the capture: measure h once, convolve, and
// expect the noise burst to agree. SuperConvolver is only LTI once its IR has
// STOPPED CHANGING, and it does not start that way. A Size change does not take
// effect at once: the rebuild (decode → window → FFT plan) is TIME-SLICED across
// many process() calls and crossfaded (superconvolver::SlicedIrRebuild), precisely
// so the render callback stays inside its budget instead of spiking. So the engine
// spends the first stretch after setShortIr() crossfading from the DEFAULT 1.5 s IR
// toward the 0.05 s one — it is genuinely time-VARYING, and h probed there is a
// blend of two IRs that describes no instant of the run.
//
// Capturing immediately is therefore measuring a moving target: it FAILED that way
// (CPU vs oracle: relative RMS 3.4 — not a drift, a different signal), and it fails
// in a way that frames the ENGINE as broken when the fixture is what is wrong.
//
// So: PREROLL samples of silence run first. The graph is live, process() is being
// called, the plugin is draining its sliced rebuild — and nothing is being measured
// yet. THEN the marker impulse.
//
// A silent pre-roll is still only an ASSUMPTION that convergence fits inside it, and
// an assumption that decays the moment someone changes the slice budget or the IR
// length. So the source ends with a SECOND unit impulse, after the analysis window,
// and its response must equal the first one's. That is a direct test of the
// time-invariance the oracle depends on: if the IR were still moving anywhere in the
// measured span, the two impulse responses would disagree and this fails LOUDLY with
// a named reason — instead of silently poisoning every other check downstream.
//
// THE TAMPER SHIM: pulp_gpu_override_kernel lives on the GPU DSP module, which is
// instantiated inside the DedicatedWorker (gpu-worker.mjs) — the page cannot
// reach it. So this fixture SERVES the worker a shim ES module in place of
// pulp-gpu-dsp.js: the shim awaits the real factory, reads the real WGSL out of
// the real module, rewrites it, and pushes it back through the real C ABI —
// all BEFORE the worker calls pulp_gpu_init (the override is rejected after
// initialization, by design). Nothing in the shipped bridge or worker is special-
// cased for the test.
//
// Usage:
//   node validate-gpu.mjs --gpu-wasm <SuperConvolverGpu.wasm> --gpu-build <pulp-gpu-dsp dir>
//                         [--browser <path>] [--screenshot <png>] [--headed] [--tol <rel-rms>]
// Exit 0 = PASS, or a NAMED SKIP when the machine has no WebGPU adapter.
// PULP_REQUIRE_WEBGPU=1 turns that skip into a hard failure — that is what stops
// this gate degrading into always-skipped-always-green.

import http from "node:http";
import { readFile } from "node:fs/promises";
import { existsSync } from "node:fs";
import { extname, resolve, join } from "node:path";
import { chromium } from "playwright-core";
import {
  convolve, firstOnset, makeNoise, maxScaledDeviation, relativeRmsError, rms,
} from "./oracle.mjs";

const HERE = new URL(".", import.meta.url).pathname;
const REPO = resolve(HERE, "../../../..");
const PLAYER = resolve(REPO, "packages/pulp-web-player/src");
const BRIDGE = resolve(HERE, "../js");           // gpu-bridge / gpu-worker / gpu-ring
const PORT = 8736;
const PAGE = `http://localhost:${PORT}/`;

function arg(flag, dflt) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : dflt;
}
const gpuWasm = arg("--gpu-wasm", null);
const gpuBuild = arg("--gpu-build", null);
const screenshot = arg("--screenshot", null);
const headed = process.argv.includes("--headed");
const requireWebGpu = process.env.PULP_REQUIRE_WEBGPU === "1";

// The GPU path is an FP32 FFT convolution; the oracle is a float64 direct
// convolution, so the gap is rounding, not structure.
//
// MEASURED on this branch (Apple M-series / metal-3, headless Chrome 150), not
// guessed — the instruction was to derive this rather than invent it:
//   Run A (GPU, WGSL FFT)            relative RMS error vs the oracle: 1.68e-7
//   Run C (CPU PartitionedConvolver) relative RMS error vs the oracle: 2.03e-7
// Both engines land at ~2e-7, which is exactly the float32 rounding floor for an
// FFT convolution of this length — and notably the GPU is no worse than the CPU.
//
// 1e-5 pins the gate ~50x above the measured floor: tight enough that a wrong IR,
// a wrong scale factor, or an off-by-one in the overlap-add cannot slip through
// (those all land at 1e-2 or worse), loose enough to absorb a different GPU's
// rounding. The earlier 2e-3 placeholder was 10,000x the real error and would have
// passed a visibly wrong convolution.
const TOL = Number(arg("--tol", process.env.PULP_GPU_RMS_TOL || "1e-5"));
// The two impulse responses are the SAME engine convolving the SAME unit impulse,
// so they agree to the bit unless the IR actually moved between them. The tolerance
// is loose only to absorb block-phase noise; what it must reject is a still-
// converging rebuild, and that misses by O(1), not by 1e-4.
const TOL_IR = 1e-4;

const BROWSERS = [
  arg("--browser", null),
  process.env.PLAYWRIGHT_CHROMIUM_PATH,
  process.env.CHROME_PATH,
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium-browser",
  "/usr/bin/chromium",
].filter(Boolean);

const die = (m) => { console.error("FAIL: " + m); process.exit(1); };
if (!gpuWasm || !existsSync(gpuWasm)) die("--gpu-wasm <SuperConvolverGpu.wasm> is required");
if (!gpuBuild || !existsSync(join(gpuBuild, "pulp-gpu-dsp.js")))
  die("--gpu-build <dir with pulp-gpu-dsp.js + .wasm> is required");
for (const f of ["gpu-bridge.mjs", "gpu-worker.mjs", "gpu-ring.mjs"])
  if (!existsSync(join(BRIDGE, f))) die(`the browser GPU bridge is missing: ${join(BRIDGE, f)}`);

// ── Signal geometry. N is the comparison window; D keeps the marker impulse's
//    own response out of it (D > N).
const N = 8192;
const D = 16384;
const BURST = 8192;
// Silence before the marker impulse, so the time-sliced IR rebuild has converged
// before anything is measured (see THE PREROLL AND THE SECOND IMPULSE). 0.5 s is
// ~187 process() quanta — orders of magnitude more than the rebuild needs — and the
// second impulse below is what actually PROVES it landed, so this length is a
// comfortable margin rather than a load-bearing guess.
const PREROLL = 24000;
// The second impulse sits GAP samples past the end of the analysis window, far
// enough that the noise burst's own decaying tail (<= the IR's 2,400 taps at minimum
// Size) cannot bleed into it.
const IMP2_GAP = 8192;
const IMP2_OFF = D + N + IMP2_GAP;         // relative to the marker, in both source and capture
const RUN_SECONDS = 3;                     // real time; also the miss-rate window
const SAMPLE_RATE = 48000;
const CAPTURE = SAMPLE_RATE * RUN_SECONDS;
const BLOCK = 512;                         // SuperConvolver::kInternalBlock
const LATENCY_BLOCKS = 2;                  // SuperConvolver::kWebGpuLatencyBlocks

const noise = makeNoise(BURST);

// The tamper: scale every store into an output storage buffer by 0.5. It is
// applied to the WGSL the MODULE hands us, so a shader rename or a reformat is a
// loud failure here rather than a silent no-op.
const TAMPER_KERNEL = "conv_bmul";
const TAMPER_SHIM = `
import factory from "./pulp-gpu-dsp.js";
const KERNEL = ${JSON.stringify(TAMPER_KERNEL)};

function mutate(src) {
  // \`out[i] = expr;\` -> \`out[i] = 0.5 * (expr);\` for any storage buffer whose name
  // reads like an output.
  //
  // LINE-SCOPED, AND COMMENTS ARE SKIPPED — both deliberately. A whole-source regex
  // with a \`[^;]+\` tail is a trap: conv_bmul's header comment contains the prose
  // "result[p] = a[p] * ir[p % ir_pairs]." with NO semicolon, so the tail runs past
  // the end of the comment and swallows the next real statement — the
  // \`@group(0) @binding(0) var<storage, read> a : array<f32>;\` binding — leaving
  // \`array<f32>);\`. That is invalid WGSL, and (MEASURED) Dawn does NOT fail
  // pipeline creation for it: it reports the compile error asynchronously on the
  // uncaptured-error callback, so the module comes up "fine" and every dispatch
  // quietly produces zeros. The tamper run then fails as SILENCE and looks like a
  // dead GPU rather than a broken test. Match one statement, on one code line.
  const lines = src.split("\\n");
  let hits = 0;
  const out = lines.map((line) => {
    if (/^\\s*\\/\\//.test(line)) return line;                       // comment: never touch
    return line.replace(/^(\\s*)(\\w*(?:out|dst|result)\\w*)\\s*\\[([^\\]]+)\\]\\s*=\\s*([^;\\n]+);(\\s*)$/i,
                        (m, lead, buf, idx, rhs, tail) => {
                          hits++;
                          return \`\${lead}\${buf}[\${idx}] = 0.5 * (\${rhs});\${tail}\`;
                        });
  });
  self.__tamperHits = hits;
  return out.join("\\n");
}

export default async function (opts) {
  const M = await factory(opts);
  const label = M.stringToNewUTF8(KERNEL);
  const src = M.UTF8ToString(M._pulp_gpu_kernel_source(label));
  if (!src) { M._free(label); throw new Error("tamper: pulp_gpu_kernel_source('" + KERNEL + "') is empty"); }
  const mutated = mutate(src);
  if (mutated === src) { M._free(label); throw new Error("tamper: the WGSL rewrite matched nothing"); }
  const wgsl = M.stringToNewUTF8(mutated);
  const ok = M._pulp_gpu_override_kernel(label, wgsl);
  M._free(label); M._free(wgsl);
  if (!ok) throw new Error("tamper: pulp_gpu_override_kernel refused the source");
  // Read back through the module: what Dawn will compile is what we asserted on.
  const l2 = M.stringToNewUTF8(KERNEL);
  self.__tamper = { before: src.length, after: M.UTF8ToString(M._pulp_gpu_kernel_source(l2)).length };
  M._free(l2);
  return M;
}
`;

// ── The page. All audio happens here; node drives it and does the float64 maths.
const INDEX_HTML = `<!doctype html>
<meta charset="utf-8">
<style>body{margin:0;background:#0c1116;color:#cdd6e3;font:13px/1.6 system-ui}
pre{padding:16px;white-space:pre-wrap}</style>
<pre id="log">booting…</pre>
<script type="module">
import { createWclapAdapter } from "/player/adapters/wclap.js";
import { probe, startGpuLane } from "/bridge/gpu-bridge.mjs";

const log = (m) => { document.getElementById("log").textContent += "\\n" + m; };
const N = ${N}, D = ${D}, CAPTURE = ${CAPTURE}, BLOCK = ${BLOCK};
// The page builds the source buffer, so the pre-roll and the second impulse's offset
// have to cross the string boundary with the rest of the geometry.
const PREROLL = ${PREROLL}, IMP2_OFF = ${IMP2_OFF};
const LATENCY_BLOCKS = ${LATENCY_BLOCKS}, SAMPLE_RATE = ${SAMPLE_RATE};
const NOISE = Float32Array.from(${JSON.stringify(Array.from(noise))});
window.__preflight = null;
window.__bootError = null;

// A raw adapter probe, REPORTED and never gated: the adapter identity is a
// diagnostic. The real handshake is startGpuLane() — it is the worker, not this,
// that has to get a device, compile the shaders, and pass a self-test.
async function adapterProbe() {
  if (!navigator.gpu) return { ok: false, reason: "no-webgpu" };
  const a = await navigator.gpu.requestAdapter();
  if (!a) return { ok: false, reason: "no-adapter" };
  const i = a.info || {};
  return { ok: true, isolated: !!crossOriginIsolated,
           info: { vendor: i.vendor || "", architecture: i.architecture || "",
                   device: i.device || "", description: i.description || "" },
           features: [...a.features] };
}

function sourceBuffer(ctx) {
  // [PREROLL silence][impulse][D-1 zeros][noise burst][gap][impulse][silence]
  // — see the ALIGNMENT note.
  const buf = ctx.createBuffer(1, CAPTURE, ctx.sampleRate);
  const ch = buf.getChannelData(0);
  ch[PREROLL] = 1;                       // the marker, and the measured IR
  ch.set(NOISE, PREROLL + D);            // the signal under test
  ch[PREROLL + IMP2_OFF] = 1;            // the time-invariance check
  return buf;
}

const findParam = async (adapter, re) =>
  ((await adapter.getParameterInfo()) || []).find((p) => re.test(p.label || "")) || null;

// Drive Size to its MINIMUM, so the plugin's impulse response is SHORTER than the
// analysis geometry. This is load-bearing, not tidying.
//
// The oracle convolves with h = the first N (8192) captured taps of the measured
// impulse response, and the signal window starts D (16384) samples after the
// marker. Both the "truncated kernel is exact" and the "the impulse's own response
// has left the window" arguments in the header require the plugin's IR to be
// shorter than N and than D. At the DEFAULT Size (1.5 s = 72,000 taps at 48 kHz)
// BOTH are false: the impulse tail bleeds straight through the window under test
// and h misses 63,808 of the real taps.
//
// MEASURED, and worth stating because it is a trap: with the default Size the CPU
// engine misses the oracle by 1.3e-2 while the GPU engine matches it to 2.1e-7 —
// which reads like "the CPU convolver is broken" and is nothing of the sort. The
// GPU is exact only because the fixture HANDS it that same 8192-tap h as its IR,
// so it is being compared against its own kernel. The CPU engine is the honest one:
// it is convolving with all 72,000 taps and the oracle only knows 8,192 of them.
// At minimum Size (0.05 s = 2,400 taps) h captures the ENTIRE IR, the impulse has
// long decayed by D, and the oracle is exact for BOTH engines — which is also the
// only condition under which the CPU net and the GPU are convolving with the same
// IR at all, i.e. the condition the whole latency-aligned fallback assumes.
async function setShortIr(adapter) {
  const size = await findParam(adapter, /^size$/i);
  if (!size) throw new Error("the plugin exports no Size parameter — the IR length cannot be bounded");
  adapter.setParameterValue(size.id, size.minValue);
}

// Run the graph for RUN_SECONDS of WALL-CLOCK time and hand back every rendered
// frame, sample-exact, from the tap worklet's SharedArrayBuffer.
async function capture(ctx, adapter) {
  const samples = new SharedArrayBuffer(CAPTURE * 4);
  const index = new SharedArrayBuffer(4);
  const tap = new AudioWorkletNode(ctx, "pulp-tap", {
    numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1],
    processorOptions: { samples, index },
  });

  const src = ctx.createBufferSource();
  src.buffer = sourceBuffer(ctx);
  src.connect(adapter.audioNode);
  adapter.audioNode.connect(tap);
  const silent = ctx.createGain();          // pull the graph without making noise
  silent.gain.value = 0;
  tap.connect(silent).connect(ctx.destination);

  await ctx.resume();
  const t0 = ctx.currentTime;
  src.start();
  await new Promise((r) => setTimeout(r, ${RUN_SECONDS} * 1000 + 400));

  return {
    audio: Array.from(new Float32Array(samples)),
    captured: Atomics.load(new Int32Array(index), 0),
    elapsed: ctx.currentTime - t0,
  };
}

// One run. \`engine\` 1 = GPU, 0 = CPU. \`gpuOnly\` kills the CPU net, so a GPU that
// never dispatched yields SILENCE instead of a CPU-covered impostor. \`tamper\`
// swaps the worker's DSP module for the shim that rewrites the WGSL.
window.__run = async ({ engine, gpuOnly, tamper, blockWorker }) => {
  const ctx = new AudioContext({ sampleRate: SAMPLE_RATE });
  const notes = [];
  try {
    await ctx.audioWorklet.addModule("/fixture/tap-worklet.js");

    const pre = probe();
    if (!pre.ok) return { error: "bridge preconditions failed: " + pre.reason };

    const lane = await startGpuLane({
      // blockWorker: the NEGATIVE path. A 404 on the worker script is the same
      // shape of failure as a browser with no WebGPU: the lane must come back
      // with a NAMED reason, and the page must fall back to the CPU plugin with
      // no Engine toggle at all.
      workerUrl: blockWorker ? "/bridge/no-such-worker.mjs" : "/bridge/gpu-worker.mjs",
      moduleUrl: tamper ? "/dsp/tamper-shim.js" : "/dsp/pulp-gpu-dsp.js",
      ir: window.__ir || null,
      sampleRate: SAMPLE_RATE, blockSize: BLOCK, latencyBlocks: LATENCY_BLOCKS,
      onDeviceLost: (i) => notes.push("device-lost:" + (i && i.reason)),
    });
    if (!lane.ok && !blockWorker) return { error: "startGpuLane failed: " + lane.reason };
    if (lane.ok && blockWorker) return { error: "the blocked worker did NOT fail the handshake" };

    const laneOk = lane.ok;
    const adapter = await createWclapAdapter(ctx,
      { dsp: "/dsp/SuperConvolverGpu.wasm", processor: "/player/vendor/pulp-wasm/wclap-processor.js" },
      laneOk ? { gpuSab: lane.sab, gpuLatencyBlocks: lane.latencyBlocks } : {});
    if (!laneOk) {
      // What the page renders in this state: a NAMED reason, and no toggle.
      const engineParam = await findParam(adapter, /^engine$/i);
      if (engineParam) adapter.setParameterValue(engineParam.id, 0);   // CPU
      const mixOff = await findParam(adapter, /^mix$/i);
      if (mixOff) adapter.setParameterValue(mixOff.id, mixOff.maxValue);
      await setShortIr(adapter);   // same geometry as every other run — see setShortIr
      const out = await capture(ctx, adapter);
      await ctx.close();
      return { ...out, reason: lane.reason, gpuLane: !!(adapter.descriptor && adapter.descriptor.gpuLane) };
    }

    if (!adapter.descriptor || !adapter.descriptor.gpuLane)
      notes.push("the worklet did NOT attach the GPU ring (descriptor.gpuLane is false)");

    // Fully wet: the oracle models the convolution, not the dry mix.
    const mix = await findParam(adapter, /^mix$/i);
    if (mix) adapter.setParameterValue(mix.id, mix.maxValue);
    await setShortIr(adapter);

    const engineParam = await findParam(adapter, /^engine$/i);
    if (!engineParam) throw new Error("the plugin exports no Engine parameter");
    adapter.setParameterValue(engineParam.id, engine);

    if (gpuOnly) {
      const only = await findParam(adapter, /gpu.?only/i);
      if (!only) throw new Error("the plugin exports no 'GPU only' parameter — the CPU net cannot be killed");
      adapter.setParameterValue(only.id, only.maxValue);
    }

    const out = await capture(ctx, adapter);
    const stats = lane.pollStats();
    lane.shutdown();
    await ctx.close();
    return { ...out, stats, notes, adapterInfo: lane.adapterInfo,
             timestampQuery: lane.timestampQuery,
             gpuLane: !!(adapter.descriptor && adapter.descriptor.gpuLane) };
  } catch (e) {
    try { await ctx.close(); } catch {}
    return { error: String((e && e.stack) || e), notes };
  }
};

// The impulse response the oracle convolves with, captured from the CPU engine
// (Run C) and reused as the kernel for the GPU runs.
window.__ir = null;

try {
  window.__preflight = await adapterProbe();
  log("webgpu: " + JSON.stringify(window.__preflight));
  window.__ready = true;
} catch (e) {
  window.__bootError = String((e && e.stack) || e);
}
</script>`;

const MIME = {
  ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm",
  ".html": "text/html",
};

const server = http.createServer(async (req, res) => {
  const path = decodeURIComponent(req.url.split("?")[0]);
  const head = {
    "cross-origin-opener-policy": "same-origin",
    "cross-origin-embedder-policy": "require-corp",
    "cross-origin-resource-policy": "cross-origin",
  };
  const send = (body, type) => {
    res.writeHead(200, { ...head, "content-type": type });
    res.end(body);
  };
  if (path === "/") return send(INDEX_HTML, "text/html");
  if (path === "/dsp/tamper-shim.js") return send(TAMPER_SHIM, "text/javascript");

  let file = null;
  if (path.startsWith("/player/")) file = resolve(PLAYER, path.slice(8));
  else if (path.startsWith("/bridge/")) file = resolve(BRIDGE, path.slice(8));
  else if (path.startsWith("/fixture/")) file = resolve(HERE, path.slice(9));
  else if (path === "/dsp/SuperConvolverGpu.wasm") file = resolve(gpuWasm);
  else if (path.startsWith("/dsp/")) file = resolve(gpuBuild, path.slice(5));
  if (!file) { res.writeHead(403); return res.end(); }
  try {
    send(await readFile(file), MIME[extname(file)] || "application/octet-stream");
  } catch {
    console.log("  [404]", path);
    res.writeHead(404);
    res.end("not found: " + path);
  }
});
await new Promise((r) => server.listen(PORT, r));

const steps = [];
const check = (name, pass, detail = "") => {
  steps.push({ name, pass, detail });
  console.log(`  ${pass ? "ok  " : "FAIL"} ${name}${detail ? " — " + detail : ""}`);
};
const report = (name, detail) => {
  steps.push({ name, pass: true, detail, diagnostic: true });
  console.log(`  ..   ${name} — ${detail}`);
};

// Marker-aligned window: drop the plugin's latency, then take the N samples the
// noise burst drove. Returns { why } instead of a window when it cannot align.
//
// The three ways this fails are THREE DIFFERENT BUGS and must never be flattened
// into one word. "silent" is the one the proof is built on (a GPU that never
// produced a block, with the CPU net dead). "nan" is a poisoned parameter or a
// broken kernel — a nonfinite sample compares false against EVERY threshold, so a
// naive onset scan reports it as silence and the failure lies to you. "short" is a
// capture that under-ran. Diagnosing "nan" as "the GPU never ran" cost a debugging
// cycle here; the message now says which one it is.
function align(audio) {
  let peak = 0, nans = 0;
  for (let i = 0; i < audio.length; i++) {
    const v = audio[i];
    if (!Number.isFinite(v)) { nans++; continue; }
    const a = Math.abs(v);
    if (a > peak) peak = a;
  }
  if (nans > 0)
    return { why: `NOT SILENCE — ${nans} nonfinite samples (peak of the finite ones ${peak.toExponential(2)}); ` +
                  `a NaN reads as silence to any threshold, so this is a poisoned signal, not a dead GPU` };
  const marker = firstOnset(audio, 1e-4);
  if (marker < 0) return { why: `SILENCE — peak |x| = ${peak.toExponential(2)}, ${audio.length} samples scanned` };
  const h = Float64Array.from(audio.slice(marker, marker + N));
  const y = Float64Array.from(audio.slice(marker + D, marker + D + N));
  // The SAME impulse, replayed after the analysis window. Its response is at the
  // same offset in the capture as in the source, because the plugin's latency is
  // fixed — that is the premise the marker already relies on.
  const h2 = Float64Array.from(audio.slice(marker + IMP2_OFF, marker + IMP2_OFF + N));
  const need = marker + IMP2_OFF + N;
  if (h.length !== N || y.length !== N || h2.length !== N)
    return { why: `capture too short — marker at ${marker}, need ${need} samples, have ${audio.length}` };
  return { marker, h, y, h2, peak };
}
const aligned = (w) => !!(w && w.h);

let browser;
let skipped = null;
try {
  const exe = BROWSERS.find((p) => existsSync(p));
  if (!exe) throw new Error("no Chrome/Chromium binary found (set CHROME_PATH or --browser)");
  browser = await chromium.launch({
    executablePath: exe,
    headless: !headed,
    // A real-time AudioContext must start without a user gesture. Deliberately
    // NOT --enable-unsafe-swiftshader: MEASURED, it does not rescue WebGPU (Chrome
    // on macOS has no software WebGPU adapter at all — --use-webgpu-adapter=
    // swiftshader and forceFallbackAdapter:true both return null), so adding it
    // would only obscure which lane actually ran.
    args: ["--autoplay-policy=no-user-gesture-required"],
  });
  const page = await browser.newPage({ viewport: { width: 900, height: 600 } });
  page.on("console", (m) => console.log("  [page]", m.text()));
  page.on("pageerror", (e) => console.log("  [pageerror]", e.message));
  await page.goto(PAGE, { waitUntil: "load" });
  await page.waitForFunction(() => window.__ready || window.__bootError, null, { timeout: 30000 });
  const bootError = await page.evaluate(() => window.__bootError);
  if (bootError) throw new Error("page failed to boot:\n" + bootError);

  const pre = await page.evaluate(() => window.__preflight);
  if (!pre.ok) {
    if (requireWebGpu)
      throw new Error(`PULP_REQUIRE_WEBGPU=1 and WebGPU is unavailable (${pre.reason})`);
    skipped = pre.reason;
    console.log(`SKIP: webgpu-unavailable (${pre.reason})`);
  } else {
    report("adapter (diagnostic, never proof)",
           `${pre.info.vendor || "?"} / ${pre.info.architecture || "?"} ` +
           `${pre.info.device || ""} ${pre.info.description || ""}`.trim() +
           ` · crossOriginIsolated=${pre.isolated} · features: ${pre.features.join(",") || "none"}`);

    // ── RUN C first: it measures the impulse response on the CPU engine, so the
    //    kernel Run A is judged against never came from the GPU.
    const runC = await page.evaluate(() => window.__run({ engine: 0, gpuOnly: false }));
    if (runC.error) throw new Error("Run C (CPU) failed: " + runC.error);
    const wc = align(runC.audio);
    check("Run C — the CPU engine produced audio", aligned(wc),
          aligned(wc) ? `marker at ${wc.marker}, peak ${wc.peak.toFixed(3)}, ${runC.captured} frames captured`
                      : wc.why);
    if (!aligned(wc)) throw new Error("Run C produced no usable audio — the harness cannot proceed");

    // GATE THE KERNEL BEFORE ANYTHING USES IT. h is about to become the oracle's
    // kernel AND the GPU worker's IR, so if the engine was still mid-rebuild when h
    // was probed, every check downstream is measuring a moving target and reports
    // the ENGINE as broken. Replaying the same impulse after the analysis window
    // answers that directly: equal responses = the IR held still across the whole
    // measured span (LTI), which is the premise the oracle rests on.
    const drift = relativeRmsError(wc.h2, wc.h, N);
    check("Run C — the CPU engine's impulse response is TIME-INVARIANT across the capture " +
          "(the sliced IR rebuild converged in the pre-roll, so h is a real IR and not a crossfade)",
          drift < TOL_IR, `relative RMS drift between the two impulse responses ${drift.toExponential(2)} ` +
                          `(tolerance ${TOL_IR}); pre-roll ${PREROLL} samples`);
    if (!(drift < TOL_IR))
      throw new Error(
        "the measured impulse response MOVED during the capture — h is a blend of two IRs, not an IR. " +
        "Every oracle comparison downstream would be judged against a kernel that describes no instant of " +
        "the run. Most likely the time-sliced IR rebuild has not converged within PREROLL: raise PREROLL, " +
        "or check SlicedIrRebuild's slice budget.");

    // Hand the measured IR to the page: the GPU worker must convolve with the
    // SAME impulse response, or its wet is a different reverb and the comparison
    // is meaningless.
    await page.evaluate((h) => { window.__ir = Float32Array.from(h); }, Array.from(wc.h));

    const oracle = convolve(noise, wc.h, N);
    const errC = relativeRmsError(wc.y, oracle, N);
    check("Run C — the CPU audio matches the float64 direct-convolution oracle",
          errC < TOL, `relative RMS error ${errC.toExponential(2)} (tolerance ${TOL})`);
    // The GPU worker DOES keep running under Engine=CPU — the plugin drives the ring
    // on every block whichever engine is selected, because push/pop are what advance
    // the block timeline the wets are stamped with (a ring that idled through a CPU
    // stretch would hand the next flip to GPU a pile of pre-flip audio). What must
    // be true is that its wet was not EMITTED, and that is exactly what the oracle
    // check above proves: at this point window.__ir has not been handed over, so the
    // worker is convolving with the unit impulse it was self-tested with, whose wet
    // is the DRY signal. Had any of it reached the output, errC would be enormous
    // rather than ~1e-7.
    const sc = runC.stats || {};
    report("Run C (diagnostic) — the GPU worker keeps turning under Engine=CPU",
           `produced=${sc.produced ?? "?"} queueSubmits=${sc.queueSubmits ?? "?"} ` +
           `— and none of it was emitted (the oracle check above)`);

    // ── RUN A: the backbone. GPU engine, CPU net KILLED.
    const runA = await page.evaluate(() => window.__run({ engine: 1, gpuOnly: true }));
    if (runA.error) throw new Error("Run A (GPU-only) failed: " + runA.error);
    const wa = align(runA.audio);
    check("Run A — the GPU-only engine produced audio (not silence)", aligned(wa),
          aligned(wa) ? `marker at ${wa.marker}, peak ${wa.peak.toFixed(3)}`
                      : `${wa.why} — with the CPU net dead, silence means no GPU block was ever produced`);
    if (!aligned(wa)) throw new Error("Run A produced no usable audio");

    const errA = relativeRmsError(wa.y, oracle, N);
    check("Run A — the GPU audio matches the float64 direct-convolution oracle",
          errA < TOL, `relative RMS error ${errA.toExponential(2)} (tolerance ${TOL})`);

    const sa = runA.stats || {};
    const produced = sa.produced || 0;
    const missed = sa.miss || 0;
    const total = produced + missed;
    report("HEADLINE — the GPU-produced share of a 3 s real-time run",
           `${produced}/${total} = ${total ? ((produced / total) * 100).toFixed(1) : "0.0"}% ` +
           `(${missed} blocks the CPU net would have covered, had it been alive)`);
    report("counters (diagnostic)",
           `queueSubmits=${sa.queueSubmits ?? "?"} mapResolves=${sa.mapResolves ?? "?"} ` +
           `expired=${sa.expired ?? "?"} · queueSubmits >= produced: ${(sa.queueSubmits || 0) >= produced}`);
    report("gpuNsLast (diagnostic — Chrome QUANTIZES; 0 is not evidence the GPU idled)",
           `${sa.gpuNsLast || 0} ns · timestamp-query=${runA.timestampQuery}`);
    report("block time (diagnostic)",
           `avg ${(sa.avgBlockUs || 0).toFixed(0)} µs of a ${((BLOCK / SAMPLE_RATE) * 1e6).toFixed(0)} µs budget ` +
           `(${(((sa.avgBlockUs || 0) / ((BLOCK / SAMPLE_RATE) * 1e6)) * 100).toFixed(1)}% of real time)`);

    // ── RUN B: THE TAMPER RUN.
    const runB = await page.evaluate(() => window.__run({ engine: 1, gpuOnly: true, tamper: true }));
    if (runB.error) throw new Error("Run B (tamper) failed: " + runB.error);
    const wb = align(runB.audio);
    check("Run B — the tampered WGSL still produced audio", aligned(wb),
          aligned(wb) ? `marker at ${wb.marker}, peak ${wb.peak.toFixed(3)}` : wb.why);
    if (!aligned(wb)) throw new Error("Run B produced no usable audio");

    const dev = maxScaledDeviation(wb.y, wa.y, 0.5, N);
    const level = rms(wa.y, N);
    const ratio = level > 0 ? rms(wb.y, N) / level : 0;
    const floor = 1e-3 * level;
    check("Run B — the audio is Run A x 0.5, SAMPLE-WISE " +
          "(the WGSL TEXT is causally upstream of the samples)",
          dev < floor && Math.abs(ratio - 0.5) < 0.02,
          `max |b - 0.5a| = ${dev.toExponential(2)} (floor ${floor.toExponential(2)}), ` +
          `rms ratio ${ratio.toFixed(4)}`);
    check("Run B — the audio DIFFERS from Run A (the edit was not a no-op)",
          relativeRmsError(wb.y, wa.y, N) > 0.1,
          `relative RMS difference ${relativeRmsError(wb.y, wa.y, N).toExponential(2)}`);

    // ── RUN D: the NEGATIVE path. The worker script 404s — the same SHAPE of
    //    failure as a browser with no WebGPU. The lane must fail with a NAMED
    //    reason, no ring may attach in the worklet (so the page renders no Engine
    //    toggle at all — the static half of that is asserted in gallery-smoke.mjs),
    //    and the CPU audio must still be correct.
    const runD = await page.evaluate(() => window.__run({ engine: 0, gpuOnly: false, blockWorker: true }));
    if (runD.error) throw new Error("Run D (blocked worker) failed: " + runD.error);
    check("Run D — a blocked worker fails the handshake with a NAMED reason",
          !!runD.reason && /worker|gpu|adapter|device|sab|isolat/i.test(runD.reason),
          `reason: ${runD.reason}`);
    check("Run D — no GPU ring is attached in the worklet", runD.gpuLane === false,
          `descriptor.gpuLane=${runD.gpuLane}`);
    const wd = align(runD.audio);
    check("Run D — the CPU convolver still plays, and still matches the oracle",
          aligned(wd) && relativeRmsError(wd.y, oracle, N) < TOL,
          aligned(wd) ? `relative RMS error ${relativeRmsError(wd.y, oracle, N).toExponential(2)}` : wd.why);
  }

  await page.evaluate((s) => { window.__result = { steps: s, pass: s.every((x) => x.pass) }; }, steps);
  if (screenshot) {
    await page.screenshot({ path: screenshot });
    console.log("  screenshot:", screenshot);
  }

  if (steps.some((s) => !s.pass)) throw new Error("one or more GPU-audio checks failed");
  if (skipped) console.log(`SKIP: webgpu-unavailable (${skipped}) — no proof was attempted`);
  else console.log("PASS: WGSL compute shaders on the browser's WebGPU device produced the audio");
} catch (e) {
  console.error("FAIL: " + (e && e.message ? e.message : e));
  process.exitCode = 1;
} finally {
  console.log("\n  result  step");
  for (const s of steps) {
    const tag = s.diagnostic ? ".." : s.pass ? "ok" : "FAIL";
    console.log(`  ${tag.padEnd(6)}  ${s.name}${s.detail ? " — " + s.detail : ""}`);
  }
  if (browser) await browser.close();
  server.close();
}
