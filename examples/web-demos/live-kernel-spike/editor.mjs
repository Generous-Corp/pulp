// Pulp Live — M1 — the live editor controller.
//
// The whole point: there is NO compile step, so every number in the source is a
// live control on a playing instrument. Drag `cutoff: 700hz` while a riff plays
// and the filter sweeps under your finger — the edit re-parses, diffs against the
// running graph, and lands as a set_param on the resident kernel in ~3 ms, with a
// badge proving it. Value edits are instant (set_param); structural edits (new
// node, saw→sine) crossfade click-free on idle. All inside ONE ~39 KB single-
// thread wasm worklet — no server, no COOP/COEP, no copyleft.
//
// Paths are RELATIVE so this runs identically under the repo-root dev server and
// as a FLAT static deploy (GitHub Pages / Cloudflare Pages, no headers).

import { parsePatch, encodeLKB0, diffGraphs, PARAM_META, VERBS, convertUnit, EXAMPLES } from "./lk-dsl.mjs";

const WASM_URL = "./dist/lk_kernel.wasm";
const WORKLET_URL = "./lk-worklet.js";
const FADE_MS = 40;          // equal-power structural crossfade
const STRUCT_IDLE_MS = 300;  // apply structural edits after typing settles
const $ = (id) => document.getElementById(id);
const hzOf = (m) => 440 * Math.pow(2, (m - 69) / 12);
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

// ── state (+ the headless-test surface) ──────────────────────────────────────
const state = {
  ready: false, error: null,
  coi: (typeof crossOriginIsolated !== "undefined") ? crossOriginIsolated : "n/a",
  hasSAB: (typeof SharedArrayBuffer !== "undefined"),
  lastMeter: null, lastLatencyMs: null, lastKind: null, lastParseMs: null,
  parseOk: true, parseErrors: [], patchBytes: 0, kernelBytes: 0,
  latencies: [], hist: [],
};
window.__lkm = state;

let ctx, node, analyser, tdBuf;
let lastApplied = null;      // graph currently in the kernel
let pendingStructural = null;
let structTimer = null;
let editSeq = 0;
const pendingEdits = new Map();
let numberTokens = [];       // [{start,len,value,unit,verb,key}] for scrubbing
let activeBindings = [];     // note.* bindings of the running patch
const heldKeys = [];         // mono legato stack (last = sounding)
let lastPitch = 45;          // A2
let riffTimer = null, riffStep = 0;
const RIFF = [45, 48, 52, 55, 52, 48, 50, 47];

function post(msg, transfer) { node.port.postMessage(msg, transfer || []); }

// ── the edit pipeline ────────────────────────────────────────────────────────
function applyEdit(force) {
  const text = $("editor").value;
  const t0 = performance.now();
  const res = parsePatch(text);
  state.lastParseMs = performance.now() - t0;
  renderHighlight(text, res.errors);
  if (!res.ok) { state.parseOk = false; state.parseErrors = res.errors; showErrors(res.errors); paintReceipts(); return null; }
  state.parseOk = true; state.parseErrors = []; showErrors([]);
  const g = res.graph;
  const blob = encodeLKB0(g);
  state.patchBytes = blob.length;
  renderBytes(blob, g);

  if (!lastApplied) { hardLoad(g, blob); paintReceipts(); return { kind: "load", editId: null }; }
  const d = diffGraphs(lastApplied, g);

  if (d.kind === "structural") {
    const editId = ++editSeq;
    g.__editId = editId;
    pendingStructural = g;
    clearTimeout(structTimer);
    if (force) applyStructural();
    else structTimer = setTimeout(applyStructural, STRUCT_IDLE_MS);
    paintReceipts();
    return { kind: "structural", editId };
  }
  if (d.kind === "none") { lastApplied = g; activeBindings = g.bindings; paintReceipts(); return { kind: "none", editId: null }; }

  // value edit(s): instant set_param batch, last tagged for the timing round-trip
  const editId = ++editSeq;
  pendingEdits.set(editId, { sendPerf: performance.now(), kind: "param" });
  d.paramEdits.forEach((p, i) => post({ type: "param", node: p.node, paramId: p.paramId, value: p.value, editId: i === d.paramEdits.length - 1 ? editId : undefined }));
  lastApplied = g; activeBindings = g.bindings;
  paintReceipts();
  return { kind: "param", editId };
}

function applyStructural() {
  clearTimeout(structTimer);
  const g = pendingStructural; if (!g) return;
  // Never stack fades: a load_plan during an in-flight fade is refused by the
  // kernel (busy). Hold the latest pending blob and apply it when the fade ends
  // (design §1.4 — queue latest, never drop the user's newest edit).
  if (state.lastMeter && state.lastMeter.fading) { structTimer = setTimeout(applyStructural, 50); return; }
  pendingStructural = null;
  const blob = encodeLKB0(g);
  const editId = g.__editId || ++editSeq;
  pendingEdits.set(editId, { sendPerf: performance.now(), kind: "structural" });
  post({ type: "structuralEdit", bytes: blob.buffer, fadeMs: FADE_MS, editId }, [blob.buffer]);
  lastApplied = g; activeBindings = g.bindings;
  setTimeout(sendVoice, FADE_MS + 25); // keep a latched note sounding across the morph
}

function hardLoad(g, blob) {
  post({ type: "structuralEdit", bytes: blob.buffer, fadeMs: 0, editId: 0 }, [blob.buffer]);
  lastApplied = g; activeBindings = g.bindings;
  setTimeout(sendVoice, 25);
}

function onApplied(m) {
  const p = pendingEdits.get(m.editId); if (!p) return;
  const ms = performance.now() - p.sendPerf;
  pendingEdits.delete(m.editId);
  state.lastLatencyMs = ms; state.lastKind = p.kind;
  state.latencies.push({ ms, kind: p.kind });
  state.hist.push(ms); if (state.hist.length > 24) state.hist.shift();
  paintLatency(ms, p.kind); paintReceipts();
}

// ── voice: mono legato, note.* bindings evaluated main-thread ─────────────────
function sendVoice() {
  if (!lastApplied) return;
  const gate = heldKeys.length ? 1 : 0;
  if (heldKeys.length) lastPitch = heldKeys[heldKeys.length - 1];
  const hz = hzOf(lastPitch);
  const sets = [];
  for (const b of activeBindings) {
    if (b.kind === "hz") sets.push([b.node, b.paramId, hz * (b.mul || 1) + (b.add || 0)]);
    else if (b.kind === "gate") sets.push([b.node, b.paramId, gate]);
  }
  if (sets.length) post({ type: "params", sets });
  paintKeys();
}
function noteOn(m) { resume(); if (!heldKeys.includes(m)) heldKeys.push(m); sendVoice(); }
function noteOff(m) { const i = heldKeys.indexOf(m); if (i >= 0) heldKeys.splice(i, 1); sendVoice(); }
const LATCH_MIDI = 45;
function setLatch(on) {
  const has = heldKeys.includes(LATCH_MIDI);
  if (on && !has) heldKeys.unshift(LATCH_MIDI);
  else if (!on && has) heldKeys.splice(heldKeys.indexOf(LATCH_MIDI), 1);
  $("latchBtn")?.classList.toggle("on", on);
  sendVoice();
}
function setRiff(on) {
  $("riffBtn")?.classList.toggle("on", on);
  clearInterval(riffTimer); riffTimer = null;
  if (!on) { noteOff(RIFF[(riffStep + RIFF.length - 1) % RIFF.length] + 12); return; }
  resume();
  let prev = null;
  riffTimer = setInterval(() => {
    if (prev != null) noteOff(prev);
    const n = RIFF[riffStep % RIFF.length] + 12; riffStep++;
    noteOn(n); prev = n;
  }, 150);
}

// ── syntax highlight overlay (also drives scrubbing + squiggles) ─────────────
const NUM_RE = /[+-]?(?:\d+\.?\d*|\.\d+)(?:khz|hz|ms|s|db|ct)?/y;
function renderHighlight(text, errors) {
  const errLines = new Set((errors || []).map((e) => e.line));
  const infoLines = new Set((errors || []).filter((e) => e.level === "info" || e.level === "warn").map((e) => e.line));
  numberTokens = [];
  const lines = text.split("\n");
  let offset = 0, nid = 0, html = "";
  for (let li = 0; li < lines.length; li++) {
    const line = lines[li];
    const verbM = line.match(/=\s*([a-zA-Z]\w*)\s*\(/);
    const lineVerb = verbM ? verbM[1].toLowerCase() : (/\*\s*[+-]?[\d.]+db/.test(line) ? "gain" : null);
    let cls = "";
    if (errLines.has(li + 1)) cls = " errline";
    else if (infoLines.has(li + 1)) cls = " infoline";
    html += `<div class="ln${cls}">` + tokenizeLine(line, offset, () => nid++, lineVerb) + "</div>";
    offset += line.length + 1;
  }
  const ov = $("overlay"); if (ov) ov.innerHTML = html || "<div class='ln'></div>";
}

function tokenizeLine(line, baseOffset, nextId, lineVerb) {
  let out = "", i = 0, currentKey = null;
  const esc = (s) => s.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
  while (i < line.length) {
    const rest = line.slice(i);
    if (rest[0] === "#") { out += `<span class="c-com">${esc(rest)}</span>`; break; }
    if (/\s/.test(rest[0])) { const m = rest.match(/^\s+/)[0]; out += esc(m); i += m.length; continue; }
    // note.hz / note.gate
    const nm = rest.match(/^note\.\w+/);
    if (nm) { out += `<span class="c-note">${esc(nm[0])}</span>`; i += nm[0].length; continue; }
    // number (+ unit)
    NUM_RE.lastIndex = 0;
    const num = NUM_RE.exec(rest);
    if (num && num.index === 0 && /[\d.]/.test(num[0].replace(/^[+-]/, "")[0] || "")) {
      const txt = num[0];
      const unitM = txt.match(/(khz|hz|ms|s|db|ct)$/i);
      const unit = unitM ? unitM[1].toLowerCase() : "";
      const val = parseFloat(txt);
      const id = nextId();
      const prevCh = out.replace(/<[^>]*>/g, "").trimEnd().slice(-1);
      const noteMul = (prevCh === "*" || prevCh === "+");
      numberTokens[id] = { start: baseOffset + i, len: txt.length, value: val, unit, verb: lineVerb, key: currentKey, noteMul };
      out += `<span class="c-num" data-nid="${id}">${esc(txt)}</span>`;
      i += txt.length; continue;
    }
    // identifier / verb / key
    const idm = rest.match(/^[a-zA-Z_]\w*/);
    if (idm) {
      const word = idm[0]; const after = line.slice(i + word.length).match(/^\s*(.)/);
      const nxt = after ? after[1] : "";
      let klass = "c-id";
      if (nxt === "(") klass = "c-verb";
      else if (nxt === ":") { klass = "c-key"; currentKey = word.toLowerCase(); }
      else if (word === "out") klass = "c-out";
      out += `<span class="${klass}">${esc(word)}</span>`;
      i += word.length; continue;
    }
    if (rest[0] === ",") currentKey = null;
    if (rest[0] === "~") { out += `<span class="c-fb">~</span>`; i++; continue; }
    out += `<span class="c-punct">${esc(rest[0])}</span>`; i++;
  }
  return out || "&nbsp;";
}

// ── scrubbing: drag a number, hear it ────────────────────────────────────────
let scrub = null;
function numMeta(tok) {
  if (tok.noteMul) return { scale: "lin", step: 0.0008, min: 0.001, max: 64, unit: tok.unit };
  const verb = VERBS[tok.verb]; const type = verb ? verb.type : null;
  const meta = (type != null && tok.key && PARAM_META[type] && PARAM_META[type][tok.key]) ? PARAM_META[type][tok.key] : null;
  if (meta) return meta;
  if (tok.unit === "db") return { scale: "lin", step: 0.1, min: -60, max: 24, unit: "db" };
  return { scale: "lin", step: 0.005, min: -1e6, max: 1e6, unit: tok.unit };
}
function toDisplay(canonical, unit) {
  if (unit === "khz") return canonical / 1000;
  if (unit === "ms") return canonical * 1000;
  return canonical; // hz, s, db, unitless, ct
}
function fmtNum(n, unit) {
  let dec = 3;
  if (unit === "khz") dec = 3; else if (unit === "hz") dec = n >= 100 ? 0 : 1;
  else if (unit === "ms") dec = n >= 10 ? 0 : 1; else if (unit === "s") dec = 3;
  else if (unit === "db") dec = 1; else dec = Math.abs(n) >= 1 ? 3 : 3;
  let s = n.toFixed(dec).replace(/\.?0+$/, (m) => (m.includes(".") ? "" : m));
  if (unit === "db" && n >= 0) s = "+" + s;
  return s + unit;
}
function hoveredNumberAt(clientX, clientY) {
  const spans = $("overlay").querySelectorAll(".c-num");
  for (const s of spans) {
    const r = s.getBoundingClientRect();
    if (clientX >= r.left - 2 && clientX <= r.right + 2 && clientY >= r.top - 3 && clientY <= r.bottom + 3)
      return +s.dataset.nid;
  }
  return -1;
}
function beginScrub(nid, clientX, fine) {
  const tok = numberTokens[nid]; if (!tok) return;
  const meta = numMeta(tok);
  const canonical = convertUnit(tok.value, tok.unit);
  scrub = { nid, startX: clientX, canon0: canonical, meta, unit: tok.unit, fine };
  document.body.classList.add("scrubbing");
}
function moveScrub(clientX, shift) {
  if (!scrub) return;
  const dx = clientX - scrub.startX; const f = shift ? 0.25 : 1;
  let c = scrub.canon0;
  if (scrub.meta.scale === "log") c = Math.max(scrub.meta.min > 0 ? scrub.meta.min : 1e-4, scrub.canon0) * Math.exp(dx * 0.006 * f);
  else c = scrub.canon0 + dx * (scrub.meta.step || 0.005) * f;
  c = clamp(c, scrub.meta.min, scrub.meta.max);
  const disp = toDisplay(c, scrub.unit);
  const newText = fmtNum(disp, scrub.unit);
  // splice into the textarea, preserving the number's start offset
  const ta = $("editor"); const tok = numberTokens[scrub.nid];
  const v = ta.value;
  ta.value = v.slice(0, tok.start) + newText + v.slice(tok.start + tok.len);
  applyEdit(false); // re-parse re-tokenizes; same ordinal id maps back to this number
}
function endScrub() { scrub = null; document.body.classList.remove("scrubbing"); }

// ── receipts / readouts ──────────────────────────────────────────────────────
const SPARK = "▁▂▃▄▅▆▇█";
function sparkline(a) {
  if (!a.length) return "";
  const mx = Math.max(...a, 6);
  return a.map((v) => SPARK[Math.min(SPARK.length - 1, Math.floor(v / mx * (SPARK.length - 1)))]).join("");
}
function paintLatency(ms, kind) {
  const el = $("latency"); if (!el) return;
  el.textContent = `→ ${ms.toFixed(1)} ms`;
  const b = $("kind"); if (b) { b.textContent = kind === "structural" ? "structural · crossfade" : "param · instant"; b.className = "kind " + (kind === "structural" ? "struct" : "param"); }
  el.classList.remove("flash"); void el.offsetWidth; el.classList.add("flash");
}
let uptimeT0 = null;
function paintReceipts() {
  const set = (id, v) => { const e = $(id); if (e) e.textContent = v; };
  set("rc-parse", `${(state.lastParseMs ?? 0).toFixed(2)} ms`);
  set("rc-build", "~1.2 µs");         // S0-measured (worklet scope has no perf.now)
  set("rc-fade", `${FADE_MS} ms`);
  set("rc-kernel", `${(state.kernelBytes / 1024).toFixed(0)} KB`);
  set("rc-patch", `${state.patchBytes} B`);
  set("rc-spark", sparkline(state.hist));
  if (state.lastMeter && uptimeT0 != null) {
    const s = Math.max(0, state.lastMeter.currentTime - uptimeT0);
    set("rc-uptime", `${Math.floor(s / 60)}m ${String(Math.floor(s % 60)).padStart(2, "0")}s`);
  }
}
function showErrors(errors) {
  const el = $("errors"); if (!el) return;
  const real = (errors || []).filter((e) => e.level === "error");
  const soft = (errors || []).filter((e) => e.level !== "error");
  if (!errors || !errors.length) { el.style.display = "none"; el.innerHTML = ""; return; }
  el.style.display = "block";
  el.className = real.length ? "err" : "warn";
  el.innerHTML = [...real, ...soft].map((e) => `<div>line ${e.line}: ${esc(e.msg)}</div>`).join("");
}
const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));

function renderBytes(blob, g) {
  const el = $("bytes"); if (!el) return;
  const nN = g.nodes.length, nE = g.edges.length, nP = g.nodes.reduce((s, n) => s + n.params.length, 0);
  const hdrEnd = 12, nEnd = hdrEnd + nN * 4, eEnd = nEnd + nE * 8;
  let html = "";
  for (let i = 0; i < blob.length; i++) {
    const cls = i < hdrEnd ? "bh" : i < nEnd ? "bn" : i < eEnd ? "be" : "bp";
    html += `<span class="${cls}">${blob[i].toString(16).padStart(2, "0")}</span>`;
    if ((i + 1) % 16 === 0) html += "<br>";
  }
  el.innerHTML = html;
  $("bytesLegend").innerHTML =
    `<b>${blob.length} B</b> = <span class="bh">12 header</span> + <span class="bn">${nN}·4 nodes</span> + <span class="be">${nE}·8 edges</span> + <span class="bp">${nP}·8 params</span>`;
  const tab = document.querySelector('[data-tab="bytes"]'); if (tab) tab.textContent = `BYTES ${blob.length} B`;
}

// ── keyboard widget ──────────────────────────────────────────────────────────
const KEY_MAP = { a: 57, w: 58, s: 59, e: 60, d: 61, f: 62, t: 63, g: 64, y: 65, h: 66, u: 67, j: 68, k: 69 };
let octave = 0;
const WHITE = [57, 59, 60, 62, 64, 65, 67, 69, 71, 72];
const BLACK = { 58: 0, 61: 2, 63: 3, 66: 5, 68: 6, 70: 7 };
function buildKeyboard() {
  const kb = $("keyboard"); if (!kb) return; kb.innerHTML = "";
  const w = 100 / WHITE.length;
  WHITE.forEach((m, i) => { const k = document.createElement("div"); k.className = "wkey"; k.dataset.midi = m; k.style.left = `${i * w}%`; k.style.width = `${w}%`; kb.appendChild(k); });
  Object.keys(BLACK).forEach((mm) => { const m = +mm, idx = BLACK[mm]; const k = document.createElement("div"); k.className = "bkey"; k.dataset.midi = m; k.style.left = `calc(${(idx + 1) * w}% - ${w * 0.3}%)`; k.style.width = `${w * 0.6}%`; kb.appendChild(k); });
  kb.querySelectorAll(".wkey,.bkey").forEach((k) => {
    const m = +k.dataset.midi;
    k.addEventListener("pointerdown", (e) => { e.preventDefault(); noteOn(m + octave * 12); });
    k.addEventListener("pointerup", () => noteOff(m + octave * 12));
    k.addEventListener("pointerleave", (e) => { if (e.buttons) noteOff(m + octave * 12); });
  });
}
function paintKeys() {
  document.querySelectorAll("#keyboard .wkey,#keyboard .bkey").forEach((k) => k.classList.toggle("down", heldKeys.includes(+k.dataset.midi + octave * 12)));
}

// ── boot ─────────────────────────────────────────────────────────────────────
function resume() { if (ctx && ctx.state === "suspended") ctx.resume(); }
async function boot() {
  try {
    const coiEl = $("coi");
    if (coiEl) coiEl.innerHTML =
      `<b>crossOriginIsolated = ${String(state.coi)}</b> · SharedArrayBuffer ${state.hasSAB ? "present" : "absent"} · ` +
      `single-thread wasm — <b>no COOP/COEP required</b>, runs on plain static hosting.`;
    ctx = new AudioContext();
    try { await ctx.resume(); } catch {}
    await ctx.audioWorklet.addModule(WORKLET_URL);
    node = new AudioWorkletNode(ctx, "lk-processor", { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
    analyser = ctx.createAnalyser(); analyser.fftSize = 2048; analyser.smoothingTimeConstant = 0;
    tdBuf = new Float32Array(analyser.fftSize);
    node.connect(ctx.destination); node.connect(analyser);
    node.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "ready") { applyEdit(true); state.ready = true; setLatch(true); drawScope(); }
      else if (m.type === "meter") { state.lastMeter = m; if (uptimeT0 == null) uptimeT0 = m.currentTime; paintReceipts(); const st = $("status"); if (st) st.textContent = `${(m.peak || 0).toFixed(3)} peak · ${m.fading ? "fading" : "live"}`; }
      else if (m.type === "applied") onApplied(m);
      else if (m.type === "error") { state.error = m.message; console.error("worklet:", m.message); }
    };
    const bytes = await (await fetch(WASM_URL)).arrayBuffer();
    state.kernelBytes = bytes.byteLength; paintReceipts();
    post({ type: "wasm", bytes }, [bytes]);
  } catch (err) { state.error = String(err && err.stack || err); const st = $("status"); if (st) st.textContent = "error: " + state.error; console.error(err); }
}

function drawScope() {
  const cv = $("scope"); if (!cv) return; const g = cv.getContext("2d");
  const draw = () => {
    requestAnimationFrame(draw); if (!analyser) return;
    analyser.getFloatTimeDomainData(tdBuf);
    const w = cv.width, h = cv.height; g.clearRect(0, 0, w, h);
    g.strokeStyle = "#4fd1c5"; g.lineWidth = 2; g.beginPath();
    for (let i = 0; i < w; i++) { const s = tdBuf[Math.floor(i / w * tdBuf.length)]; const y = h / 2 - s * h * 0.46; i ? g.lineTo(i, y) : g.moveTo(i, y); }
    g.stroke();
  };
  draw();
}

// ── headless-test API ────────────────────────────────────────────────────────
state.setPatchText = (text) => new Promise((resolve) => {
  $("editor").value = text; const r = applyEdit(true);
  if (!r || r.editId == null) return resolve({ kind: r ? r.kind : "error", latencyMs: 0 });
  waitApplied(r.editId, r.kind, resolve);
});
// Simulate a scrub: replace `key: <num>` (or a `* Ndb` gain) with a new display
// value, apply, and resolve with the measured edit->sound latency.
state.editNumber = (key, newDisplay, unit) => new Promise((resolve) => {
  const ta = $("editor");
  const re = new RegExp(`(${key}\\s*:\\s*)[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(khz|hz|ms|s|db|ct)?`);
  const m = ta.value.match(re);
  const u = unit != null ? unit : (m && m[2] ? m[2] : "");
  ta.value = ta.value.replace(re, `$1${newDisplay}${u}`);
  const r = applyEdit(true);
  if (!r || r.editId == null) return resolve({ kind: r ? r.kind : "none", latencyMs: 0 });
  waitApplied(r.editId, r.kind, resolve);
});
function waitApplied(id, kind, resolve) {
  const t0 = performance.now();
  const iv = setInterval(() => {
    if (!pendingEdits.has(id)) { clearInterval(iv); resolve({ kind, latencyMs: state.lastLatencyMs }); }
    else if (performance.now() - t0 > 1200) { clearInterval(iv); resolve({ kind, latencyMs: -1 }); }
  }, 3);
}
state.latch = (on) => { resume(); setLatch(!!on); };
state.riff = (on) => setRiff(!!on);
state.noteOn = noteOn; state.noteOff = noteOff;
state.peak = () => { if (!analyser) return 0; analyser.getFloatTimeDomainData(tdBuf); let p = 0; for (const v of tdBuf) { const a = v < 0 ? -v : v; if (a > p) p = a; } return p; };
state.captureClick = async (ms) => {
  let mx = 0; const t0 = performance.now();
  while (performance.now() - t0 < ms) { analyser.getFloatTimeDomainData(tdBuf); for (let i = 1; i < tdBuf.length - 1; i++) { const l = Math.abs(tdBuf[i - 1] - 2 * tdBuf[i] + tdBuf[i + 1]); if (l > mx) mx = l; } await new Promise((r) => setTimeout(r, 2)); }
  return mx > 0 ? 20 * Math.log10(mx) : -120;
};

// ── permalink ────────────────────────────────────────────────────────────────
function encodeShare(text) { return "#p=" + btoa(unescape(encodeURIComponent(text))).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); }
function decodeShare() {
  const m = location.hash.match(/[#&]p=([^&]+)/); if (!m) return null;
  try { return decodeURIComponent(escape(atob(m[1].replace(/-/g, "+").replace(/_/g, "/")))); } catch { return null; }
}

// ── wire up ──────────────────────────────────────────────────────────────────
function init() {
  const exWrap = $("presets");
  if (exWrap) Object.keys(EXAMPLES).forEach((name) => { const b = document.createElement("button"); b.className = "chip"; b.textContent = name; b.addEventListener("click", () => { $("editor").value = EXAMPLES[name]; applyEdit(true); }); exWrap.appendChild(b); });

  const shared = decodeShare();
  $("editor").value = shared || EXAMPLES.WarmKeys;

  const ta = $("editor"), ov = $("overlay");
  renderHighlight(ta.value, []);
  const syncScroll = () => { ov.scrollTop = ta.scrollTop; ov.scrollLeft = ta.scrollLeft; };
  ta.addEventListener("scroll", syncScroll);

  let liveTimer = null;
  ta.addEventListener("input", () => { resume(); renderHighlight(ta.value, state.parseErrors); clearTimeout(liveTimer); liveTimer = setTimeout(() => applyEdit(false), 16); });
  ta.addEventListener("keydown", (e) => { if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); applyEdit(true); } });

  // scrub interaction on the editor surface
  ta.addEventListener("pointermove", (e) => {
    if (scrub) return;
    const nid = hoveredNumberAt(e.clientX, e.clientY);
    ta.style.cursor = nid >= 0 ? "ew-resize" : "";
  });
  ta.addEventListener("pointerdown", (e) => {
    const nid = hoveredNumberAt(e.clientX, e.clientY);
    if (nid < 0) return;
    e.preventDefault(); ta.setPointerCapture(e.pointerId);
    resume(); beginScrub(nid, e.clientX, e.shiftKey);
  });
  ta.addEventListener("pointermove", (e) => { if (scrub) { e.preventDefault(); moveScrub(e.clientX, e.shiftKey); } });
  ta.addEventListener("pointerup", (e) => { if (scrub) { ta.releasePointerCapture(e.pointerId); endScrub(); } });
  ta.addEventListener("pointercancel", () => { if (scrub) endScrub(); });

  // transport
  $("latchBtn")?.addEventListener("click", () => setLatch(!heldKeys.includes(LATCH_MIDI)));
  $("riffBtn")?.addEventListener("click", () => setRiff(!riffTimer));
  $("octDown")?.addEventListener("click", () => { octave = Math.max(-2, octave - 1); paintOct(); });
  $("octUp")?.addEventListener("click", () => { octave = Math.min(2, octave + 1); paintOct(); });
  $("shareBtn")?.addEventListener("click", async () => {
    const url = location.origin + location.pathname + encodeShare($("editor").value);
    history.replaceState(null, "", url);
    try { await navigator.clipboard.writeText(url); toast("Link copied — the patch travels in the URL"); } catch { toast("Copy this URL from the address bar"); }
  });

  // tabs
  document.querySelectorAll(".tab").forEach((t) => t.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((x) => x.classList.remove("on"));
    document.querySelectorAll(".pane").forEach((x) => x.classList.remove("on"));
    t.classList.add("on"); $("pane-" + t.dataset.tab)?.classList.add("on");
  }));

  // computer keyboard
  window.addEventListener("keydown", (e) => {
    if (e.target && (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT")) return;
    if (e.repeat) return; const k = e.key.toLowerCase();
    if (k === "z") { octave = Math.max(-2, octave - 1); paintOct(); return; }
    if (k === "x") { octave = Math.min(2, octave + 1); paintOct(); return; }
    if (k === " ") { e.preventDefault(); setLatch(!heldKeys.includes(LATCH_MIDI)); return; }
    if (k in KEY_MAP) noteOn(KEY_MAP[k] + octave * 12);
  });
  window.addEventListener("keyup", (e) => { const k = e.key.toLowerCase(); if (k in KEY_MAP) noteOff(KEY_MAP[k] + octave * 12); });

  buildKeyboard(); paintOct(); boot();
}
function paintOct() { const el = $("octLabel"); if (el) el.textContent = `oct ${octave >= 0 ? "+" + octave : octave}`; paintKeys(); }
let toastTimer = null;
function toast(msg) { const t = $("toast"); if (!t) return; t.textContent = msg; t.classList.add("show"); clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.remove("show"), 2200); }

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
else init();
