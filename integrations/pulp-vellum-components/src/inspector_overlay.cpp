#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/overlay_palette.hpp>

#include <vellum/view/inspector_facts.hpp>

#include <algorithm>
#include <string>

namespace pulp::vellum_components {

namespace {

// The hooks are process-wide statics on View. They read this pointer when they
// fire, never capturing the overlay, so clearing it makes any hook still
// installed a no-op.
InspectorOverlay* g_owner = nullptr;

} // namespace

BadgePlacement compute_badge_placement(float sel_x, float sel_y, float sel_h, float badge_w,
                                       float badge_h, float root_w, float gap, float top_margin) {
    BadgePlacement out;
    const float above_y = sel_y - gap - badge_h;
    if (above_y < top_margin) {
        out.y = sel_y + sel_h + gap;
        out.below = true;
    } else {
        out.y = above_y;
    }
    float x = std::max(sel_x, 0.0f);
    if (root_w > 0.0f && x + badge_w > root_w)
        x = root_w - badge_w;
    // A badge wider than the root pins to the left edge.
    out.x = std::max(x, 0.0f);
    return out;
}

InspectorOverlay::InspectorOverlay(View& root) : root_(root) {}

InspectorOverlay::~InspectorOverlay() {
    // An older overlay must not release hooks a newer one has adopted.
    if (g_owner == this)
        uninstall_inspector_hooks();
}

void InspectorOverlay::set_active(bool active) {
    active_ = active;
    if (!active) {
        selected_ = nullptr;
        hovered_ = nullptr;
        alt_hover_target_ = nullptr;
        distance_anchor_ = nullptr;
    }
}

bool InspectorOverlay::select_parent() {
    drop_stale_views();
    if (!selected_)
        return false;
    View* parent = selected_->parent();
    if (!parent)
        return false;
    selected_ = parent;
    return true;
}

overlay::CursorAffordance InspectorOverlay::cursor_affordance_at(Point pos) const {
    // A cursor hook can fire after a tree removal and before the next paint:
    // validate the selection against the live tree before touching it.
    drop_stale_views();
    if (!selected_ || !dragging_enabled_)
        return overlay::CursorAffordance::none;
    return overlay::affordance_at(overlay::bounds_in_root(selected_, root_), pos);
}

int InspectorOverlay::cursor_style_for(Point pos) const {
    return overlay::cursor_style_value(cursor_affordance_at(pos));
}

bool InspectorOverlay::handle_key_event(const ::vellum::view::KeyEvent& event) {
    using ::vellum::view::KeyCode;
    if (event.key == KeyCode::i && event.isMainModifier() && event.is_down) {
        toggle();
        return true;
    }
    if (!active_ || !event.is_down)
        return false;
    if (event.key == KeyCode::escape) {
        // Deselect and stay on, so hovering and clicking keep working without
        // reopening the overlay. Escape never dismisses it.
        if (selected_)
            selected_ = nullptr;
        hovered_ = nullptr;
        alt_hover_target_ = nullptr;
        distance_anchor_ = nullptr;
        return true;
    }
    if (event.key == KeyCode::m && event.modifiers == 0) {
        toggle_selection_mode();
        return true;
    }
    return false;
}

bool InspectorOverlay::handle_mouse_event(const ::vellum::view::MouseEvent& event) {
    if (!active_)
        return false;

    // A host that sets an explicit phase says what the event is. Otherwise
    // the button-down flag marks a press, and nothing here is mid-gesture.
    bool is_press = event.is_down;
    bool is_drag_tick = false;
    bool is_release = false;
    if (event.hasExplicitPhase()) {
        is_press = event.isPress();
        is_drag_tick = event.isDrag();
        is_release = event.isRelease();
    }
    return select_with_mouse(event, is_press, is_drag_tick, is_release);
}

bool InspectorOverlay::select_with_mouse(const ::vellum::view::MouseEvent& event, bool is_press,
                                         bool is_drag_tick, bool is_release) {
    View* hit = root_.hit_test(event.position);
    if (hit) {
        hovered_ = hit;
        // In follows_mouse mode a pure move re-selects the hovered view. A
        // press, a drag tick or a release never does, so a drag begun on empty
        // canvas cannot hop selection mid-drag, and Alt keeps its selection
        // pinned for measuring.
        if (selection_mode_ == SelectionMode::follows_mouse && !is_press && !is_drag_tick &&
            !is_release && !event.isAltDown())
            selected_ = hit;
    }

    // Alt-hover reaches from the selection to the view under the pointer.
    alt_hover_target_ = (event.isAltDown() && selected_ && hit && hit != selected_) ? hit : nullptr;

    if (is_press) {
        // Consume the press so it never reaches a widget under the overlay.
        if (hit) {
            if (event.modifiers & ::vellum::view::kModAlt) {
                if (!distance_anchor_)
                    distance_anchor_ = hit;
                else
                    selected_ = hit;
            } else {
                selected_ = hit;
                distance_anchor_ = nullptr;
            }
        }
        return true;
    }
    // An explicit-phase drag or release over the canvas is also consumed, so
    // it cannot leak through to a widget, but it does not change selection.
    if (event.hasExplicitPhase() && (is_drag_tick || is_release))
        return true;
    // Plain hover: let normal hover effects run.
    return false;
}

void InspectorOverlay::drop_stale_views() const {
    // Compare pointers against the live tree without dereferencing: a view
    // freed by a tree rebuild must not be touched.
    const auto items = overlay::flatten_tree(root_, {});
    const auto in_tree = [&](const View* v) {
        if (!v)
            return true;
        return std::any_of(items.begin(), items.end(),
                           [&](const overlay::FlatTreeItem& item) { return item.view == v; });
    };
    if (!in_tree(selected_))
        selected_ = nullptr;
    if (!in_tree(hovered_))
        hovered_ = nullptr;
    if (!in_tree(distance_anchor_))
        distance_anchor_ = nullptr;
    if (!in_tree(alt_hover_target_))
        alt_hover_target_ = nullptr;
}

void InspectorOverlay::paint(canvas::Canvas& canvas) {
    if (!active_)
        return;
    drop_stale_views();
    paint_highlight(canvas);
}

void InspectorOverlay::paint_highlight(canvas::Canvas& canvas) {
    // A selected view shows one affordance, the orange box. The blue hover box
    // is for a different view you could pick, so it is suppressed for the
    // selection and everything in its ancestry or subtree.
    const auto in_selection_chain = [&](const View* v) {
        if (!selected_ || !v)
            return false;
        return overlay::is_self_or_ancestor(selected_, v) ||
               overlay::is_self_or_ancestor(v, selected_);
    };

    if (hovered_ && hovered_ != selected_ && !in_selection_chain(hovered_)) {
        const auto r = overlay::bounds_in_root(hovered_, root_);
        canvas.set_fill_color(overlay::kHighlightFill);
        canvas.fill_rect(r.x, r.y, r.width, r.height);
        canvas.set_stroke_color(overlay::kHighlightStroke);
        canvas.set_line_width(1.5f);
        canvas.stroke_rect(r.x, r.y, r.width, r.height);

        const std::string label = ::vellum::view::view_type_name(*hovered_) + " " +
                                  std::to_string(static_cast<int>(r.width)) + "\xc3\x97" +
                                  std::to_string(static_cast<int>(r.height));
        canvas.set_font("monospace", kFontSize);
        const float text_w = canvas.measure_text(label);
        const auto place = compute_badge_placement(r.x, r.y, r.height, text_w + 8, kBadgeHeight,
                                                   root_.bounds().width, 2.0f, kBadgeHeight);
        canvas.set_fill_color(overlay::kBadgeBackground);
        canvas.fill_rounded_rect(place.x, place.y, text_w + 8, kBadgeHeight, 3);
        canvas.set_fill_color(overlay::kBadgeText);
        canvas.fill_text(label, place.x + 4, place.y + 13);
    }

    if (!selected_)
        return;
    const auto r = overlay::bounds_in_root(selected_, root_);
    canvas.set_fill_color(overlay::kSelectedFill);
    canvas.fill_rect(r.x, r.y, r.width, r.height);
    canvas.set_stroke_color(overlay::kSelectedStroke);
    canvas.set_line_width(2.0f);
    canvas.stroke_rect(r.x, r.y, r.width, r.height);

    if (!dragging_enabled_)
        return;
    const auto paint_handle = [&](float cx, float cy) {
        canvas.set_fill_color(overlay::kHandleFill);
        canvas.fill_rect(cx - kHandleHalfSide, cy - kHandleHalfSide, kHandleHalfSide * 2,
                         kHandleHalfSide * 2);
        canvas.set_stroke_color(overlay::kHandleStroke);
        canvas.set_line_width(1.0f);
        canvas.stroke_rect(cx - kHandleHalfSide, cy - kHandleHalfSide, kHandleHalfSide * 2,
                           kHandleHalfSide * 2);
    };
    const float mid_x = r.x + r.width * 0.5f;
    const float mid_y = r.y + r.height * 0.5f;
    paint_handle(r.x, r.y);
    paint_handle(r.x + r.width, r.y);
    paint_handle(r.x, r.y + r.height);
    paint_handle(r.x + r.width, r.y + r.height);
    paint_handle(mid_x, r.y);
    paint_handle(mid_x, r.y + r.height);
    paint_handle(r.x + r.width, mid_y);
    paint_handle(r.x, mid_y);
}

void install_inspector_hooks(InspectorOverlay& inspector) {
    g_owner = &inspector;
    View::set_inspector_paint_hook([](canvas::Canvas& canvas, View* painting_root) {
        InspectorOverlay* owner = g_owner;
        if (!owner)
            return;
        // An unknown root (null) paints; a known one must be the inspected root.
        if (painting_root && painting_root != &owner->inspected_root())
            return;
        owner->paint(canvas);
    });
    View::set_inspector_key_hook([](const ::vellum::view::KeyEvent& e) {
        InspectorOverlay* owner = g_owner;
        return owner ? owner->handle_key_event(e) : false;
    });
    View::set_inspector_mouse_hook([](const ::vellum::view::MouseEvent& e, View* event_root) {
        InspectorOverlay* owner = g_owner;
        if (!owner || (event_root && event_root != &owner->inspected_root()))
            return false;
        return owner->handle_mouse_event(e);
    });
    View::set_inspector_cursor_hook([](const ::vellum::view::MouseEvent& e, View* event_root) {
        InspectorOverlay* owner = g_owner;
        if (!owner || (event_root && event_root != &owner->inspected_root()))
            return -1;
        return owner->cursor_style_for(e.position);
    });
}

void uninstall_inspector_hooks() {
    // Clear the owner first so a hook firing between the two steps is inert.
    g_owner = nullptr;
    View::set_inspector_paint_hook({});
    View::set_inspector_key_hook({});
    View::set_inspector_mouse_hook({});
    View::set_inspector_cursor_hook({});
}

} // namespace pulp::vellum_components
