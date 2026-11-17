// Linux standalone outbound file-drag backend (XDND source).
//
// View::start_file_drag falls through to the free begin_file_drag(native_view, …)
// when the view tree is owned by a WindowHost (the SDL standalone app) rather
// than a plugin host. On Linux native_view is the X11 Window id
// (SdlWindowHostImpl::native_content_view_handle). We open our OWN Display
// connection and run the shared XDND source handshake from xdnd_source.hpp on
// it. The handshake creates its own protocol source window on that connection,
// because XdndStatus and XdndFinished are delivered to the connection that
// created the window they address; the host window is used only to pick the
// screen.
//
// Compiled only where Xlib links (find_package(X11)); otherwise the
// `#if !defined(__APPLE__) && …` no-op in drag_drop.cpp provides begin_file_drag.

#include "xdnd_source.hpp"

#include <pulp/view/drag_drop.hpp>

#include <chrono>
#include <cstdint>

namespace pulp::view {

bool begin_file_drag(void* native_view, const FileDragRequest& request) {
    if (!native_view || request.file_paths.empty()) return false;
    const Window host_window =
        static_cast<Window>(reinterpret_cast<uintptr_t>(native_view));
    if (!host_window) return false;

    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    int screen = DefaultScreen(display);
    XWindowAttributes wa;
    if (XGetWindowAttributes(display, host_window, &wa))
        screen = XScreenNumberOfScreen(wa.screen);

    const bool ok = xdnd::run_drag_source(display, screen, request.file_paths,
                                          std::chrono::seconds(60));
    XCloseDisplay(display);
    return ok;
}

}  // namespace pulp::view
