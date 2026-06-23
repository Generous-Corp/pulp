#pragma once

#include <pulp/view/view.hpp>
#include <string>
#include <memory>
#include <functional>
#include <cstdint>

namespace pulp::view {

// SDL3-backed window host for cross-platform standalone UI preview
// Uses SDL3 for window management and input, CoreGraphics/etc for rendering
class SdlWindowHost {
public:
    struct Options {
        std::string title = "Pulp";
        uint32_t width = 400;
        uint32_t height = 300;
        bool resizable = true;

        Options() = default;
    };

    // Create an SDL3 window hosting the given view tree
    static std::unique_ptr<SdlWindowHost> create(View& root, Options options);

    virtual ~SdlWindowHost() = default;

    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void repaint() = 0;

    // Run the event loop (blocks until window closed)
    virtual void run_event_loop() = 0;

    // Native content-view handle for outbound file drag (mirrors
    // WindowHost::native_content_view_handle). X11 Window id on Linux, HWND on
    // Windows; nullptr where unsupported so the drag degrades to false. The SDL
    // backend overrides this; the default keeps non-drag SDL hosts buildable.
    virtual void* native_content_view_handle() const { return nullptr; }

    // Set close callback
    virtual void set_close_callback(std::function<void()> cb) = 0;
};

} // namespace pulp::view
