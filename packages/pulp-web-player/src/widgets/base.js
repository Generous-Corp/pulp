// widgets/base.js — shared helpers for the canvas-drawn Pulp widgets.
//
// Everything here is token-driven: colors are read live from the CSS custom
// properties in tokens.css (getComputedStyle), never hardcoded, so the widgets
// track the active theme (our demos force data-theme="dark"). Geometry follows
// the native paint code as documented in tokens/WIDGETS.md.

export const FONT = '"Inter", system-ui, sans-serif';

// Read a CSS custom property (a design token) resolved on `el`.
export function tok(el, name) {
  return getComputedStyle(el).getPropertyValue(name).trim();
}

// sRGB linear interpolation of two hex colors — matches Color::interpolate
// (per-channel lerp of gamma-encoded sRGB, canvas.hpp:63-68). t in [0,1].
export function lerpHex(a, b, t) {
  const pa = hexToRgb(a), pb = hexToRgb(b);
  if (!pa || !pb) return a;
  const m = (x, y) => Math.round(x + (y - x) * t);
  return `rgb(${m(pa.r, pb.r)},${m(pa.g, pb.g)},${m(pa.b, pb.b)})`;
}
function hexToRgb(h) {
  h = h.trim();
  const m = /^#?([0-9a-f]{6})$/i.exec(h);
  if (m) {
    const n = parseInt(m[1], 16);
    return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
  }
  const rgb = /rgba?\(([^)]+)\)/i.exec(h);
  if (rgb) {
    const p = rgb[1].split(",").map((s) => parseFloat(s));
    return { r: p[0], g: p[1], b: p[2] };
  }
  return null;
}

// A crisp HiDPI canvas. Returns { cv, ctx, w, h }. ctx is pre-scaled so all
// drawing uses logical (CSS) pixel coordinates; clear with clearRect(0,0,w,h).
export function makeCanvas(w, h) {
  const cv = document.createElement("canvas");
  const dpr = Math.max(1, window.devicePixelRatio || 1);
  cv.width = Math.round(w * dpr);
  cv.height = Math.round(h * dpr);
  cv.style.width = w + "px";
  cv.style.height = h + "px";
  cv.style.display = "block";
  const ctx = cv.getContext("2d");
  ctx.scale(dpr, dpr);
  cv._dpr = dpr;
  cv._w = w;
  cv._h = h;
  return { cv, ctx, w, h };
}

// Resize an existing canvas (e.g. on a dpr change) keeping the scale invariant.
export function sizeCanvas(cv, w, h) {
  const dpr = Math.max(1, window.devicePixelRatio || 1);
  cv.width = Math.round(w * dpr);
  cv.height = Math.round(h * dpr);
  cv.style.width = w + "px";
  cv.style.height = h + "px";
  const ctx = cv.getContext("2d");
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.scale(dpr, dpr);
  cv._dpr = dpr;
  cv._w = w;
  cv._h = h;
  return ctx;
}

export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// Repaint a canvas widget once it is actually in the document — CSS custom
// properties (our design tokens) only resolve via getComputedStyle on an
// ATTACHED element, and the native Inter face may still be loading. Without
// this the first construction-time draw() paints token colors as "" (ignored →
// black) and text in a fallback face. A ResizeObserver fires when the element
// gains a box (i.e. is laid out in the DOM); document.fonts.ready covers the
// font. draw() itself bails while detached so no black flash is committed.
export function mountRepaint(el, draw) {
  try {
    const ro = new ResizeObserver(() => {
      const r = el.getBoundingClientRect();
      if (r.width > 0 && r.height > 0) draw();
    });
    ro.observe(el);
    el._ro = ro;
  } catch { requestAnimationFrame(() => el.isConnected && draw()); }
  if (typeof document !== "undefined" && document.fonts) {
    document.fonts.ready.then(() => el.isConnected && draw());
    try { document.fonts.addEventListener("loadingdone", () => el.isConnected && draw()); } catch {}
  }
}

// The quantization step for a parameter in value units (0 = continuous float).
export function quantStep(p) {
  if (p.type === "int") return 1;
  if (p.step && p.step > 0) return p.step;
  if (p.discreteStep && p.discreteStep > 0) return p.discreteStep;
  return 0;
}
// The step used by keyboard / wheel nudges (never 0).
export function nudgeStep(p) {
  const q = quantStep(p);
  if (q > 0) return q;
  const span = (p.maxValue - p.minValue) || 1;
  return span / 100;
}

// Snap a value to the parameter's grid and clamp to range.
export function snap(p, v) {
  const q = quantStep(p);
  if (q > 0) v = p.minValue + Math.round((v - p.minValue) / q) * q;
  return clamp(v, p.minValue, p.maxValue);
}

// Normalized position 0..1 for a value.
export const toNorm = (p, v) =>
  p.maxValue === p.minValue ? 0 : (v - p.minValue) / (p.maxValue - p.minValue);
export const fromNorm = (p, n) => p.minValue + n * (p.maxValue - p.minValue);

// Value readout string: choice → label; boolean → on/off; else 0/1/2 decimals
// by magnitude, plus unit.
export function formatValue(p, v) {
  if (p.type === "choice" && p.labels && p.labels.length) {
    const q = quantStep(p) || 1;
    let i = Math.round((v - p.minValue) / q);
    i = clamp(i, 0, p.labels.length - 1);
    return p.labels[i];
  }
  if (p.type === "boolean") return v >= 0.5 ? "on" : "off";
  let dec;
  const a = Math.abs(v);
  if (p.type === "int") dec = 0;
  else if (a >= 100) dec = 0;
  else if (a >= 10) dec = 1;
  else dec = 2;
  let s = v.toFixed(dec);
  if (p.unit) s += (p.unit === "%" ? "" : " ") + p.unit;
  return s;
}

// A tiny value-space animator: eases `state[key]` toward a target, invoking
// draw() each frame until settled. duration in seconds (from motion tokens).
export function ease(state, key, target, duration, draw) {
  state[key + "_to"] = target;
  if (state[key + "_raf"]) return; // a loop is already running toward the target
  let last = performance.now();
  const step = (now) => {
    const dt = (now - last) / 1000;
    last = now;
    const to = state[key + "_to"];
    const cur = state[key];
    // exponential approach tuned so ~duration reaches within ~2%.
    const k = duration > 0 ? 1 - Math.exp((-dt / duration) * 4) : 1;
    const next = cur + (to - cur) * k;
    state[key] = Math.abs(to - next) < 0.001 ? to : next;
    draw();
    if (state[key] !== to) {
      state[key + "_raf"] = requestAnimationFrame(step);
    } else {
      state[key + "_raf"] = 0;
    }
  };
  state[key + "_raf"] = requestAnimationFrame(step);
}

// ————————————————————————————————————————————————————— input modality
// The keyboard-vs-pointer focus flag. A ring/cue is painted only while the last
// input was a navigation key (data-kb-nav present on <html>). Cleared on any
// pointerdown — so no focus cue ever appears from mouse interaction (during a
// drag, after release, or after picking a combo option), while DOM focus stays
// where it belongs for keyboard users (no blur() hacks). Widgets read isKbNav()
// in their draw() and register as the focused widget so a modality flip repaints
// them immediately.
const NAV_KEYS = new Set(["Tab", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",
  "Home", "End", "PageUp", "PageDown", "Enter", " "]);
let focusedWidget = null;
let modalityInited = false;
export function isKbNav() {
  return typeof document !== "undefined" && document.documentElement.hasAttribute("data-kb-nav");
}
export function setFocusedWidget(w) { focusedWidget = w; }
export function clearFocusedWidget(w) { if (focusedWidget === w) focusedWidget = null; }
export function initModality() {
  if (modalityInited || typeof document === "undefined") return;
  modalityInited = true;
  addEventListener("keydown", (e) => {
    if (NAV_KEYS.has(e.key)) {
      document.documentElement.dataset.kbNav = "";
      focusedWidget?.draw?.();
    }
  }, true);
  addEventListener("pointerdown", () => {
    if ("kbNav" in document.documentElement.dataset) {
      delete document.documentElement.dataset.kbNav;
      focusedWidget?.draw?.();
    }
  }, true);
}

// Attach drag / wheel / keyboard / double-click behavior to a value widget.
// opts: { el, param, get, set, pixels, orientation }
//   get()          -> current value
//   set(v, opts?)  -> commit a value (caller snaps/clamps/redraws/announces)
//   pixels         -> px of vertical travel for the full range (knob 150; fader = track len)
// Vertical drag; up = increase. Shift = fine (0.25x). Wheel + arrows respect
// the parameter step. Double-click resets to defaultValue. NOTE: pointerdown
// does NOT call el.focus() — the drag runs off pointer capture, and letting Tab
// own focus keeps the keyboard cue from ever appearing on a mouse drag.
export function attachInteraction({ el, param, get, set, pixels }) {
  const span = (param.maxValue - param.minValue) || 1;
  let dragging = false, startY = 0, startVal = 0, pid = null;

  el.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return;
    dragging = true;
    startY = e.clientY;
    startVal = get();
    pid = e.pointerId;
    try { el.setPointerCapture(pid); } catch {}
    e.preventDefault();     // also prevents native focus-on-mousedown → no cue while dragging
  });
  el.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    const dy = startY - e.clientY;                 // up = positive
    const fine = e.shiftKey ? 0.25 : 1;
    const deltaNorm = (dy / pixels) * fine;
    set(startVal + deltaNorm * span);
    e.preventDefault();
  });
  const end = () => {
    if (!dragging) return;
    dragging = false;
    try { el.releasePointerCapture(pid); } catch {}
  };
  el.addEventListener("pointerup", end);
  el.addEventListener("pointercancel", end);
  el.addEventListener("lostpointercapture", end);

  el.addEventListener("dblclick", (e) => {
    set(param.defaultValue);
    e.preventDefault();
  });

  el.addEventListener("wheel", (e) => {
    const inc = nudgeStep(param) * (e.shiftKey ? 0.25 : 1);
    const dir = e.deltaY < 0 ? 1 : -1;
    set(get() + dir * inc);
    e.preventDefault();
  }, { passive: false });

  el.addEventListener("keydown", (e) => {
    const base = nudgeStep(param);
    const inc = base * (e.shiftKey ? 0.25 : 1);
    let handled = true;
    switch (e.key) {
      case "ArrowUp": case "ArrowRight": set(get() + inc); break;
      case "ArrowDown": case "ArrowLeft": set(get() - inc); break;
      case "PageUp": set(get() + base * 5); break;
      case "PageDown": set(get() - base * 5); break;
      case "Home": set(param.minValue); break;
      case "End": set(param.maxValue); break;
      default: handled = false;
    }
    if (handled) e.preventDefault();
  });
}
