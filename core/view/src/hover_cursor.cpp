#include <pulp/view/hover_cursor.hpp>

#include <pulp/view/overlay_dismissal.hpp>

namespace pulp::view {

View::CursorStyle hover_cursor_at(View& root, Point p) {
    // Resolve what is painted under the pointer, disabled views included. A
    // disabled view is transparent to ordinary hit testing, so without this the
    // cursor of whatever lies beneath it would show over a control that cannot
    // be used. If nothing in the chain is disabled this is the same view the
    // ordinary hit test returns.
    View::DisabledHitTestScope include_disabled;
    View* target = hover_target_at(root, p);
    if (target && !target->effectively_enabled())
        return View::CursorStyle::default_;
    return target ? target->cursor() : View::CursorStyle::default_;
}

std::optional<View::CursorStyle> HoverCursorTracker::poll_resolved(
    View::CursorStyle style) {
    if (published_ && style == last_) return std::nullopt;
    last_ = style;
    published_ = true;
    return style;
}

std::optional<View::CursorStyle> HoverCursorTracker::poll(View& root) {
    if (!has_pointer_) return std::nullopt;
    return poll_resolved(hover_cursor_at(root, pointer_));
}

}  // namespace pulp::view
