// In-browser WebCLAP host for PulpGain.
//
// Loads the WebCLAP `.wasm` and drives it through the real CLAP lifecycle with
// WebClapHost — the SAME engine the Node runner uses, unchanged — entirely on
// the browser main thread. The host vtable is synthesized with trampoline
// modules, so this needs no `WebAssembly.Function` and no browser flag. The
// page builds a generated control per plugin parameter and renders/plays audio
// through the plugin.
//
// Audio model: the host renders a block through `plugin.process()` on the main
// thread and plays the result via an AudioBuffer. Real-time AudioWorklet
// streaming (host on a worker, worklet draining a SharedArrayBuffer ring) is a
// later refinement; this slice proves the module is hosted and audible in the
// browser with a working generated UI.
//
// Requires cross-origin isolation (COOP/COEP) so the module's shared
// `WebAssembly.Memory` is allowed — serve via serve.mjs, which sets the headers.
import { WebClapHost } from "/core/format/src/wasm/wclap-host.mjs";

const WASM_URL = "/examples/web-demos/wclap-build/build/PulpGain.wasm";
const SR = 48000;
const BLOCK = 512;
const RENDER_FRAMES = SR; // 1 second
const TONE_HZ = 220;

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const log = (msg, cls = "") => {
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = msg;
  logEl.appendChild(line);
};
const fail = (msg) => {
  log("FAIL: " + msg, "err");
  window.__wclapError = msg;
};

let host, plugin, params;
const sliders = new Map(); // param id -> input element

function buildControls() {
  const wrap = $("params");
  wrap.textContent = "";
  for (const p of params) {
    const row = document.createElement("div");
    row.className = "param";
    const label = document.createElement("label");
    label.textContent = p.name;
    const range = document.createElement("input");
    range.type = "range";
    range.min = String(p.min);
    range.max = String(p.max);
    range.step = "any"; // continuous — exact parameter values, no step rounding
    range.value = String(p.default);
    const out = document.createElement("output");
    const fmt = () => { out.textContent = Number(range.value).toFixed(2); };
    fmt();
    range.addEventListener("input", () => {
      fmt();
      try { renderAndPlay(false); } catch (e) { fail(String(e && e.stack ? e.stack : e)); }
    });
    row.append(label, range, out);
    wrap.appendChild(row);
    sliders.set(p.id, range);
  }
}

// Build a stereo test tone, process it through the plugin with the current
// slider values injected as parameter events, and return {input, output, rms}.
function renderBlockChain() {
  const inL = new Float32Array(RENDER_FRAMES);
  const inR = new Float32Array(RENDER_FRAMES);
  for (let i = 0; i < RENDER_FRAMES; i++) {
    inL[i] = inR[i] = 0.3 * Math.sin((2 * Math.PI * TONE_HZ * i) / SR);
  }
  const paramEvents = params
    .map((p) => ({ id: p.id, value: Number(sliders.get(p.id).value) }))
    .filter((e, i) => e.value !== params[i].default);

  const outL = new Float32Array(RENDER_FRAMES);
  const outR = new Float32Array(RENDER_FRAMES);
  for (let off = 0; off < RENDER_FRAMES; off += BLOCK) {
    const n = Math.min(BLOCK, RENDER_FRAMES - off);
    const blk = [inL.subarray(off, off + n), inR.subarray(off, off + n)];
    // Inject the parameter events on the first block only (they latch).
    const ev = off === 0 ? paramEvents : [];
    const out = plugin.process(blk, n, { paramEvents: ev });
    outL.set(out[0].subarray(0, n), off);
    outR.set(out[1].subarray(0, n), off);
  }
  const rms = (a) => Math.sqrt(a.reduce((s, x) => s + x * x, 0) / a.length);
  return { inL, outL, outR, inRms: rms(inL), outRms: rms(outL) };
}

function drawScope(samples) {
  const c = $("scope");
  const ctx = c.getContext("2d");
  const { width: w, height: h } = c;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = "#5b8cff";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  const step = Math.max(1, Math.floor(samples.length / w));
  for (let x = 0; x < w; x++) {
    const s = samples[Math.min(samples.length - 1, x * step)] || 0;
    const y = h / 2 - s * (h / 2) * 0.9;
    x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.stroke();
}

let audioCtx;
async function renderAndPlay(play) {
  const { outL, outR, inRms, outRms } = renderBlockChain();
  drawScope(outL);
  const deltaDb = 20 * Math.log10((outRms || 1e-9) / (inRms || 1e-9));
  $("meter").textContent = `in ${inRms.toFixed(3)}  →  out ${outRms.toFixed(3)}  (${deltaDb >= 0 ? "+" : ""}${deltaDb.toFixed(2)} dB)`;
  // Expose for headless validation.
  window.__wclapLast = { inRms, outRms, deltaDb,
    params: params.map((p) => ({ id: p.id, name: p.name, value: Number(sliders.get(p.id).value) })) };

  if (play) {
    audioCtx ??= new AudioContext({ sampleRate: SR });
    if (audioCtx.state === "suspended") await audioCtx.resume();
    const buf = audioCtx.createBuffer(2, RENDER_FRAMES, SR);
    buf.copyToChannel(outL, 0);
    buf.copyToChannel(outR, 1);
    const src = audioCtx.createBufferSource();
    src.buffer = buf;
    src.connect(audioCtx.destination);
    src.start();
  }
}

async function boot() {
  try {
    if (!self.crossOriginIsolated) {
      fail("page is not cross-origin isolated (COOP/COEP) — shared memory unavailable. Use serve.mjs.");
      return;
    }
    log("fetching " + WASM_URL);
    const bytes = await (await fetch(WASM_URL)).arrayBuffer();
    host = new WebClapHost({ onLog: (fd, t) => log(t.replace(/\n$/, "")) });
    await host.instantiate(bytes);
    plugin = host.createPlugin(0);
    $("title").textContent = `${plugin.descriptor.name} — WCLAP`;
    $("subtitle").textContent = `id "${plugin.descriptor.id}", hosted entirely in the browser.`;
    plugin.init();
    plugin.activate(SR, 1, BLOCK);
    params = plugin.params();
    log(`activated; ${params.length} parameters`, "ok");
    buildControls();
    renderAndPlay(false); // initial offline render (no user gesture needed)
    $("render").disabled = false;
    $("render").addEventListener("click", () => renderAndPlay(true));
    window.__wclapReady = true;
    log("ready — WCLAP plugin is hosted in the browser.", "ok");
  } catch (e) {
    fail(String(e && e.stack ? e.stack : e));
  }
}

boot();
