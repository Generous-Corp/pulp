// widgets/combo.js — canvas-drawn closed field + token-styled dropdown matching
// the native ComboBox (core/view/src/ui_components.cpp:69-217).
// See tokens/WIDGETS.md "ComboBox". Used for `choice` parameters.

import {
  FONT, tok, makeCanvas, clamp,
  mountRepaint, isKbNav, setFocusedWidget, clearFocusedWidget, initModality,
} from "./base.js";

export function createCombo({ param, value, onChange, width = 150, height = 28 }) {
  initModality();
  let focused = false;
  const labels = param.labels || [];
  const q = (param.discreteStep && param.discreteStep > 0) ? param.discreteStep
          : (param.step && param.step > 0) ? param.step : 1;
  const valueForIndex = (i) => param.minValue + i * q;
  const indexForValue = (v) => clamp(Math.round((v - param.minValue) / q), 0, labels.length - 1);
  let idx = indexForValue(value);

  const el = document.createElement("div");
  el.className = "pw-combo";
  el.tabIndex = 0;
  el.setAttribute("role", "combobox");
  el.setAttribute("aria-haspopup", "listbox");
  el.setAttribute("aria-expanded", "false");
  el.style.width = width + "px";
  el.style.height = height + "px";
  el.style.position = "relative";
  el.style.cursor = "pointer";

  const base_h = Math.min(height, 28);
  const { cv, ctx, w, h } = makeCanvas(width, height);
  el.appendChild(cv);

  function rrect(x, y, rw, rh, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + rw, y, x + rw, y + rh, r);
    ctx.arcTo(x + rw, y + rh, x, y + rh, r);
    ctx.arcTo(x, y + rh, x, y, r);
    ctx.arcTo(x, y, x + rw, y, r);
    ctx.closePath();
  }

  function drawField() {
    if (!el.isConnected) return;
    ctx.clearRect(0, 0, w, h);
    const cue = focused && isKbNav();
    // Field: bg.surface, radius 4, 1px border — accent-primary while keyboard-focused.
    ctx.fillStyle = tok(el, "--bg-surface");
    rrect(0.5, 0.5, w - 1, base_h - 1, 4); ctx.fill();
    ctx.strokeStyle = cue ? tok(el, "--accent-primary") : tok(el, "--control-border");
    ctx.lineWidth = 1;
    rrect(0.5, 0.5, w - 1, base_h - 1, 4); ctx.stroke();

    // Text: text.primary, 12px, left pad 8, baseline base_h*0.5 + 12*0.34.
    let fs = 12;
    const avail = w - 30;
    ctx.textAlign = "left";
    ctx.textBaseline = "alphabetic";
    ctx.fillStyle = tok(el, "--text-primary");
    let text = labels[idx] ?? "";
    ctx.font = `${fs}px ${FONT}`;
    while (fs > 9 && ctx.measureText(text).width > avail) { fs -= 0.5; ctx.font = `${fs}px ${FONT}`; }
    if (ctx.measureText(text).width > avail) {
      while (text.length > 1 && ctx.measureText(text + "…").width > avail) text = text.slice(0, -1);
      text += "…";
    }
    ctx.fillText(text, 8, base_h * 0.5 + fs * 0.34);

    // Chevron: text.secondary, 1.5px. Native (ui_components.cpp:114-120) uses
    // ay = base_h/2 - 1 with strokes (ax-3, ay-2)→(ax, ay+2)→(ax+3, ay-2):
    // a 6px-wide, 4px-tall "V" whose vertex sits at (w-16, base_h/2+1) and whose
    // arm tips reach (w-16±3, base_h/2-3). (The prior code drew the arms only 2px
    // above the vertex, making the V half its native height.)
    ctx.strokeStyle = tok(el, "--text-secondary");
    ctx.lineWidth = 1.5;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    const ax = w - 16, ay = base_h / 2 - 1;
    ctx.beginPath();
    ctx.moveTo(ax - 3, ay - 2);
    ctx.lineTo(ax, ay + 2);
    ctx.lineTo(ax + 3, ay - 2);
    ctx.stroke();
  }

  // ——— dropdown panel (DOM, token-styled per WIDGETS.md open-dropdown spec)
  let panel = null;
  let hoverRow = -1;

  function closePanel() {
    if (!panel) return;
    panel.remove(); panel = null; hoverRow = -1;
    el.setAttribute("aria-expanded", "false");
    removeEventListener("scroll", closePanel, true);
    removeEventListener("resize", closePanel);
    document.removeEventListener("pointerdown", onDocDown, true);
  }
  function onDocDown(e) { if (panel && !panel.contains(e.target) && e.target !== el && !el.contains(e.target)) closePanel(); }

  function openPanel() {
    if (panel) return;
    const rect = el.getBoundingClientRect();
    panel = document.createElement("div");
    panel.className = "pw-combo-panel";
    panel.setAttribute("role", "listbox");
    const longest = labels.reduce((m, s) => Math.max(m, s.length), 0);
    const panelW = Math.max(rect.width, longest * 7 + 34);
    const left = Math.min(rect.left, window.innerWidth - 14 - panelW);
    panel.style.cssText =
      `position:fixed;left:${Math.max(4, left)}px;top:${rect.bottom + 2}px;` +
      `width:${panelW}px;background:var(--bg-elevated);border:1px solid var(--control-border);` +
      `border-radius:4px;z-index:1000;padding:1px 0;font:12px/1 ${FONT};` +
      `box-shadow:0 6px 20px rgba(0,0,0,.35);`;
    labels.forEach((name, i) => {
      const row = document.createElement("div");
      row.setAttribute("role", "option");
      row.setAttribute("aria-selected", i === idx ? "true" : "false");
      row.dataset.i = String(i);
      row.style.cssText =
        `height:24px;display:flex;align-items:center;padding:0 6px;color:var(--text-primary);` +
        `white-space:nowrap;cursor:pointer;`;
      row.innerHTML =
        `<span class="chk" style="width:16px;color:var(--accent-primary)">${i === idx ? "✓" : ""}</span>` +
        `<span>${name}</span>`;
      row.addEventListener("pointerenter", () => {
        for (const r of panel.children) r.style.background = "";
        row.style.background = "var(--accent-primary)";
        row.querySelector(".chk").style.color = "#ffffff";
        hoverRow = i;
      });
      row.addEventListener("pointerleave", () => {
        row.style.background = "";
        row.querySelector(".chk").style.color = "var(--accent-primary)";
      });
      row.addEventListener("pointerdown", (e) => { e.preventDefault(); select(i); closePanel(); });
      panel.appendChild(row);
    });
    document.body.appendChild(panel);
    el.setAttribute("aria-expanded", "true");
    addEventListener("scroll", closePanel, true);
    addEventListener("resize", closePanel);
    document.addEventListener("pointerdown", onDocDown, true);
  }

  function announce() {
    el.setAttribute("aria-valuetext", labels[idx] ?? "");
  }
  function select(i) {
    i = clamp(i, 0, labels.length - 1);
    if (i !== idx) { idx = i; onChange?.(valueForIndex(idx)); }
    announce(); drawField();
  }

  el.addEventListener("pointerdown", (e) => {
    if (panel && panel.contains(e.target)) return;
    el.focus();
    panel ? closePanel() : openPanel();
    e.preventDefault();
  });
  el.addEventListener("keydown", (e) => {
    switch (e.key) {
      case "ArrowDown": case "ArrowRight": select(idx + 1); e.preventDefault(); break;
      case "ArrowUp": case "ArrowLeft": select(idx - 1); e.preventDefault(); break;
      case "Home": select(0); e.preventDefault(); break;
      case "End": select(labels.length - 1); e.preventDefault(); break;
      case "Enter": case " ": panel ? closePanel() : openPanel(); e.preventDefault(); break;
      case "Escape": closePanel(); break;
    }
  });
  const focusRef = { draw: drawField };
  el.addEventListener("focus", () => { focused = true; setFocusedWidget(focusRef); drawField(); });
  el.addEventListener("blur", () => {
    focused = false; clearFocusedWidget(focusRef); drawField();
    setTimeout(() => { if (panel && !panel.contains(document.activeElement)) closePanel(); }, 0);
  });

  el.setValue = (v) => { idx = indexForValue(v); announce(); drawField(); };
  el.getValue = () => valueForIndex(idx);
  announce();
  mountRepaint(el, drawField);
  drawField();
  return el;
}
