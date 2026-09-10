#pragma once

/// Hover-cursor tracking for platform window hosts.
///
/// Platform cursor APIs are EDGE-driven: AppKit (and Win32's WM_SETCURSOR, and
/// X11's window cursor attribute) re-ask which cursor to show when the pointer
/// MOVES or a button changes. None of them re-ask when the content under a
/// stationary pointer changes. So a layout pass that slides a different widget
/// under a still pointer — a panel opening, a list reflowing, a JS style
/// mutation — leaves the previous cursor on screen until the user jiggles the
/// mouse or clicks.
///
/// `HoverCursorTracker` closes that gap: the host records where the pointer is,
/// and re-resolves the style from its FRAME path rather than only from its
/// mouse-event handlers. Change detection lives here so the host may poll every
/// frame and only touch the platform when the answer actually differs.

#include <optional>

#include <pulp/view/geometry.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

/// The cursor style a pointer at `p` (root-view coordinates) should show over
/// `root`: the style of the view `hit_test` resolves to, or `default_` when the
/// point hits nothing. This is the same rule the platform hosts apply in their
/// pointer handlers, factored out so the frame path can re-run it.
View::CursorStyle hover_cursor_at(View& root, Point p);

/// Remembers the pointer position and the style last handed to the platform.
///
/// A host calls `set_pointer` / `clear_pointer` from its enter/move/exit
/// handlers, `note_published` whenever it pushes a style through its own
/// pointer-driven path, and `poll` once per rendered frame. `poll` yields a
/// style only when it differs from the last published one, so the platform call
/// happens on change rather than on every frame.
class HoverCursorTracker {
public:
    /// Record that the pointer is over the surface at `p` (root coordinates).
    void set_pointer(Point p) {
        pointer_ = p;
        has_pointer_ = true;
    }

    /// The pointer left the surface. Also drops the published style, so the
    /// first resolve after the pointer returns always publishes.
    void clear_pointer() {
        has_pointer_ = false;
        published_ = false;
    }

    bool has_pointer() const { return has_pointer_; }
    Point pointer() const { return pointer_; }

    /// Record a style the host just pushed to the platform itself, so the next
    /// poll does not push the same style again.
    void note_published(View::CursorStyle style) {
        last_ = style;
        published_ = true;
    }

    /// The style last pushed to the platform, or `default_` before any push.
    View::CursorStyle published() const { return last_; }

    /// Change detection over a style the caller resolved itself. Hosts that
    /// layer an overlay or inspector override on top of the hit-test answer
    /// resolve the final style and route it through here. Returns the style
    /// when it differs from the last published one, `nullopt` otherwise, and
    /// records a returned style as published.
    std::optional<View::CursorStyle> poll_resolved(View::CursorStyle style);

    /// Re-resolve at the remembered pointer position and report a change.
    /// `nullopt` when no pointer is over the surface or the style is unchanged.
    /// This is the stationary-pointer path: it takes no event and no new
    /// coordinates, so a content change alone can move the cursor.
    std::optional<View::CursorStyle> poll(View& root);

private:
    Point pointer_{};
    bool has_pointer_ = false;
    bool published_ = false;
    View::CursorStyle last_ = View::CursorStyle::default_;
};

}  // namespace pulp::view
