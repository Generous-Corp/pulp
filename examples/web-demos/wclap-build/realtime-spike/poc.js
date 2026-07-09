// WS-C2 SPIKE — main-thread driver for the worklet-resident WebCLAP host.
//
// Flow (Architecture A):
//   1. main thread fetches + COMPILES the WebCLAP .wasm off the audio thread
//      (async WebAssembly.compile → a structured-cloneable WebAssembly.Module);
//   2. addModule() loads the classic worklet script;
//   3. postMessage the Module into the worklet, which creates the shared
//      WebAssembly.Memory and instantiates the CLAP plugin THERE;
//   4. the worklet renders one 128-frame quantum per process() call, in real
//      time, and posts back {ready, params, meter, paramsChanged, log};
//   5. moving a generated param slider posts a {param} message → latched as a
//      CLAP param event on the next quantum.
//
// This proves a WebCLAP plugin renders in REAL TIME through an AudioWorklet
// (not offline), exposes an AudioNode, and audibly changes output on a param
// change — the minimum a WebCLAP adapter needs to satisfy the shared player's
// contract. Requires cross-origin isolation (serve.mjs sends COOP/COEP/CORP).

const WASM_URL = "./PulpGain.wasm";
const WORKLET_URL = "./wclap-worklet.js";

const $ = (id) => document.getElementById(id);
const log = (msg, cls = "") => {
  const el = $("log"); const line = document.createElement("div");
  if (cls) line.className = cls; line.textContent = msg; el.appendChild(line);
  el.scrollTop = el.scrollHeight;
};
const fail = (m) => { log("FAIL: " + m, "err"); window.__pocError = m; };

let node, params = [];
const sliders = new Map();
// Measurement surface for headless validation.
window.__poc = { ready: false, meters: [], lastMeter: null, paramsChanged: [], startTime: 0 };

function buildControls() {
  const wrap = $("params"); wrap.textContent = "";
  for (const p of params) {
    const row = document.createElement("div"); row.className = "param";
    const label = document.createElement("label"); label.textContent = p.name;
    const range = document.createElement("input");
    range.type = "range"; range.min = String(p.min); range.max = String(p.max);
    range.step = "any"; range.value = String(p.default);
    const out = document.createElement("output");
    const fmt = () => { out.textContent = Number(range.value).toFixed(2); }; fmt();
    range.addEventListener("input", () => {
      fmt();
      node.port.postMessage({ type: "param", id: p.id, value: Number(range.value) });
    });
    row.append(label, range, out); wrap.appendChild(row); sliders.set(p.id, range);
  }
}

async function boot() {
  try {
    if (!self.crossOriginIsolated) {
      fail("page is not cross-origin isolated (COOP/COEP) — shared memory unavailable. Use serve.mjs.");
      return;
    }
    log("crossOriginIsolated = true", "ok");

    // 1. Fetch the module bytes on the main thread.
    //
    // NB (spike finding): posting a compiled WebAssembly.Module INTO an
    // AudioWorklet is silently dropped in Chrome — the message never arrives and
    // the sender does not throw. So we TRANSFER the raw bytes and compile inside
    // the worklet (sync `new WebAssembly.Module` is allowed in
    // AudioWorkletGlobalScope). See wclap-worklet.js::instantiateSync.
    log("fetching " + WASM_URL);
    const t0 = performance.now();
    const bytes = await (await fetch(WASM_URL)).arrayBuffer();
    log(`fetched ${(bytes.byteLength / 1e6).toFixed(2)} MB in ${(performance.now() - t0).toFixed(0)} ms`, "ok");

    // 2. Real-time AudioContext + worklet node.
    const ctx = new AudioContext({ sampleRate: 48000 });
    if (ctx.state === "suspended") await ctx.resume();
    await ctx.audioWorklet.addModule(WORKLET_URL);
    node = new AudioWorkletNode(ctx, "wclap-rt", {
      numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
      processorOptions: { useInternalTone: true, toneHz: 220, toneAmp: 0.3 },
    });
    window.__pocNode = node; // expose the AudioNode (adapter contract surface)
    node.onprocessorerror = (ev) => fail("processor error (audio thread): " +
      (ev && ev.message ? ev.message : "AudioWorkletProcessor threw — see worklet script"));

    node.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "ready") {
        params = m.params;
        $("title").textContent = `${m.descriptor.name} — WebCLAP (real-time worklet)`;
        $("subtitle").textContent = `id "${m.descriptor.id}", hosted INSIDE the AudioWorklet at ${m.sampleRate} Hz.`;
        buildControls();
        window.__poc.ready = true; window.__poc.descriptor = m.descriptor;
        window.__poc.params = m.params; window.__poc.startTime = performance.now();
        window.__poc.capabilities = m.capabilities;
        log(`ready — ${m.params.length} params, rendering real-time in the worklet`, "ok");
        const c = m.capabilities || {};
        log(`worklet caps: scope=${c.globalScope} Worker=${c.hasWorker} ` +
          `SAB=${c.hasSharedArrayBuffer} sharedMem=${c.memoryIsShared} ` +
          `thisPluginSpawns=${c.moduleImportsThreadSpawn}`, c.hasWorker ? "" : "ok");
      } else if (m.type === "meter") {
        window.__poc.lastMeter = m; window.__poc.meters.push(m);
        if (window.__poc.meters.length > 600) window.__poc.meters.shift(); // bound memory
        const dB = 20 * Math.log10((m.outRms || 1e-9) / (m.inRms || 1e-9));
        $("meter").textContent = `quanta=${m.quanta}  ctxTime=${m.currentTime.toFixed(2)}s  ` +
          `in ${m.inRms.toFixed(3)} → out ${m.outRms.toFixed(3)}  (${dB >= 0 ? "+" : ""}${dB.toFixed(2)} dB)`;
        drawScope(m);
      } else if (m.type === "paramsChanged") {
        window.__poc.paramsChanged.push(...m.changes);
      } else if (m.type === "log") {
        log(m.text);
      } else if (m.type === "error") {
        fail(m.message);
      }
    };

    // 3. Route through a silent sink so it renders real-time without blasting
    //    audio in headless CI (a real demo would connect to ctx.destination).
    const sink = ctx.createGain(); sink.gain.value = 0.0001;
    node.connect(sink).connect(ctx.destination);
    window.__pocCtx = ctx;

    // Optional: a button to actually hear it.
    $("play").addEventListener("click", () => { sink.gain.value = 0.5; log("output un-muted", "ok"); });

    // 4. Transfer the bytes into the worklet to compile + instantiate + activate.
    node.port.postMessage({ type: "load", bytes }, [bytes]);
    log("transferred module bytes to worklet; instantiating there…");
  } catch (e) {
    fail(String(e && e.stack ? e.stack : e));
  }
}

function drawScope(m) {
  const c = $("scope"); if (!c) return; const ctx = c.getContext("2d");
  const { width: w, height: h } = c; ctx.clearRect(0, 0, w, h);
  const barW = 4, gap = 1; const bars = Math.floor(w / (barW + gap));
  const data = window.__poc.meters.slice(-bars);
  ctx.fillStyle = "#5b8cff";
  data.forEach((d, i) => { const v = Math.min(1, d.outRms * 2); const bh = v * (h - 4);
    ctx.fillRect(i * (barW + gap), h - bh, barW, bh); });
}

boot();
