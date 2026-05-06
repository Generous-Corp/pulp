// Yoga layout adapter — replaces hand-rolled flexbox with Meta's Yoga engine.
// Conditionally compiled only when PULP_HAS_YOGA is defined.

#ifdef PULP_HAS_YOGA

#include <pulp/view/view.hpp>
#include <yoga/Yoga.h>
#include <vector>
#include <algorithm>

namespace pulp::view {

// Map Pulp FlexDirection to Yoga.
// pulp #1434 (rn batch B) — row_reverse / column_reverse now route
// to YGFlexDirectionRowReverse / ColumnReverse. Before, the ternary
// silently mapped them to YGFlexDirectionColumn (the false branch).
static YGFlexDirection to_yg_direction(FlexDirection d) {
    switch (d) {
        case FlexDirection::row:            return YGFlexDirectionRow;
        case FlexDirection::row_reverse:    return YGFlexDirectionRowReverse;
        case FlexDirection::column:         return YGFlexDirectionColumn;
        case FlexDirection::column_reverse: return YGFlexDirectionColumnReverse;
    }
    return YGFlexDirectionColumn;
}

// Map Pulp FlexAlign to Yoga.
// pulp #1434 (rn batch B) — `baseline` added.
static YGAlign to_yg_align(FlexAlign a) {
    switch (a) {
        case FlexAlign::start:    return YGAlignFlexStart;
        case FlexAlign::center:   return YGAlignCenter;
        case FlexAlign::end:      return YGAlignFlexEnd;
        case FlexAlign::stretch:  return YGAlignStretch;
        case FlexAlign::auto_:    return YGAlignAuto;
        case FlexAlign::baseline: return YGAlignBaseline;
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

// Apply FlexStyle to a YGNode.
//
// `is_absolute` is true when the owning View has position:absolute or
// position:fixed. CSS / Yoga semantics: an out-of-flow box is taken out
// of its parent's flex line, so flex_grow / flex_shrink / flex_basis
// must NOT contribute to the parent's main-axis sizing — and crucially,
// must not consume "remaining space" on the parent's flex line. Pulp's
// FlexStyle defaults (flex_shrink = 1) would otherwise leak in and let
// Yoga shrink an absolute-with-explicit-dimension child to fit a flex
// neighbour's slot. See pulp #1379 / #998. Direction / align / justify
// still describe the absolute box's OWN inner layout, so those stay.
static void apply_flex_style(YGNodeRef node, const FlexStyle& f, bool is_absolute) {
    YGNodeStyleSetFlexDirection(node, to_yg_direction(f.direction));
    YGNodeStyleSetAlignItems(node, to_yg_align(f.align_items));
    YGNodeStyleSetAlignSelf(node, to_yg_align(f.align_self));
    YGNodeStyleSetJustifyContent(node, to_yg_justify(f.justify_content));

    if (!is_absolute) {
        // In-flow only: grow/shrink/basis are flex-line participation knobs.
        // Out-of-flow boxes should not carry them — they belong to the
        // containing block sizing path, not the flex line.
        YGNodeStyleSetFlexGrow(node, f.flex_grow);
        YGNodeStyleSetFlexShrink(node, f.flex_shrink);
        // pulp #1434 (rn batch C) — dispatch flex_basis on dim_*.unit:
        // `'auto'` → YGNodeStyleSetFlexBasisAuto; `'50%'` →
        // YGNodeStyleSetFlexBasisPercent; numeric → existing px API.
        if (f.dim_flex_basis.unit == DimensionUnit::auto_) {
            YGNodeStyleSetFlexBasisAuto(node);
        } else if (f.dim_flex_basis.unit == DimensionUnit::percent && f.dim_flex_basis.value > 0) {
            YGNodeStyleSetFlexBasisPercent(node, f.dim_flex_basis.value);
        } else if (f.flex_basis >= 0) {
            YGNodeStyleSetFlexBasis(node, f.flex_basis);
        }
    } else {
        // Be explicit: zero them so a previously-set Yoga node (we don't
        // recycle here today, but defensively) doesn't carry residue, and
        // so Yoga's "absolute child has flex_basis" code path doesn't fire.
        YGNodeStyleSetFlexGrow(node, 0.0f);
        YGNodeStyleSetFlexShrink(node, 0.0f);
    }

    YGNodeStyleSetFlexWrap(node, f.flex_wrap ? YGWrapWrap : YGWrapNoWrap);

    // Gap
    float gap = f.gap;
    float rg = f.row_gap >= 0 ? f.row_gap : gap;
    float cg = f.column_gap >= 0 ? f.column_gap : gap;
    if (rg > 0) YGNodeStyleSetGap(node, YGGutterRow, rg);
    if (cg > 0) YGNodeStyleSetGap(node, YGGutterColumn, cg);

    // Padding — pulp #1434 (cross-surface mega-batch) dispatches on
    // dim_padding_*.unit when set. percent → YGNodeStyleSetPaddingPercent.
    // Yoga's padding does NOT support `auto` (only margin does), so the
    // auto_ unit is treated as no-op here (the bridge already rejects
    // 'auto' on padding edges, but defensive belt-and-suspenders).
    auto apply_padding = [&](YGEdge edge, const Dimension& dim, float legacy_per_edge, float uniform) {
        if (dim.unit == DimensionUnit::percent && dim.value > 0) {
            YGNodeStyleSetPaddingPercent(node, edge, dim.value);
            return;
        }
        float v = legacy_per_edge >= 0 ? legacy_per_edge : uniform;
        if (v > 0) YGNodeStyleSetPadding(node, edge, v);
    };
    apply_padding(YGEdgeTop,    f.dim_padding_top,    f.padding_top,    f.padding);
    apply_padding(YGEdgeRight,  f.dim_padding_right,  f.padding_right,  f.padding);
    apply_padding(YGEdgeBottom, f.dim_padding_bottom, f.padding_bottom, f.padding);
    apply_padding(YGEdgeLeft,   f.dim_padding_left,   f.padding_left,   f.padding);

    // Margin — pulp #1434 (cross-surface mega-batch) dispatches on
    // dim_margin_*.unit. percent → YGNodeStyleSetMarginPercent;
    // auto_ → YGNodeStyleSetMarginAuto (used for centering with
    // `marginLeft: 'auto'; marginRight: 'auto'` etc.). px or unset
    // falls through to the legacy per-edge / uniform float path.
    auto apply_margin = [&](YGEdge edge, const Dimension& dim, float legacy_per_edge, float uniform) {
        if (dim.unit == DimensionUnit::auto_) {
            YGNodeStyleSetMarginAuto(node, edge);
            return;
        }
        if (dim.unit == DimensionUnit::percent && dim.value != 0) {
            YGNodeStyleSetMarginPercent(node, edge, dim.value);
            return;
        }
        float v = legacy_per_edge >= 0 ? legacy_per_edge : uniform;
        if (v > 0) YGNodeStyleSetMargin(node, edge, v);
    };
    apply_margin(YGEdgeTop,    f.dim_margin_top,    f.margin_top,    f.margin);
    apply_margin(YGEdgeRight,  f.dim_margin_right,  f.margin_right,  f.margin);
    apply_margin(YGEdgeBottom, f.dim_margin_bottom, f.margin_bottom, f.margin);
    apply_margin(YGEdgeLeft,   f.dim_margin_left,   f.margin_left,   f.margin);

    // Dimensions — pulp #1423 dispatches on dim_*.unit so width/height
    // accept percentage values. The bridge's setFlex(width|height, ...)
    // path populates dim_width / dim_height with the unit info; this
    // adapter routes percent values to Yoga's native percent API
    // instead of treating "100%" as 100 px.
    if (f.dim_width.unit == DimensionUnit::percent && f.dim_width.value > 0) {
        YGNodeStyleSetWidthPercent(node, f.dim_width.value);
    } else if (f.preferred_width > 0) {
        YGNodeStyleSetWidth(node, f.preferred_width);
    }
    if (f.dim_height.unit == DimensionUnit::percent && f.dim_height.value > 0) {
        YGNodeStyleSetHeightPercent(node, f.dim_height.value);
    } else if (f.preferred_height > 0) {
        YGNodeStyleSetHeight(node, f.preferred_height);
    }
    // pulp #1434 (rn batch C) — min/max width/height dispatch on dim_*.unit
    // for the percent path; existing px path stays for numeric values.
    if (f.dim_min_width.unit == DimensionUnit::percent && f.dim_min_width.value > 0) {
        YGNodeStyleSetMinWidthPercent(node, f.dim_min_width.value);
    } else if (f.min_width > 0) YGNodeStyleSetMinWidth(node, f.min_width);
    if (f.dim_min_height.unit == DimensionUnit::percent && f.dim_min_height.value > 0) {
        YGNodeStyleSetMinHeightPercent(node, f.dim_min_height.value);
    } else if (f.min_height > 0) YGNodeStyleSetMinHeight(node, f.min_height);
    if (f.dim_max_width.unit == DimensionUnit::percent && f.dim_max_width.value > 0) {
        YGNodeStyleSetMaxWidthPercent(node, f.dim_max_width.value);
    } else if (f.max_width > 0) YGNodeStyleSetMaxWidth(node, f.max_width);
    if (f.dim_max_height.unit == DimensionUnit::percent && f.dim_max_height.value > 0) {
        YGNodeStyleSetMaxHeightPercent(node, f.dim_max_height.value);
    } else if (f.max_height > 0) YGNodeStyleSetMaxHeight(node, f.max_height);

    // Aspect ratio (pulp #1434) — Yoga sizes the cross axis from the main
    // axis using `width / height`. Only forward when explicitly set so the
    // default (no aspect constraint) doesn't fight other size constraints.
    // Negative / zero values are nonsensical for ratio semantics; treat
    // those as "clear" so an "auto" or invalid value flowing through the
    // CSS path doesn't pin the cross axis to 0.
    if (f.aspect_ratio.has_value() && *f.aspect_ratio > 0.0f) {
        YGNodeStyleSetAspectRatio(node, *f.aspect_ratio);
    }

    // Order (Yoga doesn't support order directly — we handle it via child insertion order)
}

static void apply_position_style(YGNodeRef node, const View& view) {
    switch (view.position()) {
        case View::Position::absolute:
        case View::Position::fixed:
            YGNodeStyleSetPositionType(node, YGPositionTypeAbsolute);
            break;
        case View::Position::relative:
        case View::Position::static_:
        case View::Position::sticky:
        default:
            YGNodeStyleSetPositionType(node, YGPositionTypeRelative);
            break;
    }

    // pulp #1434 batch 6 — dispatch on per-edge unit so top/right/bottom/left
    // accept percent values. The bridge's setTop / setRight / setBottom /
    // setLeft path populates View::top_unit_ / etc. with the unit info; this
    // adapter routes percent values to Yoga's native percent API instead of
    // treating "50%" as 50 px. Mirrors the FlexStyle::dim_width path from
    // pulp #1423 (PR #1426) for the View positional fields.
    if (view.has_top()) {
        if (view.top_unit() == DimensionUnit::percent) {
            YGNodeStyleSetPositionPercent(node, YGEdgeTop, view.top());
        } else {
            YGNodeStyleSetPosition(node, YGEdgeTop, view.top());
        }
    }
    if (view.has_right()) {
        if (view.right_unit() == DimensionUnit::percent) {
            YGNodeStyleSetPositionPercent(node, YGEdgeRight, view.right());
        } else {
            YGNodeStyleSetPosition(node, YGEdgeRight, view.right());
        }
    }
    if (view.has_bottom()) {
        if (view.bottom_unit() == DimensionUnit::percent) {
            YGNodeStyleSetPositionPercent(node, YGEdgeBottom, view.bottom());
        } else {
            YGNodeStyleSetPosition(node, YGEdgeBottom, view.bottom());
        }
    }
    if (view.has_left()) {
        if (view.left_unit() == DimensionUnit::percent) {
            YGNodeStyleSetPositionPercent(node, YGEdgeLeft, view.left());
        } else {
            YGNodeStyleSetPosition(node, YGEdgeLeft, view.left());
        }
    }
}

// Measure callback for widgets with intrinsic size
static YGSize yoga_measure(YGNodeConstRef node, float width, YGMeasureMode widthMode,
                            float height, YGMeasureMode heightMode) {
    (void) widthMode;
    (void) heightMode;
    auto* view = static_cast<View*>(YGNodeGetContext(node));
    float w = view->intrinsic_width();
    float h = view->intrinsic_height();
    if (w <= 0) w = width;
    if (h <= 0) h = height;
    return {w, h};
}

static std::vector<View*> ordered_visible_children(View& parent) {
    struct ChildEntry { View* view; int order; };
    std::vector<ChildEntry> ordered;
    for (size_t i = 0; i < parent.child_count(); ++i) {
        auto* child = parent.child_at(i);
        if (!child->visible()) continue;
        ordered.push_back({const_cast<View*>(child), child->flex().order});
    }
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const ChildEntry& a, const ChildEntry& b) { return a.order < b.order; });

    std::vector<View*> children;
    children.reserve(ordered.size());
    for (const auto& entry : ordered)
        children.push_back(entry.view);
    return children;
}

static void build_yoga_subtree(View& view, YGNodeRef node) {
    // Position-type wins ordering: tell Yoga "this is absolute" BEFORE
    // any flex-flow attributes are applied, so flex_grow/flex_shrink/
    // flex_basis can be gated on absolute-ness in apply_flex_style and
    // never contribute to the parent's flex line. (pulp #1379 / #998)
    apply_position_style(node, view);

    const bool is_absolute = view.position() == View::Position::absolute
                          || view.position() == View::Position::fixed;
    apply_flex_style(node, view.flex(), is_absolute);
    YGNodeSetContext(node, &view);

    auto children = ordered_visible_children(view);
    bool has_managed_children = !children.empty() && view.layout_mode() != LayoutMode::grid;

    if (!has_managed_children && (view.intrinsic_width() > 0 || view.intrinsic_height() > 0)) {
        YGNodeSetMeasureFunc(node, yoga_measure);
    }

    if (!has_managed_children)
        return;

    for (size_t i = 0; i < children.size(); ++i) {
        auto* child = children[i];
        YGNodeRef ygChild = YGNodeNew();
        build_yoga_subtree(*child, ygChild);
        YGNodeInsertChild(node, ygChild, static_cast<uint32_t>(i));
    }
}

static void apply_yoga_results(View& parent, YGNodeRef node) {
    const uint32_t childCount = YGNodeGetChildCount(node);
    for (uint32_t i = 0; i < childCount; ++i) {
        YGNodeRef childNode = YGNodeGetChild(node, i);
        auto* child = static_cast<View*>(YGNodeGetContext(childNode));
        if (!child) continue;

        child->set_bounds({
            YGNodeLayoutGetLeft(childNode),
            YGNodeLayoutGetTop(childNode),
            YGNodeLayoutGetWidth(childNode),
            YGNodeLayoutGetHeight(childNode)
        });

        if (child->layout_mode() == LayoutMode::grid) {
            child->layout_children();
            continue;
        }

        apply_yoga_results(*child, childNode);
    }
}

// Build YGNode tree from View tree, compute layout, apply results
void yoga_layout(View& root) {
    auto rootBounds = root.local_bounds();

    YGNodeRef ygRoot = YGNodeNew();
    YGNodeStyleSetWidth(ygRoot, rootBounds.width);
    YGNodeStyleSetHeight(ygRoot, rootBounds.height);
    build_yoga_subtree(root, ygRoot);

    YGNodeCalculateLayout(ygRoot, rootBounds.width, rootBounds.height, YGDirectionLTR);
    apply_yoga_results(root, ygRoot);

    YGNodeFreeRecursive(ygRoot);
}

} // namespace pulp::view

#endif // PULP_HAS_YOGA
