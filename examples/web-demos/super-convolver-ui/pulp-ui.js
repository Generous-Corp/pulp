// pulp-ui.js — the glue between the Pulp UI wasm module and a web-player
// HostAdapter.
//
// The player's custom-UI hook calls mountPulpUi(canvasEl, adapter). Everything
// below is plumbing across the two directions of the seam:
//
//   UI  -> host : Module.onParamChange / onGestureBegin / onGestureEnd
//                 -> adapter.setParameterValue (gesture callbacks bracket the
//                    edit so the host can group it for undo).
//   host -> UI  : adapter.onParamsChanged -> _pulp_ui_set_param.
//
// The adapter contract is packages/pulp-web-player/src/adapters/adapter.d.ts.
// Parameter VALUES cross the seam in real units; the module normalizes.
// Parameter INDICES here are positions in the getParameterInfo() array; the
// adapter's own numeric `id` never enters the wasm module.

const DEFAULT_MODULE_URL = "./PulpSuperConvolverUi.js";

export async function mountPulpUi(canvasEl, adapter, opts = {}) {
    if (!canvasEl || !canvasEl.id) {
        throw new Error("mountPulpUi: the canvas element needs an id (the module binds by selector)");
    }
    const moduleUrl = opts.moduleUrl || DEFAULT_MODULE_URL;
    const createModule = (await import(moduleUrl)).default;

    const params = (await adapter.getParameterInfo()) || [];

    const Module = await createModule({ canvas: canvasEl });

    const cstr = (s) => Module.stringToNewUTF8(s || "");
    const slotOf = new Map(); // adapter param id -> slot index
    params.forEach((p, slot) => {
        slotOf.set(p.id, slot);
        const name = cstr(p.label || `Param ${p.id}`);
        const unit = cstr(p.unit || "");
        Module._pulp_ui_add_param(
            slot,
            name,
            typeof p.minValue === "number" ? p.minValue : 0,
            typeof p.maxValue === "number" ? p.maxValue : 1,
            typeof p.defaultValue === "number" ? p.defaultValue : 0,
            unit,
        );
        Module._free(name);
        Module._free(unit);
    });

    // UI -> host.
    Module.onParamChange = (slot, value) => {
        const p = params[slot];
        if (p) adapter.setParameterValue(p.id, value);
    };
    Module.onGestureBegin = (slot) => {
        const p = params[slot];
        if (p && typeof adapter.beginGesture === "function") adapter.beginGesture(p.id);
    };
    Module.onGestureEnd = (slot) => {
        const p = params[slot];
        if (p && typeof adapter.endGesture === "function") adapter.endGesture(p.id);
    };

    const rect = canvasEl.getBoundingClientRect();
    const width = Math.max(1, Math.round(rect.width || canvasEl.clientWidth || 640));
    const height = Math.max(1, Math.round(rect.height || canvasEl.clientHeight || 360));
    const selector = cstr("#" + canvasEl.id);
    const ok = Module._pulp_ui_init(selector, width, height, window.devicePixelRatio || 1);
    Module._free(selector);
    if (!ok) throw new Error("mountPulpUi: _pulp_ui_init failed (no window host / no WebGL2 context)");

    // host -> UI. The plugin changing its own parameters (preset load, host
    // automation) is the only way values arrive from the other side.
    const previousOnParamsChanged = adapter.onParamsChanged;
    adapter.onParamsChanged = (values, infos) => {
        const list = infos && infos.length ? infos : params;
        for (let i = 0; i < list.length && i < values.length; i++) {
            const slot = slotOf.has(list[i].id) ? slotOf.get(list[i].id) : i;
            Module._pulp_ui_set_param(slot, values[i]);
        }
        if (previousOnParamsChanged) previousOnParamsChanged(values, infos);
    };

    const observer = new ResizeObserver((entries) => {
        for (const entry of entries) {
            const box = entry.contentRect;
            const w = Math.max(1, Math.round(box.width));
            const h = Math.max(1, Math.round(box.height));
            Module._pulp_ui_resize(w, h, window.devicePixelRatio || 1);
        }
    });
    observer.observe(canvasEl);

    return {
        module: Module,
        setParam: (id, value) => {
            const slot = slotOf.has(id) ? slotOf.get(id) : id;
            Module._pulp_ui_set_param(slot, value);
        },
        destroy: () => {
            observer.disconnect();
            adapter.onParamsChanged = previousOnParamsChanged;
            Module._pulp_ui_shutdown();
        },
    };
}
