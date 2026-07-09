// widgets/fader.js — canvas vertical fader matching the native default Fader
// paint (core/view/src/widgets.cpp:856-1057). See tokens/WIDGETS.md "Fader".
// Editor fader box is 40×84 (vertical).

import {
  tok, makeCanvas, clamp, snap, toNorm, formatValue, ease, attachInteraction,
  mountRepaint, isKbNav, setFocusedWidget, clearFocusedWidget, initModality,
} from "./base.js";

export function createFader({ param, value, onChange, width = 40, height = 84 }) {
  initModality();
  let val = snap(param, value);
  let focused = false;
  const st = { scale: 1, scale_to: 1, scale_raf: 0 };

  const el = document.createElement("div");
  el.className = "pw-fader";
  el.tabIndex = 0;
  el.setAttribute("role", "slider");
  el.setAttribute("aria-orientation", "vertical");
  el.style.width = width + "px";
  el.style.height = height + "px";
  el.style.touchAction = "none";

  const { cv, ctx, w, h } = makeCanvas(width, height);
  el.appendChild(cv);

  // The native Fader's thumb is a PILL, not a circle. `ThumbShape::rectangle`
  // is the default (core/view/include/pulp/view/widgets.hpp:682) and the
  // Ink & Signal editor never calls set_thumb_shape (ink_signal_editor.hpp:220).
  // For the editor's 40x84 vertical fader, widgets.cpp:1003-1004 gives
  //   thumb_w = min(b.width, track_width) = 40   (full cross-axis width)
  //   thumb_h = 9
  //   radius  = min(w, h) * 0.5 = 4.5            (pill ends)
  // WIDGETS.md wrongly described this as a circle; a circular thumb was the
  // single largest geometry deviation from the native baseline.
  const THUMB_H = 9;                   // widgets.cpp:1004, unskinned vertical
  const HOVER_SCALE = 1.3;             // widgets.cpp:324, must match the ease below
  const cxc = w / 2;                   // cross-axis centre (the track)

  // Travel of the thumb CENTRE at rest. Native recomputes this from the *scaled*
  // half-height each frame (widgets.cpp:1006-1008), so a hovered thumb shrinks
  // its own travel and can never overrun the ends. Interaction mapping uses the
  // unscaled value so drag sensitivity doesn't change on hover.
  const usable = Math.max(0, h - THUMB_H);

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
    el.setAttribute("aria-valuemin", String(param.minValue));
    el.setAttribute("aria-valuemax", String(param.maxValue));
    el.setAttribute("aria-valuenow", String(val));
    el.setAttribute("aria-valuetext", formatValue(param, val));
  }

  function draw() {
    if (!el.isConnected) return;
    ctx.clearRect(0, 0, w, h);
    const cue = focused && isKbNav();
    const p = clamp(toNorm(param, val), 0, 1);
    const thick = 4, rad = 2, tx = cxc - thick / 2;

    // Track (control.track), full height.
    ctx.fillStyle = tok(el, "--control-track");
    rrect(tx, 0, thick, h, rad); ctx.fill();

    // Fill (control.fill) from the bottom, height p × length.
    const fillH = p * h;
    if (fillH > 0) {
      ctx.fillStyle = tok(el, "--control-fill");
      rrect(tx, h - fillH, thick, fillH, rad); ctx.fill();
    }

    // Thumb (control.thumb): a full-width pill, measured from the bottom.
    // Both axes scale on hover, exactly as native does — so the thumb overflows
    // the box horizontally when hovered (the widget bounds clip it, same as
    // native), while `axisHalf` from the SCALED height keeps it inside vertically.
    const tw = Math.max(1, w * st.scale);
    const th = Math.max(1, THUMB_H * st.scale);
    const axisHalf = th * 0.5;
    const travel = Math.max(0, h - 2 * axisHalf);
    const cy = axisHalf + (1 - p) * travel;
    const thumbRad = Math.min(tw, th) * 0.5;     // pill ends
    const thumbX = (w - tw) * 0.5;
    const thumbY = cy - th * 0.5;

    ctx.fillStyle = tok(el, "--control-thumb");
    rrect(thumbX, thumbY, tw, th, thumbRad); ctx.fill();

    // Keyboard cue: outline the pill in --focus-ring (never on pointer).
    if (cue) {
      ctx.strokeStyle = tok(el, "--focus-ring");
      ctx.lineWidth = 1.5;
      rrect(thumbX - 1, thumbY - 1, tw + 2, th + 2, thumbRad + 1); ctx.stroke();
    }
  }

  function commit(v) {
    const nv = snap(param, v);
    if (nv !== val) { val = nv; onChange?.(val); }
    announce();
    draw();
  }

  const focusRef = { draw };
  el.addEventListener("pointerenter", () => ease(st, "scale", HOVER_SCALE, 0.08, draw));
  el.addEventListener("pointerleave", () => ease(st, "scale", 1, 0.08, draw));
  el.addEventListener("focus", () => { focused = true; setFocusedWidget(focusRef); draw(); });
  el.addEventListener("blur", () => { focused = false; clearFocusedWidget(focusRef); draw(); });

  attachInteraction({ el, param, get: () => val, set: commit, pixels: usable });

  el.setValue = (v) => { val = snap(param, v); announce(); draw(); };
  el.getValue = () => val;
  announce();
  mountRepaint(el, draw);
  draw();
  return el;
}
