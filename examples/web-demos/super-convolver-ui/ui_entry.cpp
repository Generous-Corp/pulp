// C ABI for the DSP-free Pulp UI wasm module.
//
// THE UI -> HOST DIRECTION goes out through Module.onParamChange /
// Module.onGestureBegin / Module.onGestureEnd (see pulp-ui.js, which forwards
// them to the web-player HostAdapter). THE HOST -> UI DIRECTION comes in through
// pulp_ui_set_param. Gesture begin/end are separate from the value callback
// because the adapter maps them to the host's undo grouping.
//
// The parameter table is pushed in with pulp_ui_add_param before pulp_ui_init:
// the module owns no descriptor of its own, so the same binary serves the WAM
// and the WebCLAP demo unchanged.
//
// THE ABI IS LISTED IN THREE PLACES THAT MUST STAY IN SYNC:
//   1. the EMSCRIPTEN_KEEPALIVE definitions here
//   2. _PULP_WEBUI_EXPORTED_FUNCTIONS in tools/cmake/PulpWebUi.cmake
//   3. pulp-ui.js (mountPulpUi) and browser-test/validate.mjs

#include "super_convolver_web_ui.hpp"

#include <pulp/view/screenshot.hpp>
#include <pulp/view/web/web_event_translate.hpp>
#include <pulp/view/window_host.hpp>

#include <choc/text/choc_JSON.h>

#include <emscripten.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using pulp::webui::ParamSpec;
using pulp::webui::SuperConvolverWebUi;

std::vector<ParamSpec>& pending_params() {
    static std::vector<ParamSpec> params;
    return params;
}

std::unique_ptr<SuperConvolverWebUi> g_ui;
std::unique_ptr<pulp::view::WindowHost> g_host;
uint32_t g_width = 0;
uint32_t g_height = 0;

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

/// Renders the host's stats blob into the one status line. The blob is the
/// page's, not this module's: budget_us and rt_percent are DERIVED THERE, by
/// the same arithmetic native gpu_status() uses, so the browser and the native
/// build print the same numbers computed the same way.
///
/// The copy carries no speed claim. A measured spike (2026-06-29) showed a
/// competent real-FFT CPU convolver matches or beats this GPU path at every
/// musically plausible setting; the GPU engine is a capability demonstration.
/// Missed GPU deadlines are reported as blocks the CPU net COVERED — never as
/// blocks the GPU produced.
std::string format_gpu_status(const char* json) {
    if (!json || !*json) return {};
    try {
        auto v = choc::json::parse(json);
        if (!v.isObject()) return {};
        const auto text = [&](const char* key) -> std::string {
            return v.hasObjectMember(key) ? v[key].toString() : std::string();
        };
        const auto number = [&](const char* key) -> double {
            return v.hasObjectMember(key) ? v[key].getWithDefault<double>(0.0) : 0.0;
        };

        const std::string engine = text("engine");
        const std::string note = text("note");
        std::string line;

        if (engine == "gpu") {
            const std::string backend = text("backend");
            // IDENTITY ONLY — no live numbers in the view tree.
            //
            // The metrics now live in DOM slots on the page, each with a fixed width and
            // tabular figures. They were here, inside the canvas, and every time a counter
            // gained a digit the whole line re-laid out: it wrapped to two lines, and at
            // phone width it sheared straight through the knob labels above it. A view-tree
            // label that changes width on a 10 Hz timer is a layout event on a 10 Hz timer.
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Engine: GPU — WGSL compute%s%s",
                          backend.empty() ? "" : " on ", backend.c_str());
            line = buf;
        } else {
            line = "Engine: CPU";
        }
        if (!note.empty()) line += " · " + note;
        return line;
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

extern "C" {

EMSCRIPTEN_KEEPALIVE
void pulp_ui_add_param(int index, const char* name, float min_value,
                       float max_value, float default_value, const char* unit,
                       int is_toggle) {
    ParamSpec spec;
    spec.index = index;
    spec.name = name ? name : "";
    spec.min_value = min_value;
    spec.max_value = max_value;
    spec.default_value = default_value;
    spec.unit = unit ? unit : "";
    // ON/OFF gets a switch, not a dial — see ParamSpec::is_toggle.
    spec.is_toggle = is_toggle != 0;
    pending_params().push_back(std::move(spec));
}

EMSCRIPTEN_KEEPALIVE
int pulp_ui_init(const char* canvas_selector, int width, int height, float dpr) {
    if (g_host) return 0;
    (void) dpr;  // the browser host reads devicePixelRatio itself.

    g_width = static_cast<uint32_t>(width > 0 ? width : 1);
    g_height = static_cast<uint32_t>(height > 0 ? height : 1);

    g_ui = std::make_unique<SuperConvolverWebUi>(pending_params());
    pending_params().clear();

    g_ui->on_param_change = [](int index, float value) {
        pulp_ui_js_param_change(index, value);
    };
    g_ui->on_gesture_begin = [](int index) { pulp_ui_js_gesture_begin(index); };
    g_ui->on_gesture_end = [](int index) { pulp_ui_js_gesture_end(index); };

    pulp::view::web::install_browser_window_host(
        canvas_selector ? canvas_selector : "#canvas");

    pulp::view::WindowOptions options;
    options.title = "SuperConvolver";
    options.width = static_cast<float>(g_width);
    options.height = static_cast<float>(g_height);
    options.use_gpu = true;

    g_host = pulp::view::WindowHost::create(*g_ui, options);
    if (!g_host) return 0;

    g_host->show();
    // The browser host owns the requestAnimationFrame loop; run_event_loop()
    // arms it and returns immediately (the browser owns the real event loop, so
    // blocking here would deadlock the page).
    g_host->run_event_loop();
    return 1;
}

EMSCRIPTEN_KEEPALIVE
void pulp_ui_resize(int width, int height, float dpr) {
    if (!g_host) return;
    (void) dpr;  // handle_resize() re-reads devicePixelRatio and resizes the
                 // canvas backing store itself.
    g_width = static_cast<uint32_t>(width > 0 ? width : 1);
    g_height = static_cast<uint32_t>(height > 0 ? height : 1);
    pulp::view::web::notify_browser_host_resized(static_cast<float>(g_width),
                                                 static_cast<float>(g_height));
}

EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_param(int index, float value) {
    if (!g_ui) return;
    g_ui->set_param(index, value);
    if (g_host) g_host->mark_dirty();
}

EMSCRIPTEN_KEEPALIVE
float pulp_ui_get_param(int index) {
    return g_ui ? g_ui->param_value(index) : 0.0f;
}

// Force a synchronous paint. The pixel fixture calls this immediately before
// reading the WebGL drawing buffer back, so the readback happens in the same
// task as the draw.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_repaint() {
    if (g_host) g_host->repaint();
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
// covered / avg_us / budget_us / rt_percent / note); an unparseable or empty
// blob restores the default CPU line rather than leaving a stale GPU readout on
// screen. The page pushes this on a setInterval, NOT a rAF: a backgrounded tab
// throttles rAF, and that is exactly when misses are most interesting.
EMSCRIPTEN_KEEPALIVE
void pulp_ui_set_gpu_status(const char* json) {
    if (!g_ui) return;
    g_ui->set_status(format_gpu_status(json));
    if (g_host) g_host->mark_dirty();
}

// Root-relative rect of a widget, written as [x, y, w, h] floats. `kind` 0 =
// the knob, 1 = the name label, 2 = the status line (index ignored). Returns 0
// when the index is unknown.
EMSCRIPTEN_KEEPALIVE
int pulp_ui_widget_rect(int index, int kind, float* out_rect) {
    if (!g_ui || !out_rect) return 0;
    pulp::view::Rect r;
    const bool ok = (kind == 0)   ? g_ui->knob_bounds(index, &r)
                    : (kind == 2) ? g_ui->status_bounds(&r)
                                  : g_ui->label_bounds(index, &r);
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
    g_host.reset();
    g_ui.reset();
    pulp::view::WindowHost::clear_factory();
}

}  // extern "C"
