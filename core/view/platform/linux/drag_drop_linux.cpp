// Linux standalone outbound file-drag backend (XDND source).
//
// View::start_file_drag falls through to the free begin_file_drag(native_view, …)
// when the view tree is owned by a WindowHost (the SDL standalone app) rather
// than a plugin host. On Linux native_view is the X11 Window id
// (SdlWindowHostImpl::native_content_view_handle). We open our OWN Display
// connection — X11 atoms are server-global, and the source only needs to own the
// XdndSelection + grab the pointer, both of which work for a window id across a
// second connection (same pattern as the X11 plugin host) — and run the shared
// XDND source handshake from xdnd_source.hpp.
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
    const Window source =
        static_cast<Window>(reinterpret_cast<uintptr_t>(native_view));
    if (!source) return false;

    Display* display = XOpenDisplay(nullptr);
    if (!display) return false;
    int screen = DefaultScreen(display);
    XWindowAttributes wa;
    if (XGetWindowAttributes(display, source, &wa))
        screen = XScreenNumberOfScreen(wa.screen);

    const bool ok = xdnd::run_drag_source(display, screen, source,
                                          request.file_paths,
                                          std::chrono::seconds(60));
    XCloseDisplay(display);
    return ok;
}

}  // namespace pulp::view
