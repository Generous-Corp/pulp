#!/usr/bin/env node
// custom-ui.test.mjs — the parity guard for mountDemo({ customUi }).
//
// The hook replaces the auto-generated parameter grid and NOTHING ELSE, in the
// SHARED shell — so a WAM demo and a WebCLAP demo get it from one code path and
// cannot drift. These tests run the real shell against a mock host adapter on a
// minimal DOM shim (zero dependencies — the package ships none, and the other
// two suites run in any Node), and assert:
//
//   1. no customUi                → the generated widget grid still renders
//   2. customUi                   → grid gone, hook mounted once, and the
//                                   overlay / touch hygiene / keyboard / scope /
//                                   meter / limiter all still work around it
//   3. params flow both ways      → customUi → adapter, adapter → customUi
//   4. a customUi that THROWS     → honest degradation back to the grid
//   5. teardown                   → destroy() runs, DOM detached, rAF cancelled
//
//   Run:  node test/custom-ui.test.mjs

let failed = 0;
const ok = (cond, msg) => { console.log(`${cond ? "  ok  " : "FAIL  "}${msg}`); if (!cond) failed++; };

// ————————————————————————————————————————————————————————— minimal DOM shim
// Only what the shell and the canvas widgets actually touch. Everything the
// widgets draw with is a no-op sink; the tests assert on the DOM tree and on the
// adapter traffic, never on pixels.
const VOID = new Set(["meta", "link", "input", "br", "img", "hr", "path", "circle", "rect", "source", "use"]);
const kebab = (s) => s.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
const camel = (s) => s.replace(/-([a-z])/g, (_, c) => c.toUpperCase());

const ctx2d = new Proxy({}, {
  get: (t, k) => (k in t ? t[k] : () => ({ width: 0, addColorStop() {} })),
  set: (t, k, v) => { t[k] = v; return true; },
});

class El {
  constructor(tag) {
    this.tagName = String(tag).toUpperCase();
    this.childNodes = [];
    this.parentNode = null;
    this._attrs = new Map();
    this._text = "";
    this._listeners = new Map();
    this.style = {};
    this.value = "";
    this.checked = false;
    this.tabIndex = -1;
    this.clientWidth = 600;
    this.clientHeight = 110;
    this.scrollTop = 0;
    this.scrollHeight = 0;
    this.isContentEditable = false;
    const self = this;
    this.dataset = new Proxy({}, {
      get: (_, k) => (typeof k === "string" ? self._attrs.get("data-" + kebab(k)) : undefined),
      set: (_, k, v) => { self._attrs.set("data-" + kebab(k), String(v)); return true; },
      has: (_, k) => self._attrs.has("data-" + kebab(k)),
      deleteProperty: (_, k) => { self._attrs.delete("data-" + kebab(k)); return true; },
      ownKeys: () => [...self._attrs.keys()].filter((k) => k.startsWith("data-")).map((k) => camel(k.slice(5))),
      getOwnPropertyDescriptor: () => ({ enumerable: true, configurable: true }),
    });
    this.classList = {
      add: (...c) => self._setClasses([...self._classes(), ...c]),
      remove: (...c) => self._setClasses(self._classes().filter((x) => !c.includes(x))),
      contains: (c) => self._classes().includes(c),
      toggle: (c, on) => (on ?? !self._classes().includes(c)) ? self.classList.add(c) : self.classList.remove(c),
    };
  }
  _classes() { return (this._attrs.get("class") || "").split(/\s+/).filter(Boolean); }
  _setClasses(list) { this._attrs.set("class", [...new Set(list)].join(" ")); }

  get className() { return this._attrs.get("class") || ""; }
  set className(v) { this._attrs.set("class", String(v)); }
  get id() { return this._attrs.get("id") || ""; }
  set id(v) { this._attrs.set("id", String(v)); }
  get type() { return this._attrs.get("type") || ""; }
  get children() { return this.childNodes.filter((n) => n.tagName !== "#TEXT"); }
  get childElementCount() { return this.children.length; }
  get firstChild() { return this.childNodes[0] || null; }
  get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === document.documentElement; }

  get textContent() {
    if (this.tagName === "#TEXT") return this._text;
    return this.childNodes.map((c) => c.textContent).join("");
  }
  set textContent(v) {
    if (this.tagName === "#TEXT") { this._text = String(v); return; }
    this.childNodes = [];
    if (v !== "" && v != null) this.appendChild(text(String(v)));
  }
  set innerHTML(v) {
    this.childNodes = [];
    for (const n of parseHTML(String(v))) this.appendChild(n);
  }
  get innerHTML() { return this.childNodes.map(serialize).join(""); }

  appendChild(n) { if (n.parentNode) n.parentNode.removeChild(n); n.parentNode = this; this.childNodes.push(n); return n; }
  removeChild(n) { const i = this.childNodes.indexOf(n); if (i >= 0) this.childNodes.splice(i, 1); n.parentNode = null; return n; }
  remove() { this.parentNode?.removeChild(this); }
  after(n) {
    const p = this.parentNode;
    if (!p) return;
    if (n.parentNode) n.parentNode.removeChild(n);
    p.childNodes.splice(p.childNodes.indexOf(this) + 1, 0, n);
    n.parentNode = p;
  }
  contains(n) { for (let c = n; c; c = c.parentNode) if (c === this) return true; return false; }

  setAttribute(k, v) { this._attrs.set(k, String(v)); }
  getAttribute(k) { return this._attrs.has(k) ? this._attrs.get(k) : null; }
  hasAttribute(k) { return this._attrs.has(k); }
  removeAttribute(k) { this._attrs.delete(k); }

  addEventListener(type, fn, opts) {
    if (!this._listeners.has(type)) this._listeners.set(type, []);
    this._listeners.get(type).push({ fn, once: !!(opts && opts.once) });
  }
  removeEventListener(type, fn) {
    const l = this._listeners.get(type);
    if (l) this._listeners.set(type, l.filter((e) => e.fn !== fn));
  }
  dispatchEvent(ev) {
    ev.target = ev.target || this;
    for (const e of (this._listeners.get(ev.type) || []).slice()) {
      if (e.once) this.removeEventListener(ev.type, e.fn);
      e.fn.call(this, ev);
    }
    return true;
  }
  listenerCount(type) { return (this._listeners.get(type) || []).length; }

  getBoundingClientRect() { return { width: 110, height: 110, top: 0, left: 0, right: 110, bottom: 110 }; }
  getContext() { return ctx2d; }
  setPointerCapture() {}
  releasePointerCapture() {}
  focus() { document.activeElement = this; }
  blur() { if (document.activeElement === this) document.activeElement = null; }

  querySelector(sel) { return query(this, sel)[0] || null; }
  querySelectorAll(sel) { return query(this, sel); }
}

const text = (s) => { const n = new El("#text"); n._text = s; return n; };
const serialize = (n) =>
  n.tagName === "#TEXT" ? n._text
    : `<${n.tagName.toLowerCase()}>${n.childNodes.map(serialize).join("")}</${n.tagName.toLowerCase()}>`;

const TAG = /<\/?([a-zA-Z][a-zA-Z0-9-]*)/y;
const ATTR = /\s*([a-zA-Z_:@-][a-zA-Z0-9:._-]*)(\s*=\s*("([^"]*)"|'([^']*)'|([^\s"'>]+)))?/y;
function parseHTML(html) {
  const out = [];
  const stack = [];
  const push = (n) => (stack.length ? stack[stack.length - 1].appendChild(n) : out.push(n));
  let i = 0;
  while (i < html.length) {
    const lt = html.indexOf("<", i);
    if (lt < 0) { if (html.slice(i).trim()) push(text(html.slice(i))); break; }
    if (lt > i) { const t = html.slice(i, lt); if (t.trim()) push(text(t)); }
    TAG.lastIndex = lt;
    const m = TAG.exec(html);
    if (!m) { i = lt + 1; continue; }
    const closing = html[lt + 1] === "/";
    const name = m[1];
    let j = TAG.lastIndex;
    if (closing) { i = html.indexOf(">", j) + 1; stack.pop(); continue; }
    const el = new El(name);
    for (;;) {
      ATTR.lastIndex = j;
      const a = ATTR.exec(html);
      if (!a || !a[1]) break;
      el.setAttribute(a[1], a[4] ?? a[5] ?? a[6] ?? "");
      j = ATTR.lastIndex;
    }
    while (j < html.length && html[j] !== ">") j++;
    const selfClosing = html[j - 1] === "/" || VOID.has(name.toLowerCase());
    push(el);
    if (!selfClosing) stack.push(el);
    i = j + 1;
  }
  return out;
}

// Selector engine: comma groups of descendant-combined compounds; a compound is
// any mix of tag / #id / .class / [attr="value"].
function matchesCompound(el, comp) {
  const toks = comp.match(/(\[[^\]]+\]|[#.]?[a-zA-Z0-9_*-]+)/g) || [];
  for (const t of toks) {
    if (t === "*") continue;
    if (t[0] === "#") { if (el.id !== t.slice(1)) return false; }
    else if (t[0] === ".") { if (!el.classList.contains(t.slice(1))) return false; }
    else if (t[0] === "[") {
      const m = /^\[([^=\]]+)(?:=["']?([^"'\]]*)["']?)?\]$/.exec(t);
      if (!m) return false;
      if (!el.hasAttribute(m[1])) return false;
      if (m[2] !== undefined && el.getAttribute(m[1]) !== m[2]) return false;
    } else if (el.tagName !== t.toUpperCase()) return false;
  }
  return true;
}
function matchesChain(el, group) {
  const parts = group.trim().split(/\s+/);
  if (!matchesCompound(el, parts[parts.length - 1])) return false;
  let n = el.parentNode;
  for (let i = parts.length - 2; i >= 0; i--) {
    while (n && !matchesCompound(n, parts[i])) n = n.parentNode;
    if (!n) return false;
    n = n.parentNode;
  }
  return true;
}
function query(rootEl, sel) {
  const groups = sel.split(",").map((s) => s.trim()).filter(Boolean);
  const out = [];
  (function walk(n) {
    for (const c of n.childNodes) {
      if (c.tagName === "#TEXT") continue;
      if (groups.some((g) => matchesChain(c, g))) out.push(c);
      walk(c);
    }
  })(rootEl);
  return out;
}

const document = {
  documentElement: new El("html"),
  head: new El("head"),
  body: new El("body"),
  activeElement: null,
  hidden: false,
  title: "",
  fonts: { ready: Promise.resolve(), addEventListener() {} },
  createElement: (t) => new El(t),
  querySelector(s) { return query(this.documentElement, s)[0] || null; },
  querySelectorAll(s) { return query(this.documentElement, s); },
  addEventListener() {},
  removeEventListener() {},
};
document.documentElement.appendChild(document.head);
document.documentElement.appendChild(document.body);

// requestAnimationFrame never fires — callbacks are only counted, so a leaked
// loop is visible as a still-pending id after teardown.
const pendingRaf = new Set();
let rafSeq = 0;

const def = (name, value) => Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });
def("window", globalThis);
def("document", document);
def("devicePixelRatio", 1);
def("innerWidth", 1200);
def("isSecureContext", true);
def("matchMedia", () => ({ matches: false, addEventListener() {}, addListener() {} }));
def("getComputedStyle", () => ({ getPropertyValue: () => "" }));
def("ResizeObserver", class { observe() {} unobserve() {} disconnect() {} });
def("requestAnimationFrame", () => { const id = ++rafSeq; pendingRaf.add(id); return id; });
def("cancelAnimationFrame", (id) => { pendingRaf.delete(id); });
def("addEventListener", () => {});
def("removeEventListener", () => {});
if (!globalThis.navigator) def("navigator", {});

const audioNode = () => ({ connect() {}, disconnect() {} });
class FakeAudioContext {
  constructor() {
    this.sampleRate = 48000;
    this.state = "running";
    this.destination = audioNode();
  }
  resume() { this.state = "running"; return Promise.resolve(); }
  close() { this.state = "closed"; return Promise.resolve(); }
  createAnalyser() {
    return { fftSize: 2048, ...audioNode(), getFloatTimeDomainData: (b) => b.fill(0) };
  }
  createGain() { return { gain: { value: 1 }, ...audioNode() }; }
  createDynamicsCompressor() {
    return { threshold: {}, knee: {}, ratio: {}, attack: {}, release: {}, reduction: 0, ...audioNode() };
  }
  createBufferSource() { return { buffer: null, loop: false, start() {}, stop() {}, ...audioNode() }; }
  createBuffer(ch, len) { return { getChannelData: () => new Float32Array(len) }; }
}
FakeAudioContext.prototype.audioWorklet = { addModule: () => Promise.resolve() };
def("AudioContext", FakeAudioContext);

// ————————————————————————————————————————————————————————— mock host adapter
const PARAMS = [
  { id: 1, label: "Gain", type: "float", unit: "dB", minValue: 0, maxValue: 1, defaultValue: 0.5 },
  { id: 2, label: "Drive", type: "float", minValue: 0, maxValue: 10, defaultValue: 2 },
  { id: 3, label: "Bypass", type: "boolean", minValue: 0, maxValue: 1, defaultValue: 0 },
];
function makeAdapter() {
  const values = new Map(PARAMS.map((p) => [p.id, p.defaultValue]));
  return {
    descriptor: { name: "Mock", isInstrument: true, hasMidiInput: true, hasAudioOutput: true },
    audioNode: audioNode(),
    getParameterInfo: async () => PARAMS,
    setParameterValue(id, v) { values.set(id, v); this.sets.push([id, v]); },
    getParameterValue: async (id) => values.get(id),
    scheduleMidi(...a) { this.midi.push(a); },
    sendSysex() {},
    getState: async () => new Uint8Array(0),
    setState() {},
    onMidiOut: null,
    onParamsChanged: null,
    createSecondary: async () => makeAdapter(),
    destroy() {},
    sets: [],
    midi: [],
  };
}

// A custom UI that exercises everything the contract promises: it writes params,
// listens for pushes, holds an rAF loop and a listener, and releases both.
function makeCustomUi({ shouldThrow = false, falsy = false } = {}) {
  const state = { calls: [], destroyed: 0, pushes: [], rafId: 0, btn: null };
  const factory = (container, adapter, info) => {
    state.calls.push({ container, adapter, info });
    if (shouldThrow) throw new Error("custom UI blew up");
    if (falsy) return null;
    const btn = document.createElement("button");
    const onClick = () => adapter.setParameterValue(1, 0.25);
    btn.addEventListener("click", onClick);
    container.appendChild(btn);
    state.btn = btn;
    const loop = () => { state.rafId = requestAnimationFrame(loop); };
    loop();
    return {
      onParamsChanged: (values, params) => state.pushes.push([values, params]),
      destroy() {
        state.destroyed++;
        cancelAnimationFrame(state.rafId);
        btn.removeEventListener("click", onClick);
        btn.remove();
      },
    };
  };
  return { factory, state };
}

const warnings = [];
const realWarn = console.warn;
console.warn = (...a) => warnings.push(a.map(String).join(" "));

const { mountDemo } = await import("../src/shell.js");

const settle = () => new Promise((r) => setTimeout(r, 0));
async function mount(extra = {}) {
  document.body.childNodes = [];
  const root = document.createElement("div");
  document.body.appendChild(root);
  const adapter = makeAdapter();
  await mountDemo({
    root, title: "Mock", subtitle: "shell parity", mode: "instrument", paramRows: 1,
    createAdapter: async () => adapter,
    ...extra,
  });
  await globalThis.__start();
  await settle();
  return { root, adapter };
}
// Everything the shell owns and a custom UI must NOT disturb.
function assertShellIntact(root, label) {
  ok(!!root.querySelector("#overlay"), `${label}: start overlay present`);
  ok(root.querySelector("#overlay").style.display === "none", `${label}: overlay dismissed by start()`);
  ok(root.querySelectorAll("#kb .wk").length > 0 && root.querySelectorAll("#kb .bk").length > 0,
     `${label}: on-screen keyboard rendered`);
  ok(!!root.querySelector("#scope"), `${label}: oscilloscope canvas present`);
  ok(!!root.querySelector(".pw-meter"), `${label}: level meter present`);
  ok(globalThis.__limiterReductionDb() !== null, `${label}: safety limiter in the output chain`);
  ok(!!root.querySelector("#stop") && !!root.querySelector("#status"), `${label}: transport + status present`);
  const css = document.head.querySelectorAll("style").map((s) => s.textContent).join("");
  ok(/-webkit-touch-callout:none/.test(css) && /touch-action:manipulation/.test(css)
     && /#panel textarea,#panel input\[type=text\]/.test(css),
     `${label}: mobile touch hygiene installed (and re-enabled on text entry)`);
}

// ——— 1. No customUi: the generated grid still renders (no regression).
{
  const { root, adapter } = await mount();
  ok(root.querySelectorAll("#params .pw-cell").length === PARAMS.length,
     "no customUi: one generated cell per parameter");
  ok(!root.querySelector("#custom-ui"), "no customUi: no custom container mounted");
  ok(Object.keys(globalThis.__widgets).length === PARAMS.length, "no customUi: widget registry populated");
  assertShellIntact(root, "no customUi");
  ok(adapter.sets.some(([id, v]) => id === 1 && v === 0.5), "no customUi: defaults seeded to the plugin");
}

// ——— 2. customUi: it replaces the grid AND ONLY the grid.
{
  const ui = makeCustomUi();
  const { root, adapter } = await mount({ customUi: ui.factory });
  ok(ui.state.calls.length === 1, "customUi: factory called exactly once");
  ok(ui.state.calls[0].adapter === adapter, "customUi: receives the same host adapter the shell drives");
  ok(ui.state.calls[0].container === root.querySelector("#custom-ui"),
     "customUi: receives the container mounted in the grid's slot");
  ok(ui.state.calls[0].container.parentNode === root.querySelector("#params"),
     "customUi: container sits inside #params (grid slot, reserved height)");
  ok(root.querySelectorAll("#params .pw-cell").length === 0, "customUi: generated grid NOT rendered");
  ok(Object.keys(globalThis.__widgets).length === 0, "customUi: no generated widgets registered");
  ok(adapter.sets.some(([id, v]) => id === 1 && v === 0.5), "customUi: defaults still seeded to the plugin");
  assertShellIntact(root, "customUi");
  // The keyboard still plays through the adapter, and paints its key.
  globalThis.__player.noteOn(60);
  ok(adapter.midi.some(([s, d1]) => s === 0x90 && d1 === 60), "customUi: keyboard still sends note-on");
  ok(root.querySelector('[data-note="60"]').classList.contains("on"), "customUi: key still lights up");
  globalThis.__player.noteOff(60);
  ok(adapter.midi.some(([s, d1]) => s === 0x80 && d1 === 60), "customUi: keyboard still sends note-off");
}
// ——— 3. Parameters flow BOTH ways through the adapter the hook was handed.
{
  const ui = makeCustomUi();
  const { adapter } = await mount({ customUi: ui.factory });
  ui.state.btn.dispatchEvent({ type: "click" });
  ok(adapter.sets.some(([id, v]) => id === 1 && v === 0.25),
     "customUi → adapter: setParameterValue reaches the host");
  adapter.onParamsChanged([0.9, 3, 1], PARAMS);
  ok(ui.state.pushes.length === 1 && ui.state.pushes[0][0][0] === 0.9,
     "adapter → customUi: onParamsChanged is forwarded to the hook");
  ok(globalThis.__demo.paramEpochUpdates === 1, "adapter → customUi: the param-epoch test seam still ticks");
}

// ——— 4. Honest degradation: a throwing customUi falls back to the grid.
{
  warnings.length = 0;
  const ui = makeCustomUi({ shouldThrow: true });
  const { root } = await mount({ customUi: ui.factory });
  ok(ui.state.calls.length === 1, "throwing customUi: factory was attempted");
  ok(root.querySelectorAll("#params .pw-cell").length === PARAMS.length,
     "throwing customUi: generated grid rendered instead");
  ok(!root.querySelector("#custom-ui"), "throwing customUi: failed container removed");
  ok(warnings.some((w) => /customUi threw while mounting/.test(w)), "throwing customUi: failure is logged");
  assertShellIntact(root, "throwing customUi");
}

// ——— 4b. A customUi that returns no handle degrades the same way.
{
  warnings.length = 0;
  const ui = makeCustomUi({ falsy: true });
  const { root } = await mount({ customUi: ui.factory });
  ok(root.querySelectorAll("#params .pw-cell").length === PARAMS.length,
     "falsy customUi: generated grid rendered instead");
  ok(warnings.some((w) => /customUi returned no handle/.test(w)), "falsy customUi: failure is logged");
}

// ——— 4c. The REAL failure mode: a customUi that mounts ASYNCHRONOUSLY and fails
// asynchronously (mountPulpUi rejects when the browser has no WebGL2 context).
// It returns a handle synchronously, so the sync fallbacks above never fire; the
// `ready` promise is what has to restore the grid. Before that seam existed, a
// browser with no WebGL2 was left with an EMPTY panel while the docs promised a
// generated grid. This mirrors the factory assemble-gallery.mjs emits.
{
  warnings.length = 0;
  let destroyed = 0;
  const canvas = document.createElement("canvas");
  const factory = (container) => {
    container.appendChild(canvas);
    const pending = Promise.reject(new Error("no WebGL2 context"));
    pending.catch(() => {});   // the page logs it; the shell handles the fallback
    return { ready: pending, destroy: () => { destroyed++; canvas.remove(); } };
  };
  const { root } = await mount({ customUi: factory });
  await settle();
  await settle();

  ok(root.querySelectorAll("#params .pw-cell").length === PARAMS.length,
     "async-failing customUi: generated grid rendered after the ready promise rejects");
  ok(!root.querySelector("#custom-ui"), "async-failing customUi: failed container removed");
  ok(destroyed === 1, "async-failing customUi: the handle's destroy() ran exactly once");
  ok(Object.keys(globalThis.__widgets).length === PARAMS.length,
     "async-failing customUi: the widget registry is populated (param sync works again)");
  ok(warnings.some((w) => /customUi failed to mount asynchronously/.test(w)),
     "async-failing customUi: failure is logged");
  assertShellIntact(root, "async-failing customUi");
}

// ——— 5. Teardown: destroy() runs, DOM detached, no leaked rAF / listener.
{
  const ui = makeCustomUi();
  const { root, adapter } = await mount({ customUi: ui.factory });
  const container = ui.state.calls[0].container;
  const rafId = ui.state.rafId;
  ok(pendingRaf.has(rafId), "teardown: the customUi render loop is live before Stop");

  root.querySelector("#stop").dispatchEvent({ type: "click" });
  await settle();

  ok(ui.state.destroyed === 1, "teardown: destroy() called exactly once");
  ok(!pendingRaf.has(rafId), "teardown: the customUi's rAF was cancelled (no leaked loop)");
  ok(ui.state.btn.listenerCount("click") === 0, "teardown: the customUi's listeners were removed");
  ok(container.parentNode === null && !root.querySelector("#custom-ui"),
     "teardown: the custom container is detached from the panel");
  const before = ui.state.pushes.length;
  adapter.onParamsChanged([0.1, 1, 0], PARAMS);
  ok(ui.state.pushes.length === before, "teardown: no param pushes reach a destroyed customUi");
  ok(root.querySelector("#overlay").style.display === "flex", "teardown: the start overlay is back");
}

console.warn = realWarn;
console.log(failed ? `\n${failed} assertion(s) FAILED` : "\ncustom-UI hook intact — all assertions passed");
process.exit(failed ? 1 : 0);
