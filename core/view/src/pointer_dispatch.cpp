#include <pulp/view/pointer_dispatch.hpp>

#include <pulp/view/ui_components.hpp>  // ScrollView

#include <algorithm>
#include <vector>

namespace pulp::view {

Point point_to_local(Point root_pos, View* target, View* root) {
    // Convert root-local `root_pos` into `target`'s local-pre-scale
    // coordinates, accounting for any ancestor's set_scale transform. The
    // forward render chain is:
    //   visual_pos = sum over ancestors A of (A.bounds * scale_chain_above_A)
    //              + (target_local * scale_chain_above_target)
    // Inverse: walk root→target, peel off each ancestor's offset (scaled by the
    // chain above it), then divide the residual by the final scale_chain.
    if (!target || !root) return root_pos;

    std::vector<View*> chain;
    for (auto* v = target; v && v != root; v = v->parent()) chain.push_back(v);
    std::reverse(chain.begin(), chain.end());  // root_child .. target

    auto local = root_pos;
    float scale_chain = 1.0f;  // accumulated scale from root down to current ancestor

    // `root_pos` is already root-local, so root's own bounds offset is NOT
    // subtracted — but a root ScrollView still paints its children shifted by
    // -scroll (and ScrollView::hit_test adds +scroll), so peel that off too.
    // Missing this made a widget jump on drag once the page was scrolled.
    if (auto* root_sv = dynamic_cast<ScrollView*>(root)) {
        local.x += root_sv->scroll_x();
        local.y += root_sv->scroll_y();
    }
    for (auto* v : chain) {
        local.x -= v->bounds().x * scale_chain;
        local.y -= v->bounds().y * scale_chain;
        scale_chain *= v->scale();
        if (auto* sv = dynamic_cast<ScrollView*>(v)) {
            local.x += sv->scroll_x() * scale_chain;
            local.y += sv->scroll_y() * scale_chain;
        }
    }
    if (scale_chain != 0.0f && scale_chain != 1.0f) {
        local.x /= scale_chain;
        local.y /= scale_chain;
    }
    return local;
}

bool dispatch_context_menu(View& root, Point root_pos) {
    View* target = root.hit_test(root_pos);
    if (!target || !target->on_context_menu) return false;
    target->on_context_menu(point_to_local(root_pos, target, &root));
    return true;
}

bool should_yield_to_gesture(View& root, const MouseEvent& event) {
    // Dispatch first and unconditionally — the arbiter must see every event to
    // advance its session — then gate the caller's delivery on a real CLAIM.
    const bool consumed = root.dispatch_gesture_pointer_event(event);
    return consumed && root.gesture_claimed_pointer();
}

namespace {
// Pointer-compare only, so it is safe to call with a `needle` that points at
// freed memory: a captured drag target can be unmounted (and destroyed) between
// the press and the next event of the same gesture. Internal — the platform
// hosts have their own copy under their own name; this one guards the shared
// delivery below, including BETWEEN channels (a modern handler may unmount the
// very view the legacy handler is about to be called on).
bool still_in_tree(View* needle, View* root) {
    if (!needle || !root) return false;
    if (needle == root) return true;
    for (size_t i = 0; i < root->child_count(); ++i)
        if (still_in_tree(needle, root->child_at(i))) return true;
    return false;
}

// Blur every root-local focus holder except `keep`. Focus callbacks may
// synchronously claim a replacement (IME commit is the common case), so a
// single snapshot is insufficient. The bounded fallback guarantees hostile
// callbacks cannot spin forever while still leaving a focus slot latched.
bool drain_root_focus(View& root, View* keep, View* protected_target) {
    constexpr size_t kMaxReentrantFocusHops = 64;
    for (size_t hop = 0; hop < kMaxReentrantFocusHops; ++hop) {
        if (protected_target &&
            !still_in_tree(protected_target, &root))
            return false;

        auto& focus = root.interaction().focused_input;
        View* current = focus;
        if (!current || current == keep) return true;
        if (!still_in_tree(current, &root)) {
            focus = nullptr;
            if (View::focused_input_ == current)
                View::focused_input_ = nullptr;
            continue;
        }

        current->release_input_focus();
        current->on_focus_changed(false);
    }

    // Pathological focus callbacks can form a cycle. Do not call the virtual
    // callback again here; force-clear the final root slot and base visual
    // state without giving the cycle another chance to relatch.
    auto& focus = root.interaction().focused_input;
    View* current = focus;
    if (current && current != keep) {
        if (still_in_tree(current, &root)) {
            current->release_input_focus();
            current->View::on_focus_changed(false);
        } else {
            focus = nullptr;
            if (View::focused_input_ == current)
                View::focused_input_ = nullptr;
        }
    }
    return !protected_target || still_in_tree(protected_target, &root);
}
}  // namespace

bool yield_to_gesture_with_handoff(View& root, View*& drag_target,
                                   const MouseEvent& event, int click_count) {
    if (event.button != MouseButton::left ||
        !should_yield_to_gesture(root, event))
        return false;

    View* handoff_target = drag_target;
    drag_target = nullptr;
    if (event.phase != MousePhase::press) {
        deliver_gesture_handoff(root, handoff_target, event.window_position,
                                event.modifiers, click_count);
    }
    return true;
}

bool transfer_input_focus(View& root, View* target) {
    if (target && !still_in_tree(target, &root)) return false;
    const bool target_focusable = target && target->focusable();
    View* keep = target_focusable ? target : nullptr;
    if (!drain_root_focus(root, keep, target)) return false;
    if (!target) return false;
    if (!target_focusable) return still_in_tree(target, &root);
    if (root.interaction().focused_input == target) return true;

    target->on_focus_changed(true);
    if (!still_in_tree(target, &root)) return false;
    // A gain callback may also claim a replacement. The explicit clicked
    // target still wins; blur that replacement before publishing the target.
    if (!drain_root_focus(root, target, target)) return false;
    target->claim_input_focus();
    return true;
}

View* focused_input_under_root(View& root) {
    View* focused = root.interaction().focused_input;
    return still_in_tree(focused, &root) ? focused : nullptr;
}

void deliver_mouse_drag(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count,
                        PointerType pointer_type, float pressure) {
    deliver_mouse_drag(root, target, root_pt, modifiers, click_count,
                       pointer_type, pressure, MouseButton::left);
}

void deliver_mouse_drag(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count,
                        PointerType pointer_type, float pressure,
                        MouseButton button) {
    deliver_mouse_drag(root, target, root_pt, modifiers, click_count, button,
                       PointerAttributes{pointer_type, pressure, 0});
}

void deliver_mouse_drag(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count,
                        MouseButton button, const PointerAttributes& pointer) {
    if (!still_in_tree(target, &root)) return;

    const Point local = point_to_local(root_pt, target, &root);

    // 1. Modern channel. This is the only place a drag carries its modifiers.
    MouseEvent me;
    me.position = local;
    me.window_position = root_pt;
    me.button = button;
    me.modifiers = modifiers;
    me.click_count = click_count;
    me.is_down = true;  // the button is still held during a drag
    me.phase = MousePhase::drag;
    me.pointer_type = pointer.type;
    me.pressure = pointer.pressure;
    me.pointer_id = pointer.pointer_id;
    target->on_mouse_event(me);

    // The legacy callbacks carry no button identity and historically mean the
    // primary/left button. Sending right or middle input through them would
    // make an auxiliary drag adjust ordinary knobs as if it were left-dragged.
    if (button != MouseButton::left) return;

    // A modern handler may unmount the tree it was dispatched into (a state
    // change that destroys the widget). Re-validate before the legacy hop.
    if (!still_in_tree(target, &root)) return;

    // 2. Legacy channel (bare Point; no modifiers). Kept for compatibility.
    target->on_mouse_drag(local);
    if (!still_in_tree(target, &root)) return;
    if (target->on_drag) target->on_drag(local);
    if (!still_in_tree(target, &root)) return;

    // 3. Ancestor bubbling: a presentational leaf is often the hit target while
    // the drag handler lives on an outer wrapper.
    for (auto* b = target->parent(); b; b = b->parent()) {
        if (!b->on_drag) continue;
        b->on_drag(point_to_local(root_pt, b, &root));
    }
}

bool deliver_mouse_down(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count, bool bubble) {
    return deliver_mouse_down(root, target, root_pt, modifiers, click_count,
                              bubble, MouseButton::left);
}

bool deliver_mouse_down(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count, bool bubble,
                        MouseButton button) {
    return deliver_mouse_down(root, target, root_pt, modifiers, click_count,
                              bubble, button, MouseDownHost{});
}

bool deliver_mouse_down(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count, bool bubble,
                        MouseButton button, const MouseDownHost& host) {
    return deliver_mouse_down(root, target, root_pt, modifiers, click_count,
                              bubble, button, host, PointerAttributes{});
}

bool deliver_mouse_down(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count, bool bubble,
                        MouseButton button, const MouseDownHost& host,
                        const PointerAttributes& pointer) {
    if (!still_in_tree(target, &root)) return false;
    const auto should_continue = [&] {
        return !host.should_continue || host.should_continue();
    };

    const Point local = point_to_local(root_pt, target, &root);

    // 1. Modern channel (press): the only channel carrying modifiers/click_count.
    MouseEvent me;
    me.position = local;
    me.window_position = root_pt;
    me.button = button;
    me.modifiers = modifiers;
    me.click_count = click_count;
    me.is_down = true;
    me.phase = MousePhase::press;
    me.pointer_type = pointer.type;
    me.pressure = pointer.pressure;
    me.pointer_id = pointer.pointer_id;
    target->on_mouse_event(me);

    // A modern handler may unmount the tree it was dispatched into. Re-validate
    // before the legacy hop and the bubble so no channel derefs a freed view.
    if (!should_continue() || !still_in_tree(target, &root)) return false;

    // 2. Legacy channel (bare Point). It has no button identity, so it remains
    // left-only; right/middle continue to the modern ancestor bubble below.
    if (button == MouseButton::left) {
        target->on_mouse_down(local);
        if (!should_continue() || !still_in_tree(target, &root)) return false;
    }

    // 3. W3C pointerdown bubble: a wrap-div around a canvas child (which wins
    // hit_test) never sees the press otherwise. Each ancestor gets a copy of the
    // press re-localized to its own space.
    if (bubble) {
        for (auto* b = target->parent(); b; b = b->parent()) {
            if (!b->on_pointer_event) continue;
            MouseEvent bme = me;
            bme.position = point_to_local(root_pt, b, &root);
            b->on_pointer_event(bme);
            if (!should_continue() || !still_in_tree(target, &root))
                return false;
        }
    }
    return true;
}

void deliver_mouse_up(View& root, View* target, Point root_pt,
                      uint16_t modifiers, int click_count,
                      const MouseUpHost& host) {
    deliver_mouse_up(root, target, root_pt, modifiers, click_count, host,
                     MouseButton::left);
}

void deliver_mouse_up(View& root, View* target, Point root_pt,
                      uint16_t modifiers, int click_count,
                      const MouseUpHost& host, MouseButton button) {
    deliver_mouse_up(root, target, root_pt, modifiers, click_count, host, button,
                     PointerAttributes{});
}

void deliver_mouse_up(View& root, View* target, Point root_pt,
                      uint16_t modifiers, int click_count,
                      const MouseUpHost& host, MouseButton button,
                      const PointerAttributes& pointer) {
    if (!still_in_tree(target, &root)) return;

    const Point local = point_to_local(root_pt, target, &root);

    // Capture the click decision inputs BEFORE any delivery. `on_mouse_up` can
    // unmount `target`; the deferred/standalone click still needs the handler,
    // id, and the same-target verdict from before it ran.
    View* released = root.hit_test(root_pt);
    View* click_target = target;
    while (click_target && !click_target->on_click) click_target = click_target->parent();
    std::function<void()> click_handler =
        click_target ? click_target->on_click : std::function<void()>{};
    const std::string clicked_id = target->id();

    // 1. Legacy channel (up is legacy-before-modern, unlike drag). The legacy
    // API has no button field and therefore remains primary/left-only.
    if (button == MouseButton::left) target->on_mouse_up(local);

    // 2. Modern channel (release). Re-validate — on_mouse_up may have unmounted
    // the target (a React flush on release), which would leave the modern deref
    // and the bubble dangling.
    if (still_in_tree(target, &root)) {
        MouseEvent me;
        me.position = local;
        me.window_position = root_pt;
        me.button = button;
        me.modifiers = modifiers;
        me.click_count = click_count;
        me.is_down = false;
        me.phase = MousePhase::release;
        me.pointer_type = pointer.type;
        me.pressure = pointer.pressure;
        me.pointer_id = pointer.pointer_id;
        target->on_mouse_event(me);

        // 3. W3C pointerup bubble (mirrors the pointerdown bubble).
        if (still_in_tree(target, &root)) {
            for (auto* b = target->parent(); b; b = b->parent()) {
                if (!b->on_pointer_event) continue;
                MouseEvent bme = me;
                bme.position = point_to_local(root_pt, b, &root);
                b->on_pointer_event(bme);
            }
        }
    }

    // 4. Same-target click suppression: fire only when the release landed on the
    // press target. The decision uses the captured pointers (matches the host
    // behavior of comparing the captured drag-target), independent of any
    // unmount during delivery — the host's fire_click owns post-teardown safety.
    if (released == target && host.fire_click)
        host.fire_click(click_handler, clicked_id, modifiers);
}

void deliver_mouse_wheel(View& root, Point root_pt,
                         float scroll_delta_x, float scroll_delta_y,
                         const WheelHost& host) {
    const auto repaint = [&] { if (host.request_repaint) host.request_repaint(); };

    // 1. An open ComboBox popup consumes the wheel to scroll its (clamped) item
    // list, ahead of any enclosing ScrollView (whose scroll would close it). The
    // dropdown paints as an overlay with no view backing, so a plain hit_test
    // lands on the sibling underneath — this mirrors the popup bypass so the
    // wheel scrolls the open menu, not the page behind it.
    if (auto* combo = ComboBox::active_popup_) {
        float ddx = 0, ddy = 0, ddw = 0, ddh = 0;
        if (combo->dropdown_window_rect(ddx, ddy, ddw, ddh) &&
            root_pt.x >= ddx && root_pt.x <= ddx + ddw &&
            root_pt.y >= ddy && root_pt.y <= ddy + ddh) {
            MouseEvent me;
            me.position = point_to_local(root_pt, combo, &root);
            me.window_position = root_pt;
            me.is_wheel = true;
            me.scroll_delta_x = scroll_delta_x;
            me.scroll_delta_y = scroll_delta_y;
            combo->on_mouse_event(me);
            repaint();
            return;
        }
    }

    auto* target = root.hit_test(root_pt);
    if (!target) {
        // 2. Hovering over empty background inside a scroll pane returns no hit
        // because there is no hit-testable child under the point. Route it to
        // the scroll container the cursor is over so scrolling works anywhere in
        // the pane without a click first.
        if (auto* scroll = find_wheel_scroll_view_at(root, root_pt)) {
            MouseEvent me;
            me.position = root_pt;
            me.window_position = root_pt;
            me.is_wheel = true;
            me.scroll_delta_x = scroll_delta_x;
            me.scroll_delta_y = scroll_delta_y;
            scroll->on_mouse_event(me);
            scroll->layout_children();
            repaint();
        }
        return;
    }

    MouseEvent me;
    me.position = root_pt;
    // Set window_position so a WidgetBridge wheel registrar can emit valid
    // clientX/clientY — without this, JSX `onWheel` handlers that do
    // `e.clientX - rect.left` (e.g. anchor-frequency for trackpad zoom) get
    // `0 - rect.left` and the wrong anchor.
    me.window_position = root_pt;
    me.is_wheel = true;
    me.scroll_delta_x = scroll_delta_x;
    me.scroll_delta_y = scroll_delta_y;

    // 3. Value widgets (knob / fader / slider / stepper / pan) under the cursor
    // consume the wheel to adjust their value, taking precedence over an
    // enclosing ScrollView — so "hover + scroll" tweaks the control rather than
    // scrolling the page.
    if (target->wants_wheel_value()) {
        target->on_wheel(me.scroll_delta_y);
        repaint();
        return;
    }

    // 4. W3C wheel bubble: dispatch to every ancestor with on_pointer_event set.
    // Each handler self-filters on me.is_wheel (registerPointer short-circuits
    // when is_wheel is true, registerWheel when it is false), so a view that
    // registered both gets both halves and one that registered only one ignores
    // the other. Stopping at the first ancestor with on_pointer_event (an older
    // approach) was wrong: it stopped at a canvas child that registered ONLY
    // pointer events, so the wheel never reached the ancestor wrap-div that
    // registered the zoom handler. A wants_wheel_scroll ancestor still wins and
    // terminates the walk.
    for (auto* v = target; v; v = v->parent()) {
        if (v->wants_wheel_scroll()) {
            v->on_mouse_event(me);
            v->layout_children();
            repaint();
            return;
        }
        if (v->on_pointer_event) v->on_mouse_event(me);
    }
    // 5. No ancestor handled the wheel — deliver to the deepest hit so any
    // default behavior still runs.
    target->on_mouse_event(me);
    repaint();
}

void deliver_gesture_handoff(View& root, View* target, Point root_pt,
                             uint16_t modifiers, int click_count) {
    // Liveness is deliver_mouse_up's own precondition, so no separate guard is
    // needed here. Empty MouseUpHost: a handoff must not synthesize a click.
    deliver_mouse_up(root, target, root_pt, modifiers, click_count,
                     MouseUpHost{});
}

}  // namespace pulp::view
