#pragma once

#include <pulp/view/view.hpp>
#include <string>
#include <memory>
#include <functional>

namespace pulp::view {

struct WindowOptions {
    std::string title = "Pulp";
    float width = 400;
    float height = 300;
    bool resizable = true;
    bool use_gpu = false;  ///< Use GPU rendering (Dawn/Skia Graphite) instead of CoreGraphics
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

    // Set a callback for when the window is closed
    virtual void set_close_callback(std::function<void()> cb) = 0;

    // Run the event loop (blocks until the window is closed)
    // Call this for standalone UI preview mode
    virtual void run_event_loop() = 0;
};

} // namespace pulp::view
