#pragma once

#include <pulp/view/view.hpp>
#include <memory>
#include <cstdint>
#include <functional>

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
};

} // namespace pulp::view
