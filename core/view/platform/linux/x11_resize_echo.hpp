#pragma once

namespace pulp::view {

/// Which ConfigureNotify events on the editor's X11 window are news.
///
/// set_size() sends XResizeWindow and only flushes, so the ConfigureNotify for
/// an earlier set_size() can still be queued when a later one drains events.
/// Taken at face value, that stale echo puts the editor back to the earlier
/// size. Every X event carries the serial of the last request the server had
/// processed from this connection, so an event older than the editor's latest
/// XResizeWindow describes a window that resize has since replaced: drop it.
/// An event at or after that serial is the resize's own echo (a no-op, it
/// matches the size just set) or a host resize that came later.
///
/// Serials are the low bits of a request counter and wrap, so they are
/// compared by signed distance, as Xlib compares them.
class X11ResizeEcho {
  public:
    /// Before XResizeWindow: `next_request` is NextRequest(display).
    void note_self_resize(unsigned long next_request) {
        self_resize_serial_ = next_request;
        has_self_resize_ = true;
    }

    /// Whether a ConfigureNotify with this serial predates the last
    /// self-issued resize.
    bool is_stale(unsigned long event_serial) const {
        return has_self_resize_ && static_cast<long>(event_serial - self_resize_serial_) < 0;
    }

  private:
    unsigned long self_resize_serial_ = 0;
    bool has_self_resize_ = false;
};

} // namespace pulp::view
