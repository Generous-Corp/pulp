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

}  // namespace pulp::view
