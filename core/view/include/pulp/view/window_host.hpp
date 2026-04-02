#pragma once

#include <pulp/view/view.hpp>
#include <string>
#include <memory>
#include <functional>
#include <vector>

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
    virtual void* native_window_handle() const { return nullptr; }
    virtual void* native_content_view_handle() const { return nullptr; }

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

    // Set a callback for when the window is closed
    virtual void set_close_callback(std::function<void()> cb) = 0;

    // Run the event loop (blocks until the window is closed)
    // Call this for standalone UI preview mode
    virtual void run_event_loop() = 0;
};

} // namespace pulp::view
