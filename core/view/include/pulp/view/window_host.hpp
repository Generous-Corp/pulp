#pragma once

#include <pulp/view/view.hpp>
#include <string>
#include <memory>
#include <functional>
#include <vector>

namespace pulp::render {
class GpuSurface;
}

namespace pulp::view {

enum class WindowType;  // Forward-declared from window_manager.hpp

struct WindowOptions {
    std::string title = "Pulp";
    float width = 400;
    float height = 300;
    float min_width = 0;   ///< Minimum window width (0 = no minimum)
    float min_height = 0;  ///< Minimum window height (0 = no minimum)
    bool resizable = true;
    bool use_gpu = false;  ///< Use GPU rendering (Dawn/Skia Graphite) instead of CoreGraphics

    /// pulp-internal #71 follow-up — when true, the window is created and
    /// the run loop drives the bridge per-vsync as usual, but the window
    /// is never made visible / brought to front / activated. The app also
    /// skips the Dock-icon and focus-stealing activation steps. Intended
    /// for headless smoke tests (live-host-pump-smoke.sh) and any
    /// validation flow that wants the full live-host code path without a
    /// GUI window flashing on screen. Backends that don't honour the
    /// flag fall back to normal behaviour.
    bool initially_hidden = false;

    // ── Multi-window hints (Phase 6) ────────────────────────────────────
    // These are used by WindowManager to configure platform-specific
    // window behavior. Callers creating standalone windows can ignore them.

    WindowType* window_type = nullptr;  ///< Optional type hint for platform styling
    void* parent_native_handle = nullptr; ///< Parent window handle (HWND, NSWindow*)
    void* shared_gpu_device = nullptr;  ///< Shared Dawn device for multi-window GPU
};

// Native window that hosts a View tree and renders it.
//
// Platform support (#299):
//   - macOS: native NSWindow-backed impl in window_host_mac.mm.
//   - iOS: native UIWindow-backed impl in window_host_ios.mm.
//   - Windows/Linux/Android: no built-in impl — the host app or a
//     future platform module registers a factory via
//     set_factory(). Without a factory, create() returns nullptr
//     so callers can surface "platform unsupported" through
//     has_factory() rather than a silent null.
class WindowHost {
public:
    struct ContentSize {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;

    // Create a window hosting the given view tree
    static std::unique_ptr<WindowHost> create(View& root, const WindowOptions& options);

    // Host-registered factory (#299). Installed by the platform
    // layer of a host app on non-Apple targets. Apple targets use
    // the built-in native impl and ignore the factory.
    using Factory = std::function<std::unique_ptr<WindowHost>(
        View& root, const WindowOptions& options)>;
    static void set_factory(Factory factory);
    static void clear_factory();
    static bool has_factory();

    virtual ~WindowHost() = default;

    // Show/hide the window
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual bool is_visible() const = 0;

    // Request a repaint
    virtual void repaint() = 0;

    // Native host/window handles for embedding child platform views such as
    // WebViews. Default implementations return nullptr on platforms that do
    // not expose these seams yet.
    /// Position this window alongside another window.
    /// Places to the right if there's screen space, otherwise to the left.
    virtual void position_beside(WindowHost* other) { (void)other; }

    virtual void* native_window_handle() const { return nullptr; }
    virtual void* native_content_view_handle() const { return nullptr; }
    virtual void* dawn_device_handle() const { return nullptr; }
    virtual void* dawn_queue_handle() const { return nullptr; }
    virtual void* dawn_instance_handle() const { return nullptr; }
    virtual render::GpuSurface* gpu_surface() const { return nullptr; }
    virtual ContentSize get_content_size() const;

    // Attach/detach a platform-native child view inside this host. Coordinates
    // use Pulp's top-left origin convention.
    virtual bool attach_native_child_view(void* child_view,
                                          float x,
                                          float y,
                                          float width,
                                          float height) {
        (void) child_view;
        (void) x;
        (void) y;
        (void) width;
        (void) height;
        return false;
    }
    virtual bool set_native_child_view_bounds(void* child_view,
                                              float x,
                                              float y,
                                              float width,
                                              float height) {
        (void) child_view;
        (void) x;
        (void) y;
        (void) width;
        (void) height;
        return false;
    }
    virtual void detach_native_child_view(void* child_view) { (void) child_view; }

    // Capture the current visible content as a PNG image.
    virtual std::vector<uint8_t> capture_png() { return {}; }

    // Clear any cached host-side input targets before the view tree is rebuilt.
    virtual void invalidate_input_state() {}

    // Request the host to close and exit its event loop.
    virtual void request_close() {}

    // Called periodically while the host event loop is running.
    // Used for editor-side polling such as hot reload.
    virtual void set_idle_callback(std::function<void()> cb) { (void)cb; }
    virtual void set_resize_callback(ResizeCallback cb);

    // Set a callback for when the window is closed
    virtual void set_close_callback(std::function<void()> cb) = 0;

    // Run the event loop (blocks until the window is closed)
    // Call this for standalone UI preview mode
    virtual void run_event_loop() = 0;

    // ── D.1 Client-side window decoration ───────────────────────────────
    /// Remove native title bar and let the app draw its own.
    virtual void set_client_decoration(bool enabled) { (void)enabled; }

    /// Hit-test region types for client-decorated windows.
    enum class HitTestRegion {
        none, client, caption,
        resize_n, resize_s, resize_e, resize_w,
        resize_ne, resize_nw, resize_se, resize_sw,
        close, minimize, maximize
    };

    // ── D.2 Window features ─────────────────────────────────────────────
    /// Constrain window resize to maintain aspect ratio.
    virtual void set_fixed_aspect_ratio(float ratio) { (void)ratio; }

    /// Keep window above all others.
    virtual void set_always_on_top(bool on_top) { (void)on_top; }

    /// Set a fixed "design viewport". When set, the root view's bounds are
    /// pinned to (design_w x design_h) and paint applies an aspect-correct
    /// scale + letterbox translate to fit the current window size. Input
    /// coordinates receive the inverse transform before hit-testing.
    /// Pass (0, 0) to disable.
    ///
    /// Use this for content authored at a known fixed size (e.g. an editor
    /// imported from Figma / Stitch / v0 / Pencil) that must resize
    /// proportionally without re-layout. Combine with
    /// set_fixed_aspect_ratio(design_w / design_h) so the OS enforces the
    /// aspect on user drag and letterbox bars only appear as a backstop
    /// during in-flight drag.
    ///
    /// Why this exists (pulp #59 / #63 / #64 / #65 — 2026-05-14):
    /// the "obvious" alternatives all failed for native-react renderers
    /// like Spectr:
    ///   * Per-child set_scale() on root children scales chrome but
    ///     canvas widgets record their draw commands at the original
    ///     size, so contents clip on shrink.
    ///   * Yoga absolute+inset:0 propagation: chains of position:absolute
    ///     + inset:0 collapse to 0×0 in Pulp's runtime-import because
    ///     Yoga only fills a containing block when the parent has a
    ///     definite POINTS size — the cascade root→body→wrap→canvas
    ///     never satisfies that.
    ///   * JS-driven canvas refit via React refs: even with refs wired
    ///     correctly through getPublicInstance, Spectr's resize() bails
    ///     on wrapRef.current/canvasRef.current existence checks and
    ///     never runs.
    /// The design-viewport approach sidesteps all of these by doing the
    /// resize at the renderer (paint-time scale of the design surface),
    /// which is what a browser webview effectively does at the layer
    /// level. The root view always thinks it's at design size; canvases
    /// record commands at design size; chrome lays out at design size.
    /// The WindowHost does the fitting on paint, and inverse-maps mouse
    /// coords so hit-test still works.
    virtual void set_design_viewport(float design_w, float design_h) {
        (void)design_w; (void)design_h;
    }

    // ── D.3 DPI / Monitor utilities ─────────────────────────────────────
    /// Get the DPI scale factor for this window.
    virtual float dpi_scale() const { return 1.0f; }

    /// Get the maximum available dimensions for this window's screen.
    virtual Size max_dimensions() const { return {1920, 1080}; }

    /// Convert physical pixels to logical coordinates.
    float convert_to_logical(float px) const { return px / dpi_scale(); }

    /// Convert logical coordinates to physical pixels.
    float convert_to_native(float logical) const { return logical * dpi_scale(); }

    /// Monitor information.
    struct MonitorInfo {
        Rect bounds;
        float dpi_scale = 1.0f;
        std::string name;
    };

    /// Get list of connected monitors. Default returns one virtual monitor.
    virtual std::vector<MonitorInfo> get_monitors() const {
        return {{{{0, 0, 1920, 1080}, 1.0f, "Default"}}};
    }

    // ── D.4 Mouse relative mode ─────────────────────────────────────────
    /// Hide cursor and report relative deltas (infinite drag for knobs).
    virtual void set_mouse_relative_mode(bool enabled) { (void)enabled; }
};

} // namespace pulp::view
