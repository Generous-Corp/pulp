// widgets/knob.js — canvas knob matching the native Ink & Signal knob paint
// (core/view/src/widgets.cpp:737-852; angle constants widgets.hpp:415-416).
// See tokens/WIDGETS.md "Knob". Editor knob box is 84×84 (attach_knob(…,84)).

import {
  FONT, tok, makeCanvas, clamp, snap, toNorm, formatValue, ease, attachInteraction,
  mountRepaint, isKbNav, setFocusedWidget, clearFocusedWidget, initModality,
} from "./base.js";

const START = 2.356; // ≈135°, literal constant (not 3π/4)
const END = 7.069;   // ≈405°, literal constant
const DRAG_PX = 150; // native: 150px vertical = full travel

export function createKnob({ param, value, onChange, size = 84 }) {
  initModality();
  let val = snap(param, value);
  let focused = false;
  const st = { hover: 0, hover_to: 0, hover_raf: 0 };

  const el = document.createElement("div");
  el.className = "pw-knob";
  el.tabIndex = 0;
  el.setAttribute("role", "slider");
  el.style.width = size + "px";
  el.style.height = size + "px";
  el.style.touchAction = "none";

  const { cv, ctx, w, h } = makeCanvas(size, size);
  el.appendChild(cv);

  // Geometry (WIDGETS.md), stable for the widget size — also exposed for tests.
  const cx = w / 2, cy = h / 2;
  const full_r = Math.min(cx, cy) - 3;
  const arc_w = Math.max(3, full_r * 0.13);
  const ring_r = full_r - arc_w / 2;
  const body_r = ring_r - arc_w / 2 - 2;
  const dot_r = Math.max(2, full_r * 0.06);
  const geom = { cx, cy, full_r, arc_w, ring_r, body_r, dot_r, start: START, end: END };
  el.__geom = geom;

  function announce() {
    el.setAttribute("aria-valuemin", String(param.minValue));
    el.setAttribute("aria-valuemax", String(param.maxValue));
    el.setAttribute("aria-valuenow", String(val));
    el.setAttribute("aria-valuetext", formatValue(param, val));
  }

  function draw() {
    if (!el.isConnected) return;      // tokens only resolve on an attached element
    ctx.clearRect(0, 0, w, h);
    const cue = focused && isKbNav();  // keyboard-only focus cue (never on pointer)
    const p = clamp(toNorm(param, val), 0, 1);
    const valueAngle = START + p * (END - START);

    // 1. hover glow (accent.primary @ 0.16 × glow) over the full track arc.
    if (st.hover > 0.001) {
      ctx.save();
      ctx.globalAlpha = 0.16 * st.hover;
      ctx.strokeStyle = tok(el, "--accent-primary");
      ctx.lineWidth = arc_w + 6;
      ctx.lineCap = "round";
      ctx.beginPath();
      ctx.arc(cx, cy, ring_r, START, END);
      ctx.stroke();
      ctx.restore();
    }

    // 2. body disc (bg.elevated).
    ctx.fillStyle = tok(el, "--bg-elevated");
    ctx.beginPath();
    ctx.arc(cx, cy, body_r, 0, Math.PI * 2);
    ctx.fill();

    // 3. bevel ring (control.border @ 60%).
    ctx.save();
    ctx.globalAlpha = 0.6;
    ctx.strokeStyle = tok(el, "--control-border");
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(cx, cy, body_r - 0.5, 0, Math.PI * 2);
    ctx.stroke();
    ctx.restore();

    // 4. track ring (knob.arc.bg; --focus-ring while keyboard-focused), round caps.
    ctx.lineCap = "round";
    ctx.strokeStyle = cue ? tok(el, "--focus-ring") : tok(el, "--knob-arc-bg");
    ctx.lineWidth = arc_w;
    ctx.beginPath();
    ctx.arc(cx, cy, ring_r, START, END);
    ctx.stroke();

    // 5. value arc (knob.arc), thickened ~1.5px as a restrained keyboard cue.
    ctx.strokeStyle = tok(el, "--knob-arc");
    ctx.lineWidth = cue ? arc_w + 1.5 : arc_w;
    ctx.beginPath();
    ctx.arc(cx, cy, ring_r, START, valueAngle);
    ctx.stroke();
    ctx.lineWidth = arc_w;

    // 6. pointer dot (knob.thumb) on the disc face.
    const dc = body_r - dot_r - 2;
    const dx = cx + Math.cos(valueAngle) * dc;
    const dy = cy + Math.sin(valueAngle) * dc;
    ctx.fillStyle = tok(el, "--knob-thumb");
    ctx.beginPath();
    ctx.arc(dx, dy, dot_r, 0, Math.PI * 2);
    ctx.fill();

    // 8. value readout (text.primary, Inter 11px) at (cx, cy+4).
    ctx.fillStyle = tok(el, "--text-primary");
    ctx.font = `11px ${FONT}`;
    ctx.textAlign = "center";
    ctx.textBaseline = "alphabetic";
    ctx.fillText(formatValue(param, val), cx, cy + 4);
  }

  function commit(v) {
    const nv = snap(param, v);
    if (nv !== val) { val = nv; onChange?.(val); }
    announce();
    draw();
  }

  el.addEventListener("pointerenter", () => ease(st, "hover", 1, 0.08, draw));
  el.addEventListener("pointerleave", () => ease(st, "hover", 0, 0.08, draw));
  const focusRef = { draw };
  el.addEventListener("focus", () => { focused = true; setFocusedWidget(focusRef); draw(); });
  el.addEventListener("blur", () => { focused = false; clearFocusedWidget(focusRef); draw(); });

  attachInteraction({ el, param, get: () => val, set: commit, pixels: DRAG_PX });

  el.setValue = (v) => { val = snap(param, v); announce(); draw(); };
  el.getValue = () => val;
  announce();
  mountRepaint(el, draw);
  draw();
  return el;
}
