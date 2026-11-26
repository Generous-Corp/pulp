#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/overlay_palette.hpp>

#include <vellum/view/inspector_facts.hpp>

#include <algorithm>
#include <cmath>
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
    if (!active) {
        cancel_field_edit();
        cancel_text_edit();
    }
    active_ = active;
    if (!active) {
        selected_ = nullptr;
        hovered_ = nullptr;
        alt_hover_target_ = nullptr;
        distance_anchor_ = nullptr;
        zoom_active_ = false;
        // The toggle stays, so reopening restores the same viewer.
        atlas_row_count_ = 0;
    }
}

bool InspectorOverlay::select_parent() {
    if (!selected_)
        return false;
    View* parent = selected_->parent();
    if (!parent)
        return false;
    selected_ = parent;
    return true;
}

overlay::CursorAffordance InspectorOverlay::cursor_affordance_at(Point pos) const {
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
    // An open edit owns the keyboard, so a letter typed into it can never
    // flip a mode: whatever the edit does not take falls through to the view
    // tree.
    if (text_editing())
        return handle_text_key(event);
    if (field_edit_.active())
        return handle_edit_key(event);
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
    if (event.key == KeyCode::v && event.modifiers == 0) {
        set_tool(Tool::select);
        return true;
    }
    if (event.key == KeyCode::t && event.modifiers == 0) {
        set_tool(Tool::text);
        return true;
    }
    if (event.key == KeyCode::p && event.modifiers == 0) {
        toggle_pass_viewer();
        return true;
    }
    if (event.key == KeyCode::a && event.modifiers == 0) {
        toggle_atlas_viewer();
        return true;
    }
    if (event.key == KeyCode::m && event.modifiers == 0) {
        toggle_selection_mode();
        return true;
    }
    if (event.key == KeyCode::z && event.modifiers == 0) {
        toggle_zoom();
        return true;
    }
    return false;
}

bool InspectorOverlay::handle_mouse_event(const ::vellum::view::MouseEvent& event) {
    if (!active_)
        return false;

    // The loupe follows every mouse event, not just moves, so it stays under
    // the pointer through a press and a drag. It never consumes one.
    if (zoom_active_)
        zoom_sample_center_ = event.position;

    // The Text tool decides first, so a press never starts a selection.
    if (mouse_text_tool(event, event.hasExplicitPhase() ? event.isPress() : event.is_down))
        return true;

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

void InspectorOverlay::drop_stale_views() {
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
    // A freed edit target is dropped without a write: there is nothing left
    // to restore.
    if (field_target_ && !overlay::tree_contains(root_, field_target_))
        clear_field_edit();
    if (text_target_ && !overlay::tree_contains(root_, text_target_))
        clear_text_edit();
}

void InspectorOverlay::paint(canvas::Canvas& canvas) {
    if (!active_)
        return;
    drop_stale_views();
    // Record the frame whether or not the viewer is showing, so turning it on
    // shows history already gathered.
    capture_pass_frame();
    paint_highlight(canvas);
    paint_distance_lines(canvas);
    if (selected_)
        paint_box_model(canvas, *selected_);
    paint_text_edit(canvas);
    // The pass viewer wins the docked panel when both viewers are on.
    if (pass_viewer_enabled_) {
        atlas_row_count_ = 0;
        paint_pass_panel(canvas);
    } else if (atlas_viewer_visible_) {
        paint_atlas_panel(canvas);
    } else {
        atlas_row_count_ = 0;
    }
    // Last, so the magnified grid sits on top of the highlights.
    if (zoom_active_)
        paint_zoom_panel(canvas);
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
    // While the selection's copy is being edited, the resize box and handles
    // would hide the text, so a thin outline stands in for them.
    if (selection_uses_subtle_edit_outline()) {
        canvas.set_stroke_color(overlay::kHighlightStroke);
        canvas.set_line_width(1.0f);
        canvas.stroke_rect(r.x, r.y, r.width, r.height);
        return;
    }
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

void InspectorOverlay::paint_distance_lines(canvas::Canvas& canvas) {
    const auto paint_one = [&](const View* from, const View* to) {
        if (!from || !to || from == to)
            return;
        const auto a = overlay::bounds_in_root(from, root_);
        const auto b = overlay::bounds_in_root(to, root_);
        const float ax = a.x + a.width / 2;
        const float ay = a.y + a.height / 2;
        const float bx = b.x + b.width / 2;
        const float by = b.y + b.height / 2;

        canvas.set_stroke_color(overlay::kDistanceLine);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(ax, ay, bx, by);

        const float dx = bx - ax;
        const float dy = by - ay;
        const std::string label =
            std::to_string(static_cast<int>(std::sqrt(dx * dx + dy * dy))) + "px";
        const float mx = (ax + bx) / 2;
        const float my = (ay + by) / 2;
        canvas.set_font("monospace", kFontSize);
        const float text_w = canvas.measure_text(label);
        canvas.set_fill_color(overlay::kDistanceLine);
        canvas.fill_rounded_rect(mx - text_w / 2 - 4, my - 8, text_w + 8, 16, 3);
        canvas.set_fill_color(overlay::kBadgeText);
        canvas.fill_text(label, mx - text_w / 2, my + 4);
    };

    // The sticky anchor and the live Alt-hover target can both be showing.
    paint_one(distance_anchor_, selected_);
    if (alt_hover_target_ && selected_ && alt_hover_target_ != distance_anchor_)
        paint_one(selected_, alt_hover_target_);
}

void InspectorOverlay::paint_box_model(canvas::Canvas& canvas, const View& view) {
    // The root has no parent to measure against.
    if (!view.parent())
        return;
    const auto r = overlay::bounds_in_root(&view, root_);
    const auto& f = view.flex();

    // Padding sits inside the view, margin outside it; a negative per-side
    // value means "use the uniform one".
    const float pad_t = f.padding_top >= 0 ? f.padding_top : f.padding;
    const float pad_r = f.padding_right >= 0 ? f.padding_right : f.padding;
    const float pad_b = f.padding_bottom >= 0 ? f.padding_bottom : f.padding;
    const float pad_l = f.padding_left >= 0 ? f.padding_left : f.padding;
    if (pad_t > 0 || pad_r > 0 || pad_b > 0 || pad_l > 0) {
        canvas.set_fill_color(overlay::kPaddingColor);
        if (pad_t > 0)
            canvas.fill_rect(r.x, r.y, r.width, pad_t);
        if (pad_b > 0)
            canvas.fill_rect(r.x, r.y + r.height - pad_b, r.width, pad_b);
        if (pad_l > 0)
            canvas.fill_rect(r.x, r.y + pad_t, pad_l, r.height - pad_t - pad_b);
        if (pad_r > 0)
            canvas.fill_rect(r.x + r.width - pad_r, r.y + pad_t, pad_r, r.height - pad_t - pad_b);
    }

    const float mar_t = f.margin_t();
    const float mar_r = f.margin_r();
    const float mar_b = f.margin_b();
    const float mar_l = f.margin_l();
    if (mar_t > 0 || mar_r > 0 || mar_b > 0 || mar_l > 0) {
        canvas.set_fill_color(overlay::kMarginColor);
        if (mar_t > 0)
            canvas.fill_rect(r.x - mar_l, r.y - mar_t, r.width + mar_l + mar_r, mar_t);
        if (mar_b > 0)
            canvas.fill_rect(r.x - mar_l, r.y + r.height, r.width + mar_l + mar_r, mar_b);
        if (mar_l > 0)
            canvas.fill_rect(r.x - mar_l, r.y, mar_l, r.height);
        if (mar_r > 0)
            canvas.fill_rect(r.x + r.width, r.y, mar_r, r.height);
    }

    // The gap to each parent edge, labelled when it is wide enough to read.
    const auto p = overlay::bounds_in_root(view.parent(), root_);
    const float to_top = r.y - p.y;
    const float to_left = r.x - p.x;
    const float to_bottom = (p.y + p.height) - (r.y + r.height);
    const float to_right = (p.x + p.width) - (r.x + r.width);
    canvas.set_font("monospace", kEdgeFontSize);
    canvas.set_fill_color(overlay::kDistanceLine);
    const auto label = [](float v) { return std::to_string(static_cast<int>(v)); };
    if (to_top > 2)
        canvas.fill_text(label(to_top), r.x + r.width / 2, r.y - 3);
    if (to_left > 2)
        canvas.fill_text(label(to_left), r.x - 20, r.y + r.height / 2);
    if (to_bottom > 2)
        canvas.fill_text(label(to_bottom), r.x + r.width / 2, r.y + r.height + 10);
    if (to_right > 2)
        canvas.fill_text(label(to_right), r.x + r.width + 3, r.y + r.height / 2);
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
    View::set_inspector_text_hook([](const ::vellum::view::TextInputEvent& e, View* event_root) {
        InspectorOverlay* owner = g_owner;
        if (!owner || (event_root && event_root != &owner->inspected_root()))
            return false;
        return owner->handle_text_input(e);
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
    View::set_inspector_text_hook({});
    View::set_inspector_cursor_hook({});
}

} // namespace pulp::vellum_components
