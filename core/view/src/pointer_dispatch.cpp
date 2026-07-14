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
}  // namespace

void deliver_mouse_drag(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count,
                        PointerType pointer_type, float pressure) {
    if (!still_in_tree(target, &root)) return;

    const Point local = point_to_local(root_pt, target, &root);

    // 1. Modern channel. This is the only place a drag carries its modifiers.
    MouseEvent me;
    me.position = local;
    me.window_position = root_pt;
    me.button = MouseButton::left;
    me.modifiers = modifiers;
    me.click_count = click_count;
    me.is_down = true;  // the button is still held during a drag
    me.phase = MousePhase::drag;
    me.pointer_type = pointer_type;
    me.pressure = pressure;
    target->on_mouse_event(me);

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

}  // namespace pulp::view
