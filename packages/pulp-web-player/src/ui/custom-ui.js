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
  host.appendChild(container);

  const unmount = () => {
    container.remove();
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
    handle.ready.catch((err) => {
      if (destroyed) return;
      destroyed = true;
      console.warn("customUi failed to mount asynchronously — falling back to the generated parameter grid:", err);
      try { handle.destroy?.(); }
      catch (e) { console.warn("customUi destroy() threw during async-failure cleanup:", e); }
      unmount();
      onFailed?.();
    });
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
