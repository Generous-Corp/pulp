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

    // ── Multi-window hints (Phase 6) ────────────────────────────────────
    // These are used by WindowManager to configure platform-specific
    // window behavior. Callers creating standalone windows can ignore them.

    WindowType* window_type = nullptr;  ///< Optional type hint for platform styling
    void* parent_native_handle = nullptr; ///< Parent window handle (HWND, NSWindow*)
    void* shared_gpu_device = nullptr;  ///< Shared Dawn device for multi-window GPU
};

// Native window that hosts a View tree and renders it
// Platform-specific implementation (NSWindow on macOS)
class WindowHost {
public:
    // Create a window hosting the given view tree
    static std::unique_ptr<WindowHost> create(View& root, const WindowOptions& options);

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
