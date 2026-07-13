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
    // A breadcrumb trail. A mount that stalls leaves NO error and no console line —
    // the page just says "Loading editor…" forever — so record where we got to.
    const trace = (globalThis.__pulpUiTrace = globalThis.__pulpUiTrace || []);
    const T = (m) => trace.push(`${Math.round(performance.now())}ms ${m}`);

    T("import module JS");
    const createModule = (await import(moduleUrl)).default;

    T("adapter.getParameterInfo()");
    const params = (await adapter.getParameterInfo()) || [];

    T(`got ${params.length} params; createModule()`);
    const Module = await createModule({ canvas: canvasEl });
    T("runtime initialized");

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

    // SIZING — the one thing that must not be guessed.
    //
    // The canvas is styled `width:100%; aspect-ratio:8/5` and mounted in the same
    // tick it is appended. Chrome resolves that box on the first
    // getBoundingClientRect(); SAFARI DOES NOT — it hands back the element's
    // INTRINSIC 300x150 default. Initialising the editor at 300px wide makes Yoga
    // wrap the controls into a vertical column and the editor never reaches its
    // real size: a Safari user sees the knobs stacked and the editor "stuck
    // loading". Reported from a live page, 2026-07-12.
    //
    // So never trust the canvas's own box when it is still the untouched default.
    // Measure the PARENT — a plain block, always laid out — and derive the height
    // from the canvas's declared aspect-ratio (falling back to the 8:5 the page
    // asks for). The ResizeObserver below corrects both once layout settles.
    const CANVAS_DEFAULT_W = 300, CANVAS_DEFAULT_H = 150;   // the HTML intrinsic size
    const rect = canvasEl.getBoundingClientRect();
    const parentRect = canvasEl.parentElement?.getBoundingClientRect();
    const declaredRatio = (() => {
        const ar = getComputedStyle(canvasEl).aspectRatio;               // e.g. "8 / 5"
        const m = ar && ar.match(/^\s*([\d.]+)\s*\/\s*([\d.]+)\s*$/);
        return m && +m[2] ? +m[1] / +m[2] : 8 / 5;
    })();

    let width = Math.round(rect.width);
    if (!width || width === CANVAS_DEFAULT_W) {
        width = Math.round(parentRect?.width || canvasEl.clientWidth || 640);
    }
    let height = Math.round(rect.height);
    if (!height || height === CANVAS_DEFAULT_H) {
        height = Math.round(width / declaredRatio);
    }
    width = Math.max(1, width);
    height = Math.max(1, height);

    const selector = cstr("#" + canvasEl.id);
    T(`_pulp_ui_init(${width}x${height})`);
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

    // Observe the PARENT as well as the canvas: on a browser that has not resolved
    // the canvas's percentage box yet (see the sizing note above), the canvas's own
    // entry keeps reporting the intrinsic default and the editor would stay stuck at
    // 300px forever. The parent is a plain block and always reports its real width,
    // so the first parent callback is what rescues it.
    const applySize = (w, h) => {
        w = Math.max(1, Math.round(w));
        h = Math.max(1, Math.round(h || w / declaredRatio));
        if (w === CANVAS_DEFAULT_W && h === CANVAS_DEFAULT_H) return;   // the untouched default: not a real measurement
        Module._pulp_ui_resize(w, h, window.devicePixelRatio || 1);
    };
    const observer = new ResizeObserver((entries) => {
        for (const entry of entries) {
            const box = entry.contentRect;
            if (entry.target === canvasEl) applySize(box.width, box.height);
            else applySize(box.width, box.width / declaredRatio);        // the parent: derive the height
        }
    });
    observer.observe(canvasEl);
    if (canvasEl.parentElement) observer.observe(canvasEl.parentElement);
    T("MOUNTED");

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
