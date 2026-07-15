// pulp-ui.js — the glue between the Pulp UI wasm module and a web-player
// HostAdapter.
//
// The module paints the plugin's REAL editor (examples/super-convolver/
// super_convolver_ui.hpp — the same header the desktop plugin builds). That editor
// draws three live things the generated parameter grid never had: the plugin's
// impulse response, its output spectrum, and the acoustic field. None of them are
// parameters, and none of them can be read out of a wasm module that has no DSP in
// it — so feeding them is this file's job. Everything below is plumbing across the
// two directions of the seam:
//
//   UI  -> host : Module.onParamChange / onGestureBegin / onGestureEnd
//                 -> adapter.setParameterValue (gesture callbacks bracket the
//                    edit so the host can group it for undo).
//                 Module.onRequestIr -> the page's file picker (the module has no
//                    filesystem and cannot open one).
//   host -> UI  : adapter.onParamsChanged -> _pulp_ui_set_param.
//                 adapter.onIrChanged     -> _pulp_ui_set_ir   (the hero waveform).
//                 an AnalyserNode         -> _pulp_ui_set_spectrum.
//
// The adapter contract is packages/pulp-web-player/src/adapters/adapter.d.ts.
// Parameter VALUES cross the seam in real units; the module normalizes.
// Parameter INDICES here are positions in the getParameterInfo() array — but the
// module ALSO needs the adapter's numeric `id`, because the editor names its
// controls by the plugin's real parameter ids (kMix, kSize, …) and a store keyed by
// anything else is a store it cannot drive. Both cross in _pulp_ui_add_param; the
// module translates (see ui_entry.cpp), and every other call here stays index-keyed.

import { decodeIrFile, buildPcmIrBlob } from "./ir-source.js";

const DEFAULT_MODULE_URL = "./PulpSuperConvolverUi.js";

// 512-point FFT = 256 bins = pulp::examples::kSpectrumBins exactly, so the frame the
// editor reads is the one the analyser produced, unresampled. The dB window matches
// the editor's own normalization ((dB + 90) / 90), so a bin at the floor draws at the
// floor. ~30 Hz on a setInterval, not rAF: the spectrum is a readout, not an
// animation, and the editor already repaints continuously for the field.
const SPECTRUM_FFT_SIZE = 512;
const SPECTRUM_HZ = 30;

export async function mountPulpUi(canvasEl, adapter, opts = {}) {
    if (!canvasEl || !canvasEl.id) {
        throw new Error("mountPulpUi: the canvas element needs an id (the module binds by selector)");
    }
    const moduleUrl = opts.moduleUrl || DEFAULT_MODULE_URL;
    // { parseContainer, buildContainer } from the player's plugin-state module. The
    // page passes it rather than this file importing it, because the two demo pages
    // sit at different depths under the site root and there is exactly one copy of
    // the container format — see ir-source.js. Without it the Source chip cannot
    // write the decoded IR into the plugin, so the picker is not offered at all.
    const pluginState = opts.pluginState || null;
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
            p.id,
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

    // This editor has only horizontal sliders and taps — no vertical drags — and it
    // is embedded in a scrollable page. Opt into `pan-y` (the input layer defaults a
    // control surface to `touch-action:none`) so a vertical touch-drag over the plugin
    // scrolls the PAGE, while a horizontal drag still adjusts a slider and a tap still
    // hits a control. Without this the canvas swallowed every touch and the page could
    // not be scrolled from on top of the plugin.
    canvasEl.style.touchAction = "pan-y";

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

    // ── the impulse response ────────────────────────────────────────────────────
    // The plugin's LIVE IR — post-normalize, post-window, the one it is actually
    // convolving with — published by the wasm plugin (pulp_ir_*), polled by the
    // worklet on its non-realtime tick, latched and re-emitted by the adapter. It
    // fires again on every rebuild, which is what makes the hero waveform morph as
    // Size regrows the tail rather than only when a file is loaded.
    //
    // Chained, not assigned: the GPU demo page hangs its own handler here first (it
    // forwards the same IR to the WebGPU worker, and the two engines must convolve
    // with the same kernel or the CPU net is no longer a substitute for a missed
    // block). Assigning a handler also REPLAYS the latest IR synchronously, so a
    // late subscriber — this one — is not punished for being late.
    const pushIr = (ir) => {
        if (!ir || !ir.length) return;
        const bytes = ir.length * 4;
        const ptr = Module._malloc(bytes);
        Module.HEAPF32.set(ir, ptr >> 2);
        Module._pulp_ui_set_ir(ptr, ir.length);
        Module._free(ptr);
    };
    const previousOnIrChanged = adapter.onIrChanged;
    const supportsIr = "onIrChanged" in adapter;
    if (supportsIr) {
        adapter.onIrChanged = (ir) => {
            pushIr(ir);
            if (previousOnIrChanged) previousOnIrChanged(ir);
        };
        // The setter replays only when a handler is newly assigned; if one was already
        // installed the latch has not been re-fired, so take it from the adapter.
        if (adapter.impulseResponse) pushIr(adapter.impulseResponse);
    }

    const setIrName = (name) => {
        const s = cstr(name || "");
        Module._pulp_ui_set_ir_name(s);
        Module._free(s);
    };

    // ── the output spectrum ─────────────────────────────────────────────────────
    // Natively the plugin publishes its wet-output spectrum from the audio thread
    // through a lock-free bus. Nothing crosses the worklet boundary here, so the
    // page measures the SAME signal from the outside: an AnalyserNode tapped off the
    // plugin's output. It is a fan-out tap, not an insert — the analyser is a
    // terminal node, so the audio the user hears is untouched.
    let analyser = null;
    let spectrumTimer = 0;
    let spectrumPtr = 0;
    const node = adapter.audioNode;
    if (node && node.context && typeof node.connect === "function") {
        analyser = node.context.createAnalyser();
        analyser.fftSize = SPECTRUM_FFT_SIZE;
        analyser.minDecibels = -90;
        analyser.maxDecibels = 0;
        analyser.smoothingTimeConstant = 0.6;
        node.connect(analyser);

        const bins = new Float32Array(analyser.frequencyBinCount);
        // One scratch buffer for the module's lifetime: allocating and freeing it 30
        // times a second would churn the heap for no benefit.
        spectrumPtr = Module._malloc(bins.length * 4);
        spectrumTimer = setInterval(() => {
            analyser.getFloatFrequencyData(bins);
            Module.HEAPF32.set(bins, spectrumPtr >> 2);
            Module._pulp_ui_set_spectrum(spectrumPtr, bins.length);
        }, Math.round(1000 / SPECTRUM_HZ));
    }

    // ── "load an impulse response" ──────────────────────────────────────────────
    // The editor's Source chip. A wasm module on a page cannot open a file dialog,
    // so the module asks (Module.onRequestIr) and the page answers: pick, decode
    // with the plugin's OWN AudioContext (so the PCM already arrives at the session
    // rate), and hand the plugin the samples through the SDK's plugin-state
    // container — getState → swap the plugin-owned blob for an SCv2 "Pcm" record →
    // setState. That is the identical path the page's own "Load impulse response…"
    // affordance uses (ir-source.js), works the same on WAM and WebCLAP, and the
    // loaded IR survives a state save/restore because it IS the state.
    //
    // The new IR then comes BACK through onIrChanged, so the hero waveform redraws
    // from what the plugin actually built — never from the file we just decoded.
    let filePicker = null;
    if (pluginState && node && node.context) {
        const writeIr = async (file) => {
            const ir = await decodeIrFile(node.context, file);
            if (!ir.mono.length) throw new Error("the file decoded to no audio");
            const current = await adapter.getState();
            let paramBytes = new Uint8Array(0);
            try { paramBytes = pluginState.parseContainer(current).params; } catch { /* bare blob */ }
            await adapter.setState(
                pluginState.buildContainer(paramBytes, buildPcmIrBlob(ir.mono, ir.rate)));
            setIrName(file.name);
        };

        filePicker = document.createElement("input");
        filePicker.type = "file";
        filePicker.accept = "audio/*,.wav,.aif,.aiff,.flac,.mp3,.ogg";
        // NOT `hidden`/`display:none`: iOS Safari silently ignores .click() on a
        // file input that is not in the render tree, which is exactly why the Source
        // chip opened the picker on desktop but did nothing on mobile Safari. Keep it
        // in the layout but visually gone and out of the way.
        filePicker.style.cssText =
            "position:fixed;top:0;left:0;width:1px;height:1px;opacity:0;pointer-events:none;border:0;padding:0;";
        filePicker.addEventListener("change", () => {
            const file = filePicker.files && filePicker.files[0];
            filePicker.value = "";   // so re-picking the same file fires `change` again
            if (file) writeIr(file).catch((err) => console.warn("IR load failed:", err));
        });
        document.body.appendChild(filePicker);
        Module.onRequestIr = () => filePicker.click();
    }

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
        // The IR's display name (the Source chip's label). Empty restores the
        // built-in synthetic room. A NAME, not a path — nothing in a browser can
        // open one.
        setIrName,
        // Host -> UI, the engine/miss/budget readout. `stats` is a plain object; the
        // module parses it into the GpuStatus its editor interface asks for (see
        // ui_entry.cpp parse_gpu_status). Pass null to restore the default CPU state.
        setGpuStatus: (stats) => {
            const s = cstr(stats ? JSON.stringify(stats) : "");
            Module._pulp_ui_set_gpu_status(s);
            Module._free(s);
        },
        destroy: () => {
            observer.disconnect();
            if (spectrumTimer) clearInterval(spectrumTimer);
            // Drop the TAP, not just the analyser's (empty) outputs — otherwise the
            // plugin node keeps feeding a node nobody reads.
            if (analyser) { node.disconnect(analyser); analyser.disconnect(); }
            if (spectrumPtr) Module._free(spectrumPtr);
            if (filePicker) filePicker.remove();
            adapter.onParamsChanged = previousOnParamsChanged;
            if (supportsIr) adapter.onIrChanged = previousOnIrChanged;
            Module._pulp_ui_shutdown();
        },
    };
}
