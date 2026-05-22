#pragma once

#include <pulp/view/view.hpp>
#include <memory>
#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::view {

// Platform-specific handle types
#ifdef __APPLE__
using NativeViewHandle = void*; // NSView* (macOS) or UIView* (iOS)
#elif defined(_WIN32)
using NativeViewHandle = void*; // HWND
#else
using NativeViewHandle = void*; // X11 Window or nullptr
#endif

// Hosts a View tree inside a DAW plugin window
// Creates a platform-native child view that paints the widget hierarchy
class PluginViewHost {
public:
    struct Size {
        uint32_t width = 400;
        uint32_t height = 300;
    };

    struct Options {
        Size size = {400, 300};
        bool use_gpu = false;  ///< Use GPU rendering (Dawn/Skia Graphite) instead of CoreGraphics
    };

    // Create a plugin view host for the given view tree.
    //
    // Platform support (#299):
    //   - macOS: NSView-backed impl in plugin_view_host_mac.mm.
    //   - iOS: UIView-backed impl in plugin_view_host_ios.mm.
    //   - Windows/Linux/Android: no built-in impl — host app
    //     registers a factory via set_factory(). Without a
    //     factory, create() returns nullptr explicitly.
    static std::unique_ptr<PluginViewHost> create(View& root, Size size);
    static std::unique_ptr<PluginViewHost> create(View& root, const Options& options);

    // Host-registered factory (#299). Installed by the host app's
    // platform layer on non-Apple targets.
    using Factory = std::function<std::unique_ptr<PluginViewHost>(
        View& root, const Options& options)>;
    static void set_factory(Factory factory);
    static void clear_factory();
    static bool has_factory();

    virtual ~PluginViewHost() = default;

    // Get the native view handle to pass to the DAW host
    virtual NativeViewHandle native_handle() = 0;

    // Attach this view to a parent native view (the DAW's editor window)
    virtual void attach_to_parent(NativeViewHandle parent) = 0;

    // Detach from parent
    virtual void detach() = 0;

    // Request a repaint (call when parameters change)
    virtual void repaint() = 0;

    // Resize
    virtual void set_size(uint32_t width, uint32_t height) = 0;
    virtual Size get_size() const = 0;

    // True only when this host is rendering through the GPU (Dawn/Skia
    // Graphite) pipeline AND GPU initialization succeeded. CoreGraphics
    // CPU hosts return false, and a GPU host that failed to initialize
    // Dawn/Skia and fell back to CPU also returns false. Adapters read
    // this after create() to scream when a GPU/scripted editor view
    // silently took the CPU path (see `format/gpu_host_select.hpp`).
    virtual bool is_gpu_backed() const { return false; }

    // Per-vsync idle pump. GPU hosts invoke this callback once per display
    // link tick (before rendering) so a scripted UI's `ScriptedUiSession::poll()`
    // — async results, timers, requestAnimationFrame — keeps running while
    // the editor is embedded. CPU hosts ignore it. Default no-op.
    virtual void set_idle_callback(std::function<void()> callback) {
        (void) callback;
    }

    // Capture the host's current back buffer as a PNG (mirrors
    // `WindowHost::capture_back_buffer_png`, issue #2001). GPU hosts read
    // back the rendered Skia frame via `SkiaSurface::read_current_rgba`;
    // hosts that cannot capture return an empty vector. Used by the
    // embedded-editor host smoke tests.
    virtual std::vector<uint8_t> capture_back_buffer_png() { return {}; }

    // Attach/detach a native child view inside the plugin editor host.
    // Coordinates use Pulp's top-left origin convention, matching WindowHost.
    virtual bool attach_native_child_view(NativeViewHandle child_view,
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
    virtual bool set_native_child_view_bounds(NativeViewHandle child_view,
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
    virtual void detach_native_child_view(NativeViewHandle child_view) {
        (void) child_view;
    }
};

} // namespace pulp::view
