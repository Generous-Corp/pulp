// widgets/toggle.js — canvas switch matching the native Toggle paint
// (core/view/src/widgets.cpp:1227-1287). See tokens/WIDGETS.md "Toggle".
// Editor toggle box is 52×28. Boolean parameter (0/1).

import {
  tok, lerpHex, makeCanvas, ease,
  mountRepaint, isKbNav, setFocusedWidget, clearFocusedWidget, initModality,
} from "./base.js";

export function createToggle({ param, value, onChange, width = 52, height = 28 }) {
  initModality();
  let on = value >= 0.5;
  let focused = false;
  const st = { t: on ? 1 : 0, t_to: on ? 1 : 0, t_raf: 0,
               hover: 0, hover_to: 0, hover_raf: 0 };

  const el = document.createElement("div");
  el.className = "pw-toggle";
  el.tabIndex = 0;
  el.setAttribute("role", "switch");
  el.style.width = width + "px";
  el.style.height = height + "px";
  el.style.touchAction = "none";
  el.style.cursor = "pointer";

  const { cv, ctx, w, h } = makeCanvas(width, height);
  el.appendChild(cv);

  const switch_w = Math.min(w, 40);
  const switch_h = Math.min(h * 0.6, 20);
  const sx = (w - switch_w) / 2, sy = (h - switch_h) / 2;
  const thumbR = switch_h * 0.4;
  const offX = sx + switch_h / 2, onX = sx + switch_w - switch_h / 2;

  function rrect(x, y, rw, rh, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + rw, y, x + rw, y + rh, r);
    ctx.arcTo(x + rw, y + rh, x, y + rh, r);
    ctx.arcTo(x, y + rh, x, y, r);
    ctx.arcTo(x, y, x + rw, y, r);
    ctx.closePath();
  }

  function announce() {
    el.setAttribute("aria-checked", on ? "true" : "false");
    el.setAttribute("aria-valuetext", on ? "on" : "off");
  }

  function draw() {
    if (!el.isConnected) return;
    ctx.clearRect(0, 0, w, h);
    const cue = focused && isKbNav();
    const t = st.t;
    // Track: lerp(control.track → accent.primary, t).
    ctx.fillStyle = lerpHex(tok(el, "--control-track"), tok(el, "--accent-primary"), t);
    rrect(sx, sy, switch_w, switch_h, switch_h / 2); ctx.fill();
    // Hover overlay: white @ 15/255 × hover.
    if (st.hover > 0.001) {
      ctx.save();
      ctx.globalAlpha = (15 / 255) * st.hover;
      ctx.fillStyle = "#ffffff";
      rrect(sx, sy, switch_w, switch_h, switch_h / 2); ctx.fill();
      ctx.restore();
    }
    // Keyboard cue: ring the track in --focus-ring (never on pointer).
    if (cue) {
      ctx.strokeStyle = tok(el, "--focus-ring");
      ctx.lineWidth = 1.5;
      rrect(sx - 1, sy - 1, switch_w + 2, switch_h + 2, (switch_h + 2) / 2); ctx.stroke();
    }
    // Thumb: lerp(control.thumb → accent.text, t).
    const tx = offX + (onX - offX) * t;
    ctx.fillStyle = lerpHex(tok(el, "--control-thumb"), tok(el, "--accent-text"), t);
    ctx.beginPath();
    ctx.arc(tx, sy + switch_h / 2, thumbR, 0, Math.PI * 2);
    ctx.fill();
  }

  function set(next) {
    on = next;
    announce();
    ease(st, "t", on ? 1 : 0, 0.15, draw);
    onChange?.(on ? 1 : 0);
  }
  function toggle() { set(!on); }

  const focusRef = { draw };
  el.addEventListener("pointerdown", (e) => {
    if (e.button !== 0) return;
    toggle(); e.preventDefault();     // no focus() → no cue on click (data-kb-nav is off)
  });
  el.addEventListener("pointerenter", () => ease(st, "hover", 1, 0.08, draw));
  el.addEventListener("pointerleave", () => ease(st, "hover", 0, 0.08, draw));
  el.addEventListener("focus", () => { focused = true; setFocusedWidget(focusRef); draw(); });
  el.addEventListener("blur", () => { focused = false; clearFocusedWidget(focusRef); draw(); });
  el.addEventListener("keydown", (e) => {
    if (e.key === " " || e.key === "Enter") { toggle(); e.preventDefault(); }
  });

  el.setValue = (v) => { on = v >= 0.5; announce(); ease(st, "t", on ? 1 : 0, 0.15, draw); };
  el.getValue = () => (on ? 1 : 0);
  announce();
  mountRepaint(el, draw);
  draw();
  return el;
}
