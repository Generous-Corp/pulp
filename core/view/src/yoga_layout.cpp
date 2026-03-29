// Yoga layout adapter — replaces hand-rolled flexbox with Meta's Yoga engine.
// Conditionally compiled only when PULP_HAS_YOGA is defined.

#ifdef PULP_HAS_YOGA

#include <pulp/view/view.hpp>
#include <yoga/Yoga.h>
#include <vector>
#include <algorithm>

namespace pulp::view {

// Map Pulp FlexDirection to Yoga
static YGFlexDirection to_yg_direction(FlexDirection d) {
    return d == FlexDirection::row ? YGFlexDirectionRow : YGFlexDirectionColumn;
}

// Map Pulp FlexAlign to Yoga
static YGAlign to_yg_align(FlexAlign a) {
    switch (a) {
        case FlexAlign::start:   return YGAlignFlexStart;
        case FlexAlign::center:  return YGAlignCenter;
        case FlexAlign::end:     return YGAlignFlexEnd;
        case FlexAlign::stretch: return YGAlignStretch;
        case FlexAlign::auto_:   return YGAlignAuto;
        default: return YGAlignStretch;
    }
}

// Map Pulp FlexJustify to Yoga
static YGJustify to_yg_justify(FlexJustify j) {
    switch (j) {
        case FlexJustify::start:         return YGJustifyFlexStart;
        case FlexJustify::center:        return YGJustifyCenter;
        case FlexJustify::end_:          return YGJustifyFlexEnd;
        case FlexJustify::space_between: return YGJustifySpaceBetween;
        case FlexJustify::space_around:  return YGJustifySpaceAround;
        case FlexJustify::space_evenly:  return YGJustifySpaceEvenly;
        default: return YGJustifyFlexStart;
    }
}

// Apply FlexStyle to a YGNode
static void apply_flex_style(YGNodeRef node, const FlexStyle& f) {
    YGNodeStyleSetFlexDirection(node, to_yg_direction(f.direction));
    YGNodeStyleSetAlignItems(node, to_yg_align(f.align_items));
    YGNodeStyleSetAlignSelf(node, to_yg_align(f.align_self));
    YGNodeStyleSetJustifyContent(node, to_yg_justify(f.justify_content));

    YGNodeStyleSetFlexGrow(node, f.flex_grow);
    YGNodeStyleSetFlexShrink(node, f.flex_shrink);
    if (f.flex_basis >= 0) YGNodeStyleSetFlexBasis(node, f.flex_basis);

    YGNodeStyleSetFlexWrap(node, f.flex_wrap ? YGWrapWrap : YGWrapNoWrap);

    // Gap
    float gap = f.gap;
    float rg = f.row_gap >= 0 ? f.row_gap : gap;
    float cg = f.column_gap >= 0 ? f.column_gap : gap;
    if (rg > 0) YGNodeStyleSetGap(node, YGGutterRow, rg);
    if (cg > 0) YGNodeStyleSetGap(node, YGGutterColumn, cg);

    // Padding
    float pt = f.padding_top >= 0 ? f.padding_top : f.padding;
    float pr = f.padding_right >= 0 ? f.padding_right : f.padding;
    float pb = f.padding_bottom >= 0 ? f.padding_bottom : f.padding;
    float pl = f.padding_left >= 0 ? f.padding_left : f.padding;
    if (pt > 0) YGNodeStyleSetPadding(node, YGEdgeTop, pt);
    if (pr > 0) YGNodeStyleSetPadding(node, YGEdgeRight, pr);
    if (pb > 0) YGNodeStyleSetPadding(node, YGEdgeBottom, pb);
    if (pl > 0) YGNodeStyleSetPadding(node, YGEdgeLeft, pl);

    // Margin
    float mt = f.margin_top >= 0 ? f.margin_top : f.margin;
    float mr = f.margin_right >= 0 ? f.margin_right : f.margin;
    float mb = f.margin_bottom >= 0 ? f.margin_bottom : f.margin;
    float ml = f.margin_left >= 0 ? f.margin_left : f.margin;
    if (mt > 0) YGNodeStyleSetMargin(node, YGEdgeTop, mt);
    if (mr > 0) YGNodeStyleSetMargin(node, YGEdgeRight, mr);
    if (mb > 0) YGNodeStyleSetMargin(node, YGEdgeBottom, mb);
    if (ml > 0) YGNodeStyleSetMargin(node, YGEdgeLeft, ml);

    // Dimensions
    if (f.preferred_width > 0) YGNodeStyleSetWidth(node, f.preferred_width);
    if (f.preferred_height > 0) YGNodeStyleSetHeight(node, f.preferred_height);
    if (f.min_width > 0) YGNodeStyleSetMinWidth(node, f.min_width);
    if (f.min_height > 0) YGNodeStyleSetMinHeight(node, f.min_height);
    if (f.max_width > 0) YGNodeStyleSetMaxWidth(node, f.max_width);
    if (f.max_height > 0) YGNodeStyleSetMaxHeight(node, f.max_height);

    // Order (Yoga doesn't support order directly — we handle it via child insertion order)
}

// Measure callback for widgets with intrinsic size
static YGSize yoga_measure(YGNodeConstRef node, float width, YGMeasureMode widthMode,
                            float height, YGMeasureMode heightMode) {
    auto* view = static_cast<View*>(YGNodeGetContext(node));
    float w = view->intrinsic_width();
    float h = view->intrinsic_height();
    if (w <= 0) w = width;
    if (h <= 0) h = height;
    return {w, h};
}

// Build YGNode tree from View tree, compute layout, apply results
void yoga_layout(View& root) {
    auto rootBounds = root.local_bounds();

    // Create root YGNode
    YGNodeRef ygRoot = YGNodeNew();
    YGNodeStyleSetWidth(ygRoot, rootBounds.width);
    YGNodeStyleSetHeight(ygRoot, rootBounds.height);
    apply_flex_style(ygRoot, root.flex());

    // Collect visible children sorted by order
    struct ChildEntry { View* view; int order; };
    std::vector<ChildEntry> ordered;
    for (size_t i = 0; i < root.child_count(); ++i) {
        auto* child = root.child_at(i);
        if (!child->visible()) continue;
        ordered.push_back({const_cast<View*>(child), child->flex().order});
    }
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const ChildEntry& a, const ChildEntry& b) { return a.order < b.order; });

    // Create child YGNodes
    std::vector<YGNodeRef> childNodes;
    for (size_t i = 0; i < ordered.size(); ++i) {
        auto* child = ordered[i].view;
        YGNodeRef ygChild = YGNodeNew();
        apply_flex_style(ygChild, child->flex());
        YGNodeSetContext(ygChild, child);

        // Set measure function for leaf nodes (widgets with intrinsic size)
        if (child->child_count() == 0 && (child->intrinsic_width() > 0 || child->intrinsic_height() > 0)) {
            YGNodeSetMeasureFunc(ygChild, yoga_measure);
        }

        YGNodeInsertChild(ygRoot, ygChild, static_cast<uint32_t>(i));
        childNodes.push_back(ygChild);
    }

    // Compute layout
    YGNodeCalculateLayout(ygRoot, rootBounds.width, rootBounds.height, YGDirectionLTR);

    // Apply results back to Views
    for (size_t i = 0; i < ordered.size(); ++i) {
        auto* child = ordered[i].view;
        float x = YGNodeLayoutGetLeft(childNodes[i]);
        float y = YGNodeLayoutGetTop(childNodes[i]);
        float w = YGNodeLayoutGetWidth(childNodes[i]);
        float h = YGNodeLayoutGetHeight(childNodes[i]);
        child->set_bounds({x, y, w, h});

        // Recurse into children
        child->layout_children();
    }

    // Cleanup YGNodes
    YGNodeFreeRecursive(ygRoot);
}

} // namespace pulp::view

#endif // PULP_HAS_YOGA
