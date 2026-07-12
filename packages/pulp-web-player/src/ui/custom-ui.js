// ui/custom-ui.js — the opt-in custom-UI hook.
//
// mountDemo({ customUi }) lets a consumer replace the auto-generated parameter
// grid with their own renderer (e.g. a Pulp-rendered canvas). The hook replaces
// the GRID AND NOTHING ELSE: the start overlay, the mobile/iOS touch hygiene,
// the on-screen + computer keyboard, the scope, the meter, the safety limiter
// and the PLST state container are shell-owned and keep working around it.
//
// The custom module gets the SAME host adapter the shell drives (adapters/
// adapter.d.ts) — no new host capability, no new ABI surface — and is mounted
// into a container that occupies exactly the grid's slot, so the reserved
// param-grid height (opts.paramRows) still holds the panel at its final height.
//
// Honest degradation is the contract: a module that throws, returns no handle,
// OR fails asynchronously must not take the audio demo down with it. Any failure
// is logged and the shell falls back to the generated grid.
//
// SYNC vs ASYNC failure. The seam is synchronous — the shell needs a handle (or
// null) before it decides whether to build the grid — but a real UI module mounts
// ASYNCHRONOUSLY: it fetches and instantiates wasm and asks for a WebGL2 context,
// neither of which can fail into a synchronous throw. A factory whose mount is
// async therefore returns its handle immediately AND exposes a `ready` promise.
// If `ready` rejects (no WebGL2, wasm fetch failed, init returned 0), we unmount
// and call onFailed(), which is what actually restores the generated grid. Before
// that existed the async path only removed its canvas and a browser without
// WebGL2 was left staring at an EMPTY panel while the docs promised a grid.

const CONTAINER_ID = "custom-ui";
const LOADING_CLASS = "pw-customui-loading";

// Reserve the editor's box the moment the overlay drops — NOT when the module
// finally mounts. Between the click and a mounted Pulp UI there are two long
// awaits (the DSP wasm, then a multi-megabyte UI wasm), and without this the
// panel sits there with an empty gap where the controls belong and then the
// editor slams in: the page visibly assembles in stages, which is exactly what a
// first-time visitor reports as "it loads weird". Called by the shell before it
// awaits the adapter; mountCustomUi() later takes the same box over.
export function reserveCustomUiSlot(host) {
  if (!host || host.querySelector("." + LOADING_CLASS)) return;
  host.style.display = "block";
  host.style.minHeight = "var(--pw-customui-h, 380px)";
  host.style.position = "relative";
  host.appendChild(makeLoadingPlaceholder());
}

function makeLoadingPlaceholder() {
  const el = document.createElement("div");
  el.className = LOADING_CLASS;
  el.setAttribute("aria-live", "polite");
  el.textContent = "Loading editor…";
  Object.assign(el.style, {
    position: "absolute", inset: "0", display: "flex",
    alignItems: "center", justifyContent: "center",
    font: "500 13px/1.4 system-ui, sans-serif",
    color: "var(--text-secondary, #8b949e)",
    // A slow pulse, not a spinner: a spinner beside an otherwise idle page reads
    // as "stuck", a pulse reads as "arriving".
    animation: "pw-customui-pulse 1.6s ease-in-out infinite",
    pointerEvents: "none",
  });
  if (!document.getElementById("pw-customui-css")) {
    const css = document.createElement("style");
    css.id = "pw-customui-css";
    css.textContent = "@keyframes pw-customui-pulse{0%,100%{opacity:.45}50%{opacity:.85}}";
    document.head.appendChild(css);
  }
  return el;
}

// factory(container, adapter, { params })
//   → { destroy(), onParamsChanged?(values, params), ready?: Promise }
// Returns a handle the shell drives, or null when the custom UI could not be
// mounted (caller then builds the generated grid). `onFailed` is invoked when an
// async mount rejects AFTER this function has already returned a handle — the
// caller must then build the generated grid itself.
export function mountCustomUi({ host, adapter, factory, params = [], onFailed = null }) {
  if (factory == null) return null;
  if (typeof factory !== "function") {
    console.warn("customUi: expected a function (container, adapter) → { destroy() } — using the generated parameter grid.");
    return null;
  }

  // The grid slot IS the custom UI's slot. #params is a CSS grid sized for the
  // generated cells; a custom renderer owns the whole box instead, so the grid
  // display is suspended for as long as it is mounted and restored on destroy.
  const gridDisplay = host.style.display;
  const container = document.createElement("div");
  container.id = CONTAINER_ID;
  container.className = "pw-customui";
  host.style.display = "block";

  // The slot was already reserved (and is showing "Loading editor…") if the shell
  // called reserveCustomUiSlot() at start — which it does. Adopt that box rather
  // than making a second one, or the placeholder would be orphaned behind the
  // editor. If nothing reserved it, reserve it now (a consumer driving
  // mountCustomUi directly).
  if (!host.querySelector("." + LOADING_CLASS)) reserveCustomUiSlot(host);
  const placeholder = host.querySelector("." + LOADING_CLASS);

  // The editor itself fades in over the placeholder; the box never changes size.
  container.style.opacity = "0";
  container.style.transition = "opacity 180ms ease-out";
  host.appendChild(container);

  const settle = () => {
    placeholder?.remove();
    host.style.minHeight = "";        // the real editor now dictates the height
    container.style.opacity = "1";
  };

  const unmount = () => {
    container.remove();
    placeholder?.remove();
    host.style.minHeight = "";
    host.style.display = gridDisplay;
  };

  let handle;
  try {
    handle = factory(container, adapter, { params });
  } catch (err) {
    console.warn("customUi threw while mounting — falling back to the generated parameter grid:", err);
    unmount();
    return null;
  }
  if (!handle) {
    console.warn("customUi returned no handle — falling back to the generated parameter grid.");
    unmount();
    return null;
  }
  if (typeof handle.destroy !== "function") {
    console.warn("customUi returned a handle with no destroy() — the shell cannot release it on teardown.");
  }

  let destroyed = false;

  // The async half of the degradation contract (see the header).
  if (handle.ready && typeof handle.ready.then === "function") {
    handle.ready.then(
      () => { if (!destroyed) settle(); },
      (err) => {
        if (destroyed) return;
        destroyed = true;
        console.warn("customUi failed to mount asynchronously — falling back to the generated parameter grid:", err);
        try { handle.destroy?.(); }
        catch (e) { console.warn("customUi destroy() threw during async-failure cleanup:", e); }
        unmount();
        onFailed?.();
      });
  } else {
    // A synchronous custom UI is already on screen, so the placeholder must go
    // now — otherwise it would sit on top of the editor forever.
    settle();
  }

  return {
    container,
    // The plugin changing its OWN parameters (a preset load) is pushed here, the
    // same values/params the generated grid would repaint from.
    paramsChanged(values, infos) {
      if (destroyed) return;
      try { handle.onParamsChanged?.(values, infos); }
      catch (err) { console.warn("customUi onParamsChanged threw:", err); }
    },
    destroy() {
      if (destroyed) return;
      destroyed = true;
      try { handle.destroy?.(); }
      catch (err) { console.warn("customUi destroy() threw:", err); }
      unmount();
    },
  };
}
