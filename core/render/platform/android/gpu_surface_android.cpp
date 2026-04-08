#if defined(__ANDROID__)

#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/platform/android/jni.hpp>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/choreographer.h>
#include <android/looper.h>
#include <android/log.h>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>

#define PULP_LOG_TAG "Pulp"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, PULP_LOG_TAG, __VA_ARGS__)
#define PULP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::render {

// ── Android GPU Surface Manager ───────────────────────────────────────────
// Manages the lifecycle of Dawn/Vulkan rendering on Android.
// Bridges ANativeWindow from Kotlin SurfaceView to GpuSurface.

static void android_render_frame(float dt = 1.0f);
static std::unique_ptr<GpuSurface> g_gpu_surface;
static std::unique_ptr<SkiaSurface> g_skia_surface;
static std::unique_ptr<view::View> g_root_view;

// Display density from Android (set by Kotlin before surface creation)
static float g_display_density = 2.625f;  // default xxhdpi, overridden by Kotlin

// Safe area insets (dp) — status bar, nav bar, notch. Set by Kotlin via WindowInsetsCompat.
static float g_safe_top = 0, g_safe_bottom = 0, g_safe_left = 0, g_safe_right = 0;

// Touch state
static view::View* g_captured_view = nullptr;
static std::chrono::steady_clock::time_point g_last_tap_time{};
static float g_last_tap_x = 0, g_last_tap_y = 0;

// Choreographer continuous render loop
static AChoreographer* g_choreographer = nullptr;
static std::atomic<bool> g_render_loop_running{false};
static int64_t g_last_frame_nanos = 0;

static void on_vsync(long frame_time_nanos, void* data);

static void start_render_loop() {
    g_choreographer = AChoreographer_getInstance();
    if (!g_choreographer) {
        PULP_LOGW("AChoreographer not available — touch-only repaints");
        return;
    }
    g_render_loop_running.store(true);
    g_last_frame_nanos = 0;
    AChoreographer_postFrameCallback(g_choreographer, on_vsync, nullptr);
    PULP_LOGI("AChoreographer render loop started");
}

static void stop_render_loop() {
    g_render_loop_running.store(false);
    g_choreographer = nullptr;
    PULP_LOGI("AChoreographer render loop stopped");
}

void android_set_display_density(float density) {
    g_display_density = density;
    PULP_LOGI("Display density set to %.2f", density);
}

void android_set_safe_area_insets(float top, float bottom, float left, float right) {
    g_safe_top = top;
    g_safe_bottom = bottom;
    g_safe_left = left;
    g_safe_right = right;
    PULP_LOGI("Safe area insets (dp): top=%.1f bottom=%.1f left=%.1f right=%.1f",
              top, bottom, left, right);
    // Force hierarchy recreation on next frame to pick up new insets
    g_root_view.reset();
    g_captured_view = nullptr;
}

// Helper: create a row of knobs in a panel container
static std::unique_ptr<view::Panel> make_knob_row(
    const std::vector<float>& values, float knob_size, float row_height) {
    using namespace view;
    auto row = std::make_unique<Panel>();
    row->flex().direction = FlexDirection::row;
    row->flex().preferred_height = row_height;
    row->flex().margin_left = 8;
    row->flex().margin_right = 8;
    row->flex().justify_content = FlexJustify::space_evenly;
    row->flex().align_items = FlexAlign::center;
    for (float v : values) {
        auto knob = std::make_unique<Knob>();
        knob->set_value(v);
        knob->flex().preferred_width = knob_size;
        knob->flex().preferred_height = knob_size;
        row->add_child(std::move(knob));
    }
    return row;
}

// Helper: horizontal fader with margins
static std::unique_ptr<view::Fader> make_fader(float value, float height) {
    using namespace view;
    auto fader = std::make_unique<Fader>();
    fader->set_value(value);
    fader->set_orientation(Fader::Orientation::horizontal);
    fader->flex().preferred_height = height;
    fader->flex().margin_left = 8;
    fader->flex().margin_right = 8;
    return fader;
}

// Recursively advance animations on all widgets in the hierarchy.
// Without a continuous render loop (AChoreographer), this is called before each
// paint with a large dt to snap animations to completion on touch-triggered repaints.
static void advance_view_animations(view::View* v, float dt) {
    if (!v) return;
    // Try each widget type that has advance_animations
    if (auto* k = dynamic_cast<view::Knob*>(v))   { k->advance_animations(dt); }
    if (auto* f = dynamic_cast<view::Fader*>(v))   { f->advance_animations(dt); }
    if (auto* t = dynamic_cast<view::Toggle*>(v))  { t->advance_animations(dt); }
    for (size_t i = 0; i < v->child_count(); ++i)
        advance_view_animations(v->child_at(i), dt);
}

// Section indices in the root view child list (for label positioning)
struct SectionIndices {
    int osc_knobs = -1;
    int toggles = -1;
    int xy_pad = -1;
    int filter_knobs = -1;
    int env_knobs = -1;
    int mixer_start = -1;  // first fader
    int master_fader = -1;
    int meter = -1;
};
static SectionIndices g_sections;

static void create_demo_view_hierarchy(float width, float height) {
    using namespace view;

    float dp_w = width / g_display_density;
    float dp_h = height / g_display_density;

    g_root_view = std::make_unique<Panel>();
    g_root_view->set_bounds({0, 0, dp_w, dp_h});
    g_root_view->set_theme(Theme::dark());
    // Apply safe area insets so content clears the status bar, nav bar, and notch
    float content_pad = 12.0f;
    g_root_view->flex().padding_top = std::max(content_pad, g_safe_top + 4.0f);
    g_root_view->flex().padding_bottom = std::max(content_pad, g_safe_bottom + 4.0f);
    g_root_view->flex().padding_left = std::max(content_pad, g_safe_left);
    g_root_view->flex().padding_right = std::max(content_pad, g_safe_right);

    // ── Title spacer (text drawn directly in render_frame) ──────────
    auto title_spacer = std::make_unique<Panel>();
    title_spacer->flex().preferred_height = 32;
    title_spacer->flex().margin_top = 4;
    // spacer — Panel bg blends with root, label text drawn in draw_section_labels()
    g_root_view->add_child(std::move(title_spacer));

    // ── Section label spacer for OSC ─────────────────────────────────
    auto osc_label = std::make_unique<Panel>();
    osc_label->flex().preferred_height = 16;
    osc_label->flex().margin_top = 2;
    // spacer for label text
    g_root_view->add_child(std::move(osc_label));

    // ── Oscillator: 4 knobs directly in a row ────────────────────────
    auto osc_knobs = make_knob_row({0.5f, 0.3f, 0.5f, 0.15f}, 48, 56);
    osc_knobs->flex().margin_top = 0;
    g_sections.osc_knobs = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(osc_knobs));

    // ── Toggle row: 4 toggles directly in root ──────────────────────
    auto toggle_row = std::make_unique<Panel>();
    toggle_row->flex().direction = FlexDirection::row;
    toggle_row->flex().preferred_height = 36;
    toggle_row->flex().margin_top = 8;
    toggle_row->flex().margin_left = 8;
    toggle_row->flex().margin_right = 8;
    toggle_row->flex().justify_content = FlexJustify::space_evenly;
    toggle_row->flex().align_items = FlexAlign::center;
    bool toggle_states[] = {true, false, true, false};
    for (int i = 0; i < 4; ++i) {
        auto toggle = std::make_unique<Toggle>();
        toggle->set_on(toggle_states[i]);
        toggle->flex().preferred_width = 48;
        toggle->flex().preferred_height = 28;
        toggle_row->add_child(std::move(toggle));
    }
    g_sections.toggles = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(toggle_row));

    // ── Section label spacer for XY ──────────────────────────────────
    auto xy_label = std::make_unique<Panel>();
    xy_label->flex().preferred_height = 16;
    xy_label->flex().margin_top = 6;
    // spacer for label text
    g_root_view->add_child(std::move(xy_label));

    // ── XY Pad ──────────────────────────────────────────────────────
    auto xy = std::make_unique<XYPad>();
    xy->set_x(0.65f);
    xy->set_y(0.35f);
    xy->flex().preferred_height = 120;
    xy->flex().margin_top = 0;
    xy->flex().margin_left = 8;
    xy->flex().margin_right = 8;
    g_sections.xy_pad = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(xy));

    // ── Section label spacer for FILTER ──────────────────────────────
    auto filter_label = std::make_unique<Panel>();
    filter_label->flex().preferred_height = 16;
    filter_label->flex().margin_top = 6;
    // spacer for label text
    g_root_view->add_child(std::move(filter_label));

    // ── Filter: 3 knobs directly in a row ───────────────────────────
    auto filter_knobs = make_knob_row({0.65f, 0.35f, 0.5f}, 44, 52);
    filter_knobs->flex().margin_top = 0;
    g_sections.filter_knobs = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(filter_knobs));

    // ── Section label spacer for ENVELOPE ────────────────────────────
    auto env_label = std::make_unique<Panel>();
    env_label->flex().preferred_height = 16;
    env_label->flex().margin_top = 4;
    // spacer for label text
    g_root_view->add_child(std::move(env_label));

    // ── Envelope: 4 ADSR knobs directly in a row ────────────────────
    auto env_knobs = make_knob_row({0.05f, 0.3f, 0.7f, 0.4f}, 40, 48);
    env_knobs->flex().margin_top = 0;
    g_sections.env_knobs = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(env_knobs));

    // ── Section label spacer for MIXER ───────────────────────────────
    auto mixer_label = std::make_unique<Panel>();
    mixer_label->flex().preferred_height = 16;
    mixer_label->flex().margin_top = 6;
    // spacer for label text
    g_root_view->add_child(std::move(mixer_label));

    // ── Mixer: 4 horizontal faders stacked vertically ───────────────
    float fader_values[] = {0.75f, 0.6f, 0.4f, 0.2f};
    g_sections.mixer_start = static_cast<int>(g_root_view->child_count());
    for (int i = 0; i < 4; ++i) {
        auto f = make_fader(fader_values[i], 22);
        f->flex().margin_top = (i == 0) ? 0 : 4;
        g_root_view->add_child(std::move(f));
    }

    // ── Section label spacer for MASTER ──────────────────────────────
    auto master_label = std::make_unique<Panel>();
    master_label->flex().preferred_height = 16;
    master_label->flex().margin_top = 6;
    // spacer for label text
    g_root_view->add_child(std::move(master_label));

    // ── Master fader ────────────────────────────────────────────────
    auto master_fader = make_fader(0.8f, 26);
    master_fader->flex().margin_top = 0;
    g_sections.master_fader = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(master_fader));

    // ── Output meter (read-only audio level display) ────────────────
    auto meter = std::make_unique<Meter>();
    meter->set_level(-8.0f, -3.0f);
    meter->flex().preferred_height = 20;
    meter->flex().margin_top = 6;
    meter->flex().margin_left = 8;
    meter->flex().margin_right = 8;
    g_sections.meter = static_cast<int>(g_root_view->child_count());
    g_root_view->add_child(std::move(meter));

    g_root_view->layout_children();
    PULP_LOGI("Android GPU surface: Synth UI created (%d children, %.0fx%.0f dp)",
              static_cast<int>(g_root_view->child_count()), dp_w, dp_h);
}

// Draw section labels directly on the canvas (bypasses Label widget pipeline
// which has a known text rendering issue under nested Skia Graphite clips)
static void draw_section_labels(canvas::Canvas& canvas) {
    if (!g_root_view || g_root_view->child_count() == 0) return;

    float pad_left = std::max(12.0f, g_safe_left);
    float pad_top = std::max(12.0f, g_safe_top + 4.0f);
    float label_x = pad_left + 8.0f;

    // Title: "PULP SYNTH"
    canvas.set_fill_color(canvas::Color::rgba(205, 214, 244));  // text.primary from dark theme
    canvas.set_font("sans-serif", 22);
    canvas.fill_text("PULP SYNTH", label_x, pad_top + 22);

    // Section labels: smaller, secondary color
    auto label_color = canvas::Color::rgba(166, 173, 200);  // text.secondary
    canvas.set_fill_color(label_color);
    canvas.set_font("sans-serif", 11);
    canvas.set_text_align(canvas::TextAlign::left);

    // Helper: get child y position for label placement
    auto child_y = [&](int idx) -> float {
        if (idx < 0 || idx >= static_cast<int>(g_root_view->child_count())) return 0;
        return g_root_view->child_at(idx)->bounds().y;
    };

    // OSC label: above the osc knob row (child before it is the spacer)
    if (g_sections.osc_knobs >= 1) {
        float y = child_y(g_sections.osc_knobs - 1) + 12;
        canvas.fill_text("OSCILLATOR", label_x, y);
    }

    // XY PAD label
    if (g_sections.xy_pad >= 1) {
        float y = child_y(g_sections.xy_pad - 1) + 12;
        canvas.fill_text("XY PAD", label_x, y);
    }

    // FILTER label
    if (g_sections.filter_knobs >= 1) {
        float y = child_y(g_sections.filter_knobs - 1) + 12;
        canvas.fill_text("FILTER", label_x, y);
    }

    // ENVELOPE label
    if (g_sections.env_knobs >= 1) {
        float y = child_y(g_sections.env_knobs - 1) + 12;
        canvas.fill_text("ENVELOPE", label_x, y);
    }

    // MIXER label
    if (g_sections.mixer_start >= 1) {
        float y = child_y(g_sections.mixer_start - 1) + 12;
        canvas.fill_text("MIXER", label_x, y);
    }

    // MASTER label
    if (g_sections.master_fader >= 1) {
        float y = child_y(g_sections.master_fader - 1) + 12;
        canvas.fill_text("MASTER", label_x, y);
    }
}

static ANativeWindow* g_native_window = nullptr;
static std::mutex g_surface_mutex;
static std::condition_variable g_surface_cv;
static bool g_render_stopped = true;
static bool g_surface_valid = false;

void android_surface_created(ANativeWindow* window) {
    PULP_LOGI("Android GPU surface: ANativeWindow received (%dx%d)",
              ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));

    g_native_window = window;
    ANativeWindow_acquire(g_native_window);

    // Create and initialize Dawn GPU surface
    g_gpu_surface = GpuSurface::create_dawn();
    if (!g_gpu_surface) {
        PULP_LOGE("Android GPU surface: failed to create Dawn GpuSurface");
        return;
    }

    GpuSurface::Config config;
    config.width = static_cast<uint32_t>(ANativeWindow_getWidth(window));
    config.height = static_cast<uint32_t>(ANativeWindow_getHeight(window));
    config.native_surface_handle = g_native_window;
    config.vsync = true;

    if (g_gpu_surface->initialize(config)) {
        PULP_LOGI("Android GPU surface: Dawn initialized (%ux%u)",
                  config.width, config.height);

        auto info = g_gpu_surface->adapter_info();
        PULP_LOGI("Android GPU surface: adapter=%s backend=%s",
                  info.name.c_str(), info.backend.c_str());

        // Create Skia Graphite surface wrapping the Dawn device
        SkiaSurface::Config skia_config;
        skia_config.width = config.width;
        skia_config.height = config.height;
        skia_config.scale_factor = g_display_density;  // SkiaSurface applies this internally

        g_skia_surface = SkiaSurface::create(*g_gpu_surface, skia_config);
        if (g_skia_surface && g_skia_surface->is_available()) {
            PULP_LOGI("Android GPU surface: Skia Graphite context created");
        } else {
            PULP_LOGW("Android GPU surface: Skia Graphite failed — Dawn-only mode");
        }

        {
            std::lock_guard lock(g_surface_mutex);
            g_surface_valid = true;
            g_render_stopped = false;
        }

        // Render an initial frame and start the continuous render loop
        android_render_frame();
        start_render_loop();
    } else {
        PULP_LOGW("Android GPU surface: Dawn initialization failed — no GPU rendering");
        g_gpu_surface.reset();
    }
}

void android_surface_resized(int width, int height) {
    if (g_gpu_surface && g_gpu_surface->is_initialized()) {
        g_gpu_surface->resize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
        PULP_LOGI("Android GPU surface: resized to %dx%d", width, height);

        // Recreate view hierarchy at new size
        g_root_view.reset();
        g_captured_view = nullptr;

        android_render_frame();
    }
}

void android_render_frame(float dt) {
    if (!g_gpu_surface || !g_gpu_surface->is_initialized()) return;

    if (g_gpu_surface->begin_frame()) {
        if (g_skia_surface && g_skia_surface->is_available()) {
            auto* canvas = g_skia_surface->begin_frame();
            if (canvas) {
                float w = static_cast<float>(g_gpu_surface->width());
                float h = static_cast<float>(g_gpu_surface->height());

                // Create view hierarchy on first render
                if (!g_root_view) {
                    create_demo_view_hierarchy(w, h);
                }

                // Advance animations with real dt from choreographer,
                // or 1.0f to snap to completion on touch-only repaints
                advance_view_animations(g_root_view.get(), dt);

                // SkiaSurface applies display density scaling internally
                g_root_view->paint_all(*canvas);

                // Draw section labels directly on canvas (workaround for
                // Label widget text not rendering under nested Graphite clips)
                draw_section_labels(*canvas);

                g_skia_surface->end_frame();
            }
        }
        g_gpu_surface->end_frame();
    }
}

// AChoreographer VSYNC callback — drives the continuous render loop
static void on_vsync(long frame_time_nanos, void* /*data*/) {
    if (!g_render_loop_running.load()) return;

    // Calculate dt from previous frame
    float dt = 0.016f;  // default ~60fps
    if (g_last_frame_nanos > 0) {
        dt = static_cast<float>(frame_time_nanos - g_last_frame_nanos) / 1e9f;
        dt = std::clamp(dt, 0.001f, 0.1f);  // clamp to 10fps..1000fps
    }
    g_last_frame_nanos = frame_time_nanos;

    android_render_frame(dt);

    // Request next frame
    if (g_render_loop_running.load() && g_choreographer) {
        AChoreographer_postFrameCallback(g_choreographer, on_vsync, nullptr);
    }
}

// ── Touch → View Routing ─────────────────────────────────────────────────

// Convert root dp coordinates to view-local coordinates by walking up the parent chain
static view::Point to_local(view::View* target, float dp_x, float dp_y) {
    // Accumulate absolute offset by walking up parents
    float abs_x = 0, abs_y = 0;
    for (auto* v = target; v != nullptr; v = v->parent()) {
        abs_x += v->bounds().x;
        abs_y += v->bounds().y;
    }
    return {dp_x - abs_x, dp_y - abs_y};
}

static view::MouseEvent make_touch_event(view::Point local, view::Point dp,
                                          int pointer_id, float pressure, bool is_down) {
    view::MouseEvent ev;
    ev.position = local;
    ev.window_position = dp;
    ev.button = view::MouseButton::left;
    ev.pointer_id = pointer_id;
    ev.pointer_type = view::PointerType::touch;
    ev.pressure = pressure;
    ev.is_down = is_down;
    ev.click_count = 1;
    return ev;
}

void android_touch_down(int pointer_id, float px_x, float px_y, float pressure) {
    if (!g_root_view) return;

    float dp_x = px_x / g_display_density;
    float dp_y = px_y / g_display_density;

    // Detect double-tap (within 300ms and 30dp)
    auto now = std::chrono::steady_clock::now();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_tap_time).count();
    float dist = std::abs(dp_x - g_last_tap_x) + std::abs(dp_y - g_last_tap_y);
    int click_count = (dt < 300 && dist < 30.0f) ? 2 : 1;
    g_last_tap_time = now;
    g_last_tap_x = dp_x;
    g_last_tap_y = dp_y;

    view::Point pt{dp_x, dp_y};
    auto* target = g_root_view->hit_test(pt);
    if (target) {
        auto local = to_local(target, dp_x, dp_y);
        // Dispatch rich event (Fader dragging_, Knob double-click reset)
        auto ev = make_touch_event(local, pt, pointer_id, pressure, true);
        ev.click_count = click_count;
        target->on_mouse_event(ev);
        // Dispatch legacy event (Knob drag_start_y_, Toggle on/off)
        target->on_mouse_down(local);
        g_captured_view = target;
    }

    android_render_frame();
}

void android_touch_move(int pointer_id, float px_x, float px_y, float pressure) {
    if (!g_captured_view) return;

    float dp_x = px_x / g_display_density;
    float dp_y = px_y / g_display_density;

    auto local = to_local(g_captured_view, dp_x, dp_y);
    g_captured_view->on_mouse_drag(local);

    android_render_frame();
}

void android_touch_up(int pointer_id, float px_x, float px_y) {
    if (!g_captured_view) return;

    float dp_x = px_x / g_display_density;
    float dp_y = px_y / g_display_density;

    auto local = to_local(g_captured_view, dp_x, dp_y);
    // Dispatch rich event (Fader clears dragging_ on is_down=false)
    auto ev = make_touch_event(local, {dp_x, dp_y}, pointer_id, 0.0f, false);
    g_captured_view->on_mouse_event(ev);
    // Dispatch legacy event
    g_captured_view->on_mouse_up(local);
    g_captured_view = nullptr;

    android_render_frame();
}

void android_surface_destroyed() {
    PULP_LOGI("Android GPU surface: destroying — stopping render loop...");
    stop_render_loop();

    {
        std::lock_guard lock(g_surface_mutex);
        g_surface_valid = false;
    }

    // Wait for render to confirm stop (condition_variable, not spin)
    {
        std::unique_lock lock(g_surface_mutex);
        g_surface_cv.wait(lock, [] { return g_render_stopped; });
    }

    g_captured_view = nullptr;
    g_root_view.reset();
    g_gpu_surface.reset();

    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = nullptr;
    }

    PULP_LOGI("Android GPU surface: destroyed");
}

// Called by the render loop each frame
bool android_gpu_begin_frame() {
    std::lock_guard lock(g_surface_mutex);
    if (!g_surface_valid || !g_gpu_surface) return false;
    return g_gpu_surface->begin_frame();
}

void android_gpu_end_frame() {
    if (g_gpu_surface) g_gpu_surface->end_frame();
}

void android_gpu_signal_render_stopped() {
    {
        std::lock_guard lock(g_surface_mutex);
        g_render_stopped = true;
    }
    g_surface_cv.notify_one();
}

GpuSurface* android_gpu_surface() {
    return g_gpu_surface.get();
}

} // namespace pulp::render

// ── JNI Exports ───────────────────────────────────────────────────────────

extern "C" {

// Display density — call from Kotlin before surface is created
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeSetDisplayDensity(
    JNIEnv*, jobject, jfloat density) {
    pulp::render::android_set_display_density(density);
}

// Safe area insets (dp) — status bar, nav bar, notch
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeSetSafeAreaInsets(
    JNIEnv*, jobject, jfloat top, jfloat bottom, jfloat left, jfloat right) {
    pulp::render::android_set_safe_area_insets(top, bottom, left, right);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnSurfaceCreated(
    JNIEnv* env, jobject thiz, jobject surface) {
    try {
        ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
        if (window) {
            pulp::render::android_surface_created(window);
            ANativeWindow_release(window);  // android_surface_created acquires its own ref
        } else {
            PULP_LOGE("ANativeWindow_fromSurface returned null");
        }
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSurfaceCreated");
    }
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnSurfaceResized(
    JNIEnv* env, jobject thiz, jint width, jint height) {
    try {
        pulp::render::android_surface_resized(width, height);
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSurfaceResized");
    }
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnSurfaceDestroyed(
    JNIEnv* env, jobject thiz) {
    try {
        pulp::render::android_surface_destroyed();
    } catch (const std::exception& e) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"),
                      "Unknown C++ exception in nativeOnSurfaceDestroyed");
    }
}

// Touch events → View hierarchy
JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchDown(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y, jfloat pressure) {
    pulp::render::android_touch_down(pointerId, x, y, pressure);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchMove(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y, jfloat pressure) {
    pulp::render::android_touch_move(pointerId, x, y, pressure);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchUp(
    JNIEnv*, jobject, jint pointerId, jfloat x, jfloat y) {
    pulp::render::android_touch_up(pointerId, x, y);
}

JNIEXPORT void JNICALL
Java_com_pulp_render_PulpSurfaceView_nativeOnTouchCancel(JNIEnv*, jobject) {
    pulp::render::g_captured_view = nullptr;
}

} // extern "C"

#endif // __ANDROID__
