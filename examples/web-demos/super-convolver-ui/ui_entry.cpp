// C ABI for the DSP-free Pulp UI wasm module.
//
// What it mounts is the REAL SuperConvolver editor — examples/super-convolver/
// super_convolver_ui.hpp, the same header the native plugin builds — not a browser
// lookalike. That is possible because the editor's non-parameter dependencies were
// cut down to the four calls of SuperConvolverUiHost; everything else it needs is a
// StateStore and a SpectrumBus, and this file owns one of each. The DSP stays where
// it belongs (the AudioWorklet); nothing under core/audio or examples/super-convolver
// /super_convolver.hpp is linked here.
//
// THE UI -> HOST DIRECTION goes out through Module.onParamChange /
// Module.onGestureBegin / Module.onGestureEnd / Module.onRequestIr (see pulp-ui.js,
// which forwards them to the web-player HostAdapter). THE HOST -> UI DIRECTION comes
// in through pulp_ui_set_param (values), pulp_ui_set_ir / pulp_ui_set_ir_name (the
// plugin's live impulse response), pulp_ui_set_spectrum (its output spectrum) and
// pulp_ui_set_gpu_status (engine stats). Gesture begin/end are separate from the
// value callback because the adapter maps them to the host's undo grouping.
//
// PARAMETERS CROSS THE SEAM TWICE-KEYED. The page knows its parameters by INDEX (the
// position in getParameterInfo()); the editor knows them by their real ParamID
// (kMix … kGpuOnly — it reads store_.get_value(kFlow) by name, so a store keyed by
// anything else is a store it cannot drive). pulp_ui_add_param carries BOTH, and this
// file is the only place that translates: ids inward, indices outward.
//
// THE ABI IS LISTED IN THREE PLACES THAT MUST STAY IN SYNC:
//   1. the EMSCRIPTEN_KEEPALIVE definitions here
//   2. _PULP_WEBUI_EXPORTED_FUNCTIONS in tools/cmake/PulpWebUi.cmake
//   3. pulp-ui.js (mountPulpUi) and browser-test/validate.mjs

#include "super_convolver_web_host.hpp"

#include "super_convolver_ui.hpp"   // the native editor, verbatim

#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/web/web_event_translate.hpp>
#include <pulp/view/window_host.hpp>

#include <choc/text/choc_JSON.h>

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using pulp::examples::SuperConvolverUi;
using pulp::state::ParamID;

// The editor's whole world: parameters, the wet-output spectrum, and the four host
// calls. Constructed at module scope so pulp_ui_add_param can register parameters
// before pulp_ui_init builds the view — the page pushes its table first, exactly as
// a format adapter would call define_parameters() before creating the editor.
pulp::state::StateStore g_store;
pulp::examples::SpectrumBus g_spectrum;
pulp::webui::SuperConvolverWebHost g_host;

std::unique_ptr<SuperConvolverUi> g_ui;
std::unique_ptr<pulp::view::WindowHost> g_window;
uint32_t g_width = 0;
uint32_t g_height = 0;

// index (the page's) <-> id (the editor's), in page order.
std::vector<ParamID>& param_ids() {
    static std::vector<ParamID> ids;
    return ids;
}

ParamID id_for_index(int index) {
    const auto& ids = param_ids();
    if (index < 0 || static_cast<std::size_t>(index) >= ids.size()) return 0;
    return ids[static_cast<std::size_t>(index)];
}

int index_for_id(ParamID id) {
    const auto& ids = param_ids();
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id) return static_cast<int>(i);
    return -1;
}

// A host-originated write is being applied to the store. The store's listener fires
// on every set_value regardless of who called it, so without this the echo the page
// just pushed in would be pushed straight back out — a value that ping-pongs across
// the worklet boundary forever, and (worse) a host automation ramp that the editor
// answers by re-writing every step of it.
bool g_applying_host_write = false;

pulp::state::ListenerToken& value_listener() {
    static pulp::state::ListenerToken token;
    return token;
}

std::string group_thousands(double value) {
    char digits[32];
    std::snprintf(digits, sizeof(digits), "%lld",
                  static_cast<long long>(value < 0 ? 0 : value));
    std::string in(digits);
    std::string out;
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0) out.push_back(',');
        out.push_back(in[i]);
    }
    return out;
}

/// Parses the host's stats blob into the GpuStatus the editor's interface asks for,
/// and renders the same numbers into `line`. The blob is the page's, not this
/// module's: budget_us and rt_percent are DERIVED THERE, by the same arithmetic
/// native gpu_status() uses, so the browser and the native build print the same
/// numbers computed the same way. An unparseable or empty blob yields a default
/// (inactive) status rather than leaving a stale GPU readout on screen.
///
/// The copy carries no speed claim. A measured spike (2026-06-29) showed a competent
/// real-FFT CPU convolver matches or beats this GPU path at every musically plausible
/// setting; the GPU engine is a capability demonstration. Missed GPU deadlines are
/// reported as blocks the CPU net COVERED — never as blocks the GPU produced.
pulp::examples::GpuStatus parse_gpu_status(const char* json, std::string* line) {
    pulp::examples::GpuStatus status;
    if (line) line->clear();
    if (!json || !*json) return status;
    try {
        auto v = choc::json::parse(json);
        if (!v.isObject()) return status;
        const auto text = [&](const char* key) -> std::string {
            return v.hasObjectMember(key) ? v[key].toString() : std::string();
        };
        const auto number = [&](const char* key) -> double {
            return v.hasObjectMember(key) ? v[key].getWithDefault<double>(0.0) : 0.0;
        };

        const std::string note = text("note");
        status.active = text("engine") == "gpu";
        status.backend = text("backend");
        // The page counts blocks the GPU PRODUCED and blocks the CPU net COVERED;
        // a covered block is a missed GPU deadline, which is what `misses` means.
        status.blocks = static_cast<std::uint64_t>(number("produced"));
        status.misses = static_cast<std::uint64_t>(number("covered"));
        status.avg_us = number("avg_us");
        status.budget_us = number("budget_us");
        status.rt_percent = number("rt_percent");
        // rooms / multi are not in the blob: the GPU lane is a page-side worker and
        // does not report its batch shape. The editor reads Rooms from the parameter.

        if (line) {
            if (status.active) {
                char buf[320];
                std::snprintf(
                    buf, sizeof(buf),
                    "Engine: GPU — WGSL compute%s%s · %s blocks on GPU · %s covered by CPU"
                    " · %.0f µs/block (%.1f%% of the %s µs real-time budget)",
                    status.backend.empty() ? "" : " on ", status.backend.c_str(),
                    group_thousands(static_cast<double>(status.blocks)).c_str(),
                    group_thousands(static_cast<double>(status.misses)).c_str(),
                    status.avg_us, status.rt_percent,
                    group_thousands(status.budget_us).c_str());
                *line = buf;
            } else {
                *line = "Engine: CPU — real-FFT PartitionedConvolver (the default, and always the fallback)";
            }
            if (!note.empty()) *line += " · " + note;
        }
        return status;
    } catch (...) {
        return {};
    }
}

}  // namespace

EM_JS(void, pulp_ui_js_param_change, (int index, float value), {
    if (Module.onParamChange) Module.onParamChange(index, value);
});

EM_JS(void, pulp_ui_js_gesture_begin, (int index), {
    if (Module.onGestureBegin) Module.onGestureBegin(index);
});

EM_JS(void, pulp_ui_js_gesture_end, (int index), {
    if (Module.onGestureEnd) Module.onGestureEnd(index);
});

// The module's one outbound COMMAND (declared in super_convolver_web_host.hpp): the
// user clicked the Source chip. A wasm module on a page cannot open a file dialog, so
// the page opens its own picker, decodes the audio, and writes it into the plugin —
// the new IR then arrives back here through pulp_ui_set_ir like any other rebuild.
EM_JS(void, pulp_ui_js_request_ir, (), {
    if (Module.onRequestIr) Module.onRequestIr();
});

extern "C" {

// `id` is the parameter's REAL id (the CLAP/StateStore id the editor names its
// controls by); `index` is the page's position for it. Registering under the id is
// what lets the shared editor drive this store at all.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_add_param(int index, int id, const char* name, float min_value,
                       float max_value, float default_value, const char* unit) {
    auto& ids = param_ids();
    if (index < 0) return;
    if (static_cast<std::size_t>(index) >= ids.size())
        ids.resize(static_cast<std::size_t>(index) + 1, 0);
    ids[static_cast<std::size_t>(index)] = static_cast<ParamID>(id);

    g_store.add_parameter({.id = static_cast<ParamID>(id),
                           .name = name ? name : "",
                           .unit = unit ? unit : "",
                           .range = {min_value, max_value, default_value}});
    g_store.set_value(static_cast<ParamID>(id), default_value);
}

EMSCRIPTEN_KEEPALIVE
int pulp_ui_init(const char* canvas_selector, int width, int height, float dpr) {
    if (g_window) return 0;
    (void) dpr;  // the browser host reads devicePixelRatio itself.

    g_width = static_cast<uint32_t>(width > 0 ? width : 1);
    g_height = static_cast<uint32_t>(height > 0 ? height : 1);

    // Editor -> page. ParameterEdit (the editor's every control) drives the store's
    // gesture + value path, so subscribing to the store IS subscribing to the editor;
    // there is no second UI-facing callback to keep in sync.
    g_store.set_gesture_callbacks(
        [](ParamID id) {
            const int index = index_for_id(id);
            if (index >= 0) pulp_ui_js_gesture_begin(index);
        },
        [](ParamID id) {
            const int index = index_for_id(id);
            if (index >= 0) pulp_ui_js_gesture_end(index);
        });
    value_listener() = g_store.add_listener(
        [](ParamID id, float value) {
            if (g_applying_host_write) return;   // the echo, not an edit — see the flag
            const int index = index_for_id(id);
            if (index >= 0) pulp_ui_js_param_change(index, value);
        },
        pulp::state::ListenerThread::Main);

    g_ui = std::make_unique<SuperConvolverUi>(g_store, g_spectrum, g_host);

    pulp::view::web::install_browser_window_host(
        canvas_selector ? canvas_selector : "#canvas");

    pulp::view::WindowOptions options;
    options.title = "SuperConvolver";
    options.width = static_cast<float>(g_width);
    options.height = static_cast<float>(g_height);
    options.use_gpu = true;

    g_window = pulp::view::WindowHost::create(*g_ui, options);
    if (!g_window) return 0;

    g_window->show();
    // The browser host owns the requestAnimationFrame loop; run_event_loop()
    // arms it and returns immediately (the browser owns the real event loop, so
    // blocking here would deadlock the page).
    g_window->run_event_loop();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void pulp_ui_resize(int width, int height, float dpr) {
    if (!g_window) return;
    (void) dpr;  // handle_resize() re-reads devicePixelRatio and resizes the
                 // canvas backing store itself.
    g_width = static_cast<uint32_t>(width > 0 ? width : 1);
    g_height = static_cast<uint32_t>(height > 0 ? height : 1);
    pulp::view::web::notify_browser_host_resized(static_cast<float>(g_width),
                                                 static_cast<float>(g_height));
}

// Host -> UI. The plugin changing its own value (preset load, host automation, the
// editor's own edit echoed back) lands here. Guarded so the store listener does not
// bounce it straight back out as if the user had just moved that control.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_param(int index, float value) {
    const ParamID id = id_for_index(index);
    if (id == 0) return;
    g_applying_host_write = true;
    g_store.set_value(id, value);
    g_applying_host_write = false;
    if (g_window) g_window->mark_dirty();
}

EMSCRIPTEN_KEEPALIVE
float pulp_ui_get_param(int index) {
    const ParamID id = id_for_index(index);
    return id == 0 ? 0.0f : g_store.get_value(id);
}

// Host -> UI. The plugin's live impulse response — post-normalize, post-window, the
// one it is ACTUALLY convolving with, which is why the hero waveform morphs as Size
// rebuilds the tail and not only when a file is loaded. `samples` is a float32 view
// of the module's own heap (the page copies it in), read-only and not retained.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_ir(const float* samples, int count) {
    g_host.set_ir(samples, count);
    if (g_window) g_window->mark_dirty();
}

// Host -> UI. Display name of the loaded IR; empty restores "Synthetic room". A NAME,
// not a path — see SuperConvolverWebHost::set_ir_name.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_ir_name(const char* name) {
    g_host.set_ir_name(name ? name : "");
    if (g_window) g_window->mark_dirty();
}

// Host -> UI. The wet output's magnitude spectrum in dB, from the page's AnalyserNode
// (natively this is published from the audio thread through the same SpectrumBus).
//
// The editor plots kSpectrumBins LINEAR bins on a log-frequency axis. An AnalyserNode
// with fftSize 512 gives exactly that many, and pulp-ui.js asks for that size — but
// the count is the page's to choose, so any other is folded to 256 by taking the
// LOUDEST source bin in each output bucket (a mean would wash out narrow peaks, which
// are the interesting part of a spectrum). Fewer than 256 bins are held.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_spectrum(const float* bins, int count) {
    if (!bins || count <= 0) return;
    constexpr int kBins = pulp::examples::kSpectrumBins;
    pulp::examples::SpectrumFrame frame{};
    for (int i = 0; i < kBins; ++i) {
        int lo = static_cast<int>(static_cast<long long>(i) * count / kBins);
        int hi = static_cast<int>(static_cast<long long>(i + 1) * count / kBins);
        if (hi <= lo) hi = lo + 1;
        if (hi > count) hi = count;
        if (lo >= count) lo = count - 1;
        float peak = bins[lo];
        for (int s = lo + 1; s < hi; ++s) peak = std::max(peak, bins[s]);
        frame[static_cast<std::size_t>(i)] = peak;
    }
    g_spectrum.write(frame);
    if (g_window) g_window->mark_dirty();
}

// Force a synchronous paint. The pixel fixture calls this immediately before
// reading the WebGL drawing buffer back, so the readback happens in the same
// task as the draw.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_repaint() {
    if (g_window) g_window->repaint();
}

// Is the GPU surface renderable right now? 0 while the browser has taken the
// WebGL context away (context-cap eviction, GPU-process crash, a backgrounded
// mobile tab) and 1 again once it has been restored and Ganesh rebuilt. A host
// page can use it to show a "recovering" state instead of a frozen UI; the
// browser fixture uses it to assert the loss/restore path with a REAL
// WEBGL_lose_context revocation.
EMSCRIPTEN_KEEPALIVE
int pulp_ui_gpu_available() {
    return pulp::view::web::browser_host_gpu_available() ? 1 : 0;
}

// Host -> UI. `json` is the page's stats blob (engine / backend / produced /
// covered / avg_us / budget_us / rt_percent / note). The page pushes this on a
// setInterval, NOT a rAF: a backgrounded tab throttles rAF, and that is exactly when
// misses are most interesting.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_gpu_status(const char* json) {
    std::string line;
    g_host.set_gpu_status(parse_gpu_status(json, &line), std::move(line));
    if (g_window) g_window->mark_dirty();
}

// Root-relative rect of a widget, written as [x, y, w, h] floats. `index` is a page
// parameter index; `kind` 0 = the control that drives it, 1 = its name/value text,
// 2 = the header engine chip (index ignored). Returns 0 when the index names a
// parameter this editor has no control for — Bypass and GPU-only are host chrome,
// not widgets here.
//
// Kind 2 was the stub panel's status LINE. The real editor has no status strip: the
// engine chip is the nearest live readout it has, and is what the fixture's "does a
// pushed host update render as ink" check now measures.
EMSCRIPTEN_KEEPALIVE
int pulp_ui_widget_rect(int index, int kind, float* out_rect) {
    if (!g_ui || !out_rect) return 0;
    pulp::view::Rect r;
    bool ok = false;
    if (kind == 2) {
        r = g_ui->engine_rect();
        ok = r.width > 0.0f && r.height > 0.0f;
    } else {
        const ParamID id = id_for_index(index);
        if (id == 0) return 0;
        ok = (kind == 0) ? g_ui->slider_track_for_param(id, &r)
                         : g_ui->slider_label_for_param(id, &r);
    }
    if (!ok) return 0;
    out_rect[0] = r.x;
    out_rect[1] = r.y;
    out_rect[2] = r.width;
    out_rect[3] = r.height;
    return 1;
}

// PNG of the live view tree rendered through Skia's CPU raster surface — the
// same canvas / TextShaper / font path the GPU surface paints with, so a
// zero-width-label or null-SkFontMgr regression shows up here too. The caller
// owns the returned buffer and must _free() it.
EMSCRIPTEN_KEEPALIVE
int pulp_ui_capture_png(uint8_t** out_ptr, int* out_len) {
    if (!g_ui || !out_ptr || !out_len) return 0;
    auto png = pulp::view::render_to_png(*g_ui, g_width, g_height, 1.0f,
                                         pulp::view::ScreenshotBackend::skia);
    if (png.empty()) return 0;
    auto* buffer = static_cast<uint8_t*>(std::malloc(png.size()));
    if (!buffer) return 0;
    std::memcpy(buffer, png.data(), png.size());
    *out_ptr = buffer;
    *out_len = static_cast<int>(png.size());
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void pulp_ui_shutdown() {
    g_window.reset();
    g_ui.reset();
    pulp::view::WindowHost::clear_factory();
}

}  // extern "C"
