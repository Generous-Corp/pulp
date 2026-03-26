#pragma once

#include <pulp/view/view.hpp>
#include <memory>
#include <cstdint>

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

    // Create a plugin view host for the given view tree
    static std::unique_ptr<PluginViewHost> create(View& root, Size size);

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
