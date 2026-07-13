// ————————————————————————————————————————————————————————— minimal DOM shim
// Shared by the shell's DOM-level suites (custom-ui, file-upload). ONE copy, so a
// gap here (e.g. the missing getElementById that used to kill the whole suite) is
// fixed once rather than in each test's private fake.
//
// Import for side effects; it installs `document`/`window` on globalThis.
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
  // custom-ui.js guards its one-time <style> injection with getElementById; without
  // this the whole suite dies on `document.getElementById is not a function`.
  getElementById(id) { return query(this.documentElement, "#" + id)[0] || null; },

  // Document-level listeners are RECORDED, not swallowed. The file-upload drop
  // guard lives here — it is what stops a near-miss from navigating away and
  // destroying a running demo — so a test must be able to assert that it is bound
  // on mount, that it preventDefault()s a stray drop, and that destroy() takes it
  // back off (a swallowed no-op could never catch a leak).
  _listeners: new Map(),
  addEventListener(type, fn) {
    if (!this._listeners.has(type)) this._listeners.set(type, new Set());
    this._listeners.get(type).add(fn);
  },
  removeEventListener(type, fn) { this._listeners.get(type)?.delete(fn); },
  listenerCount(type) { return this._listeners.get(type)?.size || 0; },
  dispatchEvent(ev) {
    for (const fn of this._listeners.get(ev.type) || []) fn(ev);
    return true;
  },
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

export { El, query, document, ok, failed, audioNode, pendingRaf, rafSeq, parseHTML };
