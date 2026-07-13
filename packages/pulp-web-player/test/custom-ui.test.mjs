#!/usr/bin/env node
// custom-ui.test.mjs — the parity guard for mountDemo({ customUi }).
//
// The hook replaces the auto-generated parameter grid and NOTHING ELSE, in the
// SHARED shell — so a WAM demo and a WebCLAP demo get it from one code path and
// cannot drift. These tests run the real shell against a mock host adapter on a
import { document, ok, failed, audioNode, pendingRaf } from "./dom-shim.mjs";

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

  // Rendering the CELLS is not the same as rendering the GRID, and asserting only the
  // former is what let this ship: #params is a CSS grid, the slot reservation overrides
  // it to `display:block` to hold the editor's box, and the fallback restored that
  // override instead of the original value. The cells were all present — stacked
  // vertically, one per row. That is the bug a Safari user actually saw, and the count
  // assertion above is blind to it.
  const paramsEl = root.querySelector("#params");
  ok(paramsEl.style.display !== "block",
     "async-failing customUi: #params is a GRID again, not the reservation's display:block " +
     "(otherwise every generated cell stacks vertically)");
  ok(!paramsEl.style.minHeight,
     "async-failing customUi: the reserved editor height is released (the grid sizes itself)");
  ok(!root.querySelector(".pw-customui-loading"),
     "async-failing customUi: the 'Loading editor…' placeholder is gone");
  ok(Object.keys(globalThis.__widgets).length === PARAMS.length,
     "async-failing customUi: the widget registry is populated (param sync works again)");
  ok(warnings.some((w) => /customUi failed to mount asynchronously/.test(w)),
     "async-failing customUi: failure is logged");
  assertShellIntact(root, "async-failing customUi");
}

// ——— 4d. The slot is RESERVED before the editor arrives.
//
// A real custom UI mounts behind two long awaits (the DSP wasm, then a multi-megabyte
// UI wasm). Without a reservation the panel sits with an empty gap where the controls
// belong and the editor then slams in — the page visibly assembles in stages, which is
// what a first-time visitor reports as "it loads weird". So the shell reserves the box
// at start, and the editor takes that same box over.
//
// The reservation is only correct if it is also REVERSIBLE (4c) — it overrides a CSS
// grid to do its job, so the two halves are tested together.
{
  const factory = (container) => {
    // Never resolves: freezes the mount exactly where a real editor spends its time,
    // so we can look at the panel mid-load instead of after.
    container.appendChild(document.createElement("canvas"));
    return { ready: new Promise(() => {}), destroy: () => {} };
  };
  const { root } = await mount({ customUi: factory });
  await settle();

  const paramsEl = root.querySelector("#params");
  ok(!!root.querySelector(".pw-customui-loading"),
     "reserved slot: the 'Loading editor…' placeholder is showing while the editor loads");
  ok(paramsEl.style.minHeight,
     "reserved slot: the panel already holds the editor's height (no gap, no late reflow)");
  ok(root.querySelectorAll("#params .pw-cell").length === 0,
     "reserved slot: the generated grid is NOT built behind a still-loading editor");
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
