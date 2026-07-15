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
    const allParams = (await adapter.getParameterInfo()) || [];

    // Parameters the PAGE owns, and the plugin's UI must therefore not also draw.
    //
    // The GPU page renders Engine as a <select> and "GPU only" as a checkbox — real
    // controls, with words on them, that say what they do. Drawing the same two params as
    // KNOBS inside the plugin gives you two controls for one parameter, and the knob is
    // the worse of the two: a continuous dial for a switch, with no detents and no labels,
    // and the page had one dial reading "0.00" whose meaning you could not possibly guess.
    // (The plugin now declares them ParamKind::Toggle, which fixes how a DAW draws them;
    // this fixes the duplicate.)
    const hidden = new Set((opts.hideParams || []).map((n) => String(n).toLowerCase()));
    const params = allParams.filter((p) => !hidden.has(String(p.label || "").toLowerCase()));

    T(`got ${params.length} params; createModule()`);
    const Module = await createModule({ canvas: canvasEl });
    T("runtime initialized");

    const cstr = (s) => Module.stringToNewUTF8(s || "");
    const slotOf = new Map(); // adapter param id -> slot index
    params.forEach((p, slot) => {
        slotOf.set(p.id, slot);
        const name = cstr(p.label || `Param ${p.id}`);
        const unit = cstr(p.unit || "");
        // An ON/OFF parameter: stepped, and its whole range is 0..1. The plugin declares it
        // ParamKind::Toggle; CLAP carries that as IS_STEPPED, and the adapter surfaces it as
        // step === 1. It gets a switch in the UI, not a knob — a dial for a two-state value
        // has no detents and no on/off affordance, and Bypass shipped exactly that way.
        const min = typeof p.minValue === "number" ? p.minValue : 0;
        const max = typeof p.maxValue === "number" ? p.maxValue : 1;
        const isToggle = p.step === 1 && min === 0 && max === 1;
        Module._pulp_ui_add_param(
            slot,
            name,
            min,
            max,
            typeof p.defaultValue === "number" ? p.defaultValue : 0,
            unit,
            isToggle ? 1 : 0,
        );
        Module._free(name);
        Module._free(unit);
    });

    // The live value of every param the ADAPTER knows about — including the ones the editor
    // does not draw. The re-emit below has to be shaped exactly like a host echo, and a host
    // echo carries the whole list.
    const indexOfId = new Map(allParams.map((p, i) => [p.id, i]));
    const liveValues = allParams.map((p) => (typeof p.defaultValue === "number" ? p.defaultValue : 0));

    // UI -> host.
    Module.onParamChange = (slot, value) => {
        const p = params[slot];
        if (!p) return;
        adapter.setParameterValue(p.id, value);

        // A UI-INITIATED WRITE GETS NO ECHO BACK. The plugin only reports a parameter it
        // moved itself (preset, automation), so anything listening on the adapter's param
        // feed goes stale the instant the user touches a knob — the page's own chrome, and
        // the IR blurb that says how Size is cutting the impulse, both froze at their last
        // host-sent value while the knob turned underneath them. The feed is the page's only
        // view of parameter state; it has to be true no matter who moved the value. So say
        // it: re-emit, in the host's shape.
        const i = indexOfId.get(p.id);
        if (i != null) liveValues[i] = value;
        if (adapter.onParamsChanged) adapter.onParamsChanged(liveValues.slice(), allParams);
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
    // INTRINSIC 300x150 default. Initializing the editor at 300px wide makes Yoga
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
    // The CSS box is `aspect-ratio` AND `min-height`, so its used height is the LARGER of
    // the two. Deriving the fallback from the ratio alone hands the module a 115px backing
    // store for a box the browser then paints 250px tall — and every knob comes out an
    // OVAL, with the labels sheared through each other. It shipped that way on a phone,
    // where the ratio's height is smallest and the floor always wins.
    const minH = parseFloat(getComputedStyle(canvasEl).minHeight) || 0;
    let height = Math.round(rect.height);
    if (!height || height === CANVAS_DEFAULT_H) {
        height = Math.round(Math.max(width / declaredRatio, minH));
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
            const id = list[i].id;
            const j = indexOfId.get(id);
            if (j != null) liveValues[j] = values[i];
            // Only params the editor DRAWS have a slot. Falling back to the array index for
            // the others sends a hidden param's value into whatever knob happens to sit at
            // that index — with Engine and GPU-only hidden on the GPU page, a host echo would
            // have jammed Mix full of Engine's value.
            if (!slotOf.has(id)) continue;
            Module._pulp_ui_set_param(slotOf.get(id), values[i]);
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
            if (entry.target === canvasEl) {
                const box = entry.contentRect;
                applySize(box.width, box.height);
            } else {
                // The PARENT resized. Do NOT derive the canvas height from the aspect ratio
                // here: the box is `aspect-ratio` AND `min-height`, so its used height is the
                // larger of the two, and deriving from the ratio alone overwrites a correct
                // 250px box with a 115px one. The backing store then no longer matches what
                // the browser paints, and every knob renders as an OVAL with the labels
                // sheared through each other — on a phone, where the floor always wins.
                //
                // Ask the canvas what it actually is.
                const r = canvasEl.getBoundingClientRect();
                applySize(r.width, r.height);
            }
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
