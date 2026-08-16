#include "import_validation_bridge.hpp"
#include "motion_geometry_internal.hpp"

#include <pulp/view/ui_components.hpp>  // ScrollView

#include <algorithm>
#include <string>
#include <vector>

namespace pulp::view {

// --- moved verbatim from widget_bridge.cpp ---------------------------------

choc::value::Value make_layout_rect_value(View* v) {
    auto result = choc::value::createObject("");
    if (!v) return result;

    const auto r = motion::resolve_geometry(*v,
                                            motion::GeometrySpace::ViewGlobal,
                                            motion::GeometrySource::Presentation);
    result.addMember("x", choc::value::createFloat64(r.x));
    result.addMember("y", choc::value::createFloat64(r.y));
    result.addMember("width", choc::value::createFloat64(r.width));
    result.addMember("height", choc::value::createFloat64(r.height));
    result.addMember("top", choc::value::createFloat64(r.y));
    result.addMember("left", choc::value::createFloat64(r.x));
    result.addMember("right", choc::value::createFloat64(r.x + r.width));
    result.addMember("bottom", choc::value::createFloat64(r.y + r.height));
    return result;
}

choc::value::Value make_layout_box_metrics_value(View* v) {
    auto result = choc::value::createObject("");
    if (!v) return result;

    const auto b = v->bounds();
    const auto effective_border = [v](bool edge_set, float edge_width) {
        return edge_set ? edge_width : (v->has_border() ? v->border_width() : 0.0f);
    };
    const float left = effective_border(v->has_border_left_set(), v->border_left_width());
    const float right = effective_border(v->has_border_right_set(), v->border_right_width());
    const float top = effective_border(v->has_border_top_set(), v->border_top_width());
    const float bottom = effective_border(v->has_border_bottom_set(), v->border_bottom_width());

    result.addMember("offsetWidth", choc::value::createFloat64(b.width));
    result.addMember("offsetHeight", choc::value::createFloat64(b.height));
    // Import/runtime helpers sometimes paint authored evidence on a generated
    // child (for example a Label inside a semantic HTML button). Expose the
    // child's untransformed parent-local origin alongside the DOM box metrics
    // so evidence captured in the owner's border-box coordinates can be
    // translated into the actual native paint target without guessing border
    // widths or losing nested transforms.
    result.addMember("localX", choc::value::createFloat64(b.x));
    result.addMember("localY", choc::value::createFloat64(b.y));
    result.addMember("borderLeftWidth", choc::value::createFloat64(left));
    result.addMember("borderTopWidth", choc::value::createFloat64(top));
    result.addMember("borderRightWidth", choc::value::createFloat64(right));
    result.addMember("borderBottomWidth", choc::value::createFloat64(bottom));
    result.addMember("marginLeft", choc::value::createFloat64(v->flex().margin_l()));
    result.addMember("marginTop", choc::value::createFloat64(v->flex().margin_t()));
    result.addMember("marginRight", choc::value::createFloat64(v->flex().margin_r()));
    result.addMember("marginBottom", choc::value::createFloat64(v->flex().margin_b()));
    result.addMember("clientWidth", choc::value::createFloat64(std::max(0.0f, b.width - left - right)));
    result.addMember("clientHeight", choc::value::createFloat64(std::max(0.0f, b.height - top - bottom)));
    return result;
}

static std::string layout_trace_id(const View& v) {
    if (!v.anchor_id().empty()) return v.anchor_id();
    if (!v.id().empty()) return v.id();
    return {};
}

choc::value::Value make_layout_ancestor_chain_value(View* v) {
    auto result = choc::value::createEmptyArray();
    if (!v) return result;

    std::vector<View*> chain;
    for (auto* cur = v; cur != nullptr; cur = cur->parent())
        chain.push_back(cur);
    std::reverse(chain.begin(), chain.end());

    for (auto* cur : chain) {
        auto id = layout_trace_id(*cur);
        if (id.empty()) continue;
        auto entry = choc::value::createObject("");
        entry.addMember("id", id);
        entry.addMember("bounds", make_layout_rect_value(cur));
        result.addArrayElement(entry);
    }
    return result;
}

// --- moved verbatim from claude_bundle.cpp ---------------------------------

void layout_runtime_snapshot_root_if_requested(View& root, const ClaudeRuntimeOptions& opts) {
    if (opts.runtime_snapshot_viewport_width <= 0 || opts.runtime_snapshot_viewport_height <= 0)
        return;

    root.set_bounds({0.0f,
                     0.0f,
                     static_cast<float>(opts.runtime_snapshot_viewport_width),
                     static_cast<float>(opts.runtime_snapshot_viewport_height)});
    root.layout_children();
}

}  // namespace pulp::view
