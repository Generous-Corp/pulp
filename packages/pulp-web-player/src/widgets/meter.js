// widgets/meter.js — canvas level meter matching the native Meter paint
// (core/view/src/widgets/visualizers.cpp:427-585). See tokens/WIDGETS.md "Meter".
// Vertical. Driven by push(level) each animation frame; the ballistics live in
// meter-ballistics.js (a pure, DOM-free, unit-tested port of the native
// MeterBallistics::update) so the release/hold timing is exact — instantaneous
// attack, exponential release with the 0.30 s meter_decay time constant, and a
// held peak that sticks for 1.50 s before it falls. Driven from real
// performance.now() deltas, so the decay is frame-rate independent.

import { tok, makeCanvas, clamp, sizeCanvas, mountRepaint } from "./base.js";
import { createBallistics, updateBallistics, meterColorStop } from "./meter-ballistics.js";

export function createMeter({ width = 12, height = 84 } = {}) {
  const el = document.createElement("div");
  el.className = "pw-meter";
  el.style.width = width + "px";
  el.style.height = height + "px";
  const { cv, ctx, w, h } = makeCanvas(width, height);
  el.appendChild(cv);
  el._ctx = ctx; el._w = w; el._h = h;

  const bal = createBallistics();
  let last = performance.now();

  function rrect(c, x, y, rw, rh, r) {
    c.beginPath();
    c.moveTo(x + r, y);
    c.arcTo(x + rw, y, x + rw, y + rh, r);
    c.arcTo(x + rw, y + rh, x, y + rh, r);
    c.arcTo(x, y + rh, x, y, r);
    c.arcTo(x, y, x + rw, y, r);
    c.closePath();
  }

  // Stepped fill colour (green / yellow / red at 0.7 and 0.9). meterColorStop
  // returns the palette name; map it to the live meter token.
  function stepColor(v) {
    return tok(el, "--meter-" + meterColorStop(v));
  }

  function draw() {
    if (!el.isConnected) return;
    const c = el._ctx, W = el._w, H = el._h;
    const level = bal.displayRms, peak = bal.displayPeak, held = bal.heldPeak;
    c.clearRect(0, 0, W, H);
    // Housing (control.track), radius 2.
    c.fillStyle = tok(el, "--control-track");
    rrect(c, 0, 0, W, H, 2); c.fill();
    // RMS fill, stepped colour, 1px side inset, flush bottom.
    const fill = Math.round(level * H);
    if (fill > 0) {
      c.fillStyle = stepColor(level);
      c.fillRect(1, H - fill, W - 2, fill);
    }
    // Peak line (control.thumb), x=1..W-1 — skipped when it coincides with fill top.
    const py = H - Math.round(peak * H);
    if (peak > 0.01 && Math.abs(py - (H - fill)) > 0.5) {
      c.strokeStyle = tok(el, "--control-thumb");
      c.lineWidth = 1;
      c.beginPath(); c.moveTo(1, py + 0.5); c.lineTo(W - 1, py + 0.5); c.stroke();
    }
    // Held peak (2px, #FF6464, → accent.error above 0.9), full width.
    if (held > 0.01) {
      const hy = H - Math.round(held * H);
      c.strokeStyle = held > 0.9 ? tok(el, "--accent-error") : "#FF6464";
      c.lineWidth = 2;
      c.beginPath(); c.moveTo(0, hy); c.lineTo(W, hy); c.stroke();
    }
  }

  // Feed one instantaneous level (0..1). The single input drives both the RMS
  // body and the peak envelope (as native does when rms==peak); attack is
  // instant, release follows meter_decay, the held peak sticks for peak_hold.
  function push(v) {
    v = clamp(v, 0, 1);
    const now = performance.now();
    const dt = (now - last) / 1000; last = now;
    updateBallistics(bal, v, v, dt);
    draw();
  }

  el.push = push;
  el.resize = () => { el._ctx = sizeCanvas(cv, width, height); draw(); };
  el.reset = () => {
    bal.displayRms = bal.displayPeak = bal.heldPeak = bal.holdCounter = 0;
    last = performance.now();
    draw();
  };
  // Test/inspection hook: read the current envelope state.
  el.ballistics = bal;
  mountRepaint(el, draw);
  draw();
  return el;
}
