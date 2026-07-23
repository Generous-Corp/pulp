// Yoga layout adapter — drives View-tree layout via Meta's Yoga engine.
// Conditionally compiled only when PULP_HAS_YOGA is defined.

#ifdef PULP_HAS_YOGA

#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <yoga/Yoga.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace pulp::view {

// Diagnostic: dump the computed bounds tree after Yoga layout.
// Gated by PULP_DUMP_BOUNDS so it never runs in production.
static void dump_bounds_tree(const View& view, int depth) {
    auto b = view.bounds();
    const char* pos_str = "static";
    switch (view.position()) {
        case View::Position::absolute: pos_str = "abs"; break;
        case View::Position::fixed:    pos_str = "fix"; break;
        case View::Position::relative: pos_str = "rel"; break;
        case View::Position::sticky:   pos_str = "sticky"; break;
        default: break;
    }
    std::fprintf(stderr, "[bounds] %*s%-3s bounds=(%.0f,%.0f,%.0fx%.0f)",
                 depth * 2, "", pos_str, b.x, b.y, b.width, b.height);
    if (view.has_top())    std::fprintf(stderr, " top=%.0f%s",    view.top(),    view.top_unit()==DimensionUnit::percent?"%":"");
    if (view.has_right())  std::fprintf(stderr, " right=%.0f%s",  view.right(),  view.right_unit()==DimensionUnit::percent?"%":"");
    if (view.has_bottom()) std::fprintf(stderr, " bottom=%.0f%s", view.bottom(), view.bottom_unit()==DimensionUnit::percent?"%":"");
    if (view.has_left())   std::fprintf(stderr, " left=%.0f%s",   view.left(),   view.left_unit()==DimensionUnit::percent?"%":"");
    if (view.flex().preferred_width > 0)  std::fprintf(stderr, " pw=%.0f", view.flex().preferred_width);
    if (view.flex().preferred_height > 0) std::fprintf(stderr, " ph=%.0f", view.flex().preferred_height);
    if (view.flex().dim_width.value != 0)  std::fprintf(stderr, " dw=%.0f%s", view.flex().dim_width.value, view.flex().dim_width.unit==DimensionUnit::percent?"%":"px");
    if (view.flex().dim_height.value != 0) std::fprintf(stderr, " dh=%.0f%s", view.flex().dim_height.value, view.flex().dim_height.unit==DimensionUnit::percent?"%":"px");
    std::fprintf(stderr, " children=%zu\n", view.child_count());
    for (size_t i = 0; i < view.child_count(); ++i) {
        dump_bounds_tree(*view.child_at(i), depth + 1);
    }
}

// Map Pulp FlexDirection to Yoga.
// Each Pulp direction maps to the matching Yoga enum. The final return is a
// defensive fallback for exhaustive-switch hygiene.
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

// align-content has a wider value vocabulary than align-items / align-self
// because the space-* distributions (space-between / space-around /
// space-evenly) are meaningful here but not on the per-item alignment axis.
// We carry the space variant on a sibling FlexStyle::AlignContentSpace enum
// and dispatch here so the rest of the flow stays uniform with FlexAlign.
static YGAlign to_yg_align_content(FlexAlign a, FlexStyle::AlignContentSpace s) {
    using AcSpace = FlexStyle::AlignContentSpace;
    if (s == AcSpace::space_between) return YGAlignSpaceBetween;
    if (s == AcSpace::space_around)  return YGAlignSpaceAround;
    if (s == AcSpace::space_evenly)  return YGAlignSpaceEvenly;
    return to_yg_align(a);
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
// neighbor's slot. Direction / align / justify still describe the absolute
// box's own inner layout, so those stay.
static void apply_flex_style(YGNodeRef node, const FlexStyle& f, bool is_absolute) {
    YGNodeStyleSetFlexDirection(node, to_yg_direction(f.direction));
    YGNodeStyleSetAlignItems(node, to_yg_align(f.align_items));
    YGNodeStyleSetAlignSelf(node, to_yg_align(f.align_self));
    // Multi-line flex cross-axis distribution. Default (FlexAlign::start,
    // AlignContentSpace::none) maps to YGAlignFlexStart, matching Yoga's
    // default.
    YGNodeStyleSetAlignContent(node, to_yg_align_content(f.align_content, f.align_content_space));
    YGNodeStyleSetJustifyContent(node, to_yg_justify(f.justify_content));

    if (!is_absolute) {
        // In-flow only: grow/shrink/basis are flex-line participation knobs.
        // Out-of-flow boxes should not carry them — they belong to the
        // containing block sizing path, not the flex line.
        YGNodeStyleSetFlexGrow(node, f.flex_grow);
        YGNodeStyleSetFlexShrink(node, f.flex_shrink);
        // Dispatch flex_basis on dim_*.unit:
        // `'auto'` → YGNodeStyleSetFlexBasisAuto; `'50%'` →
        // YGNodeStyleSetFlexBasisPercent; numeric → existing px API.
        if (f.dim_flex_basis.unit == DimensionUnit::auto_) {
            YGNodeStyleSetFlexBasisAuto(node);
        } else if (f.dim_flex_basis.unit == DimensionUnit::percent && f.dim_flex_basis.value >= 0) {
            YGNodeStyleSetFlexBasisPercent(node, f.dim_flex_basis.value);
        } else if (f.flex_basis >= 0) {
            YGNodeStyleSetFlexBasis(node, f.flex_basis);
        }
    } else {
        // Out-of-flow boxes must not participate in the parent's flex line;
        // force grow/shrink to 0 regardless of FlexStyle defaults.
        YGNodeStyleSetFlexGrow(node, 0.0f);
        YGNodeStyleSetFlexShrink(node, 0.0f);
    }

    // Tri-state wrap: YGWrapWrapReverse covers `wrap-reverse`.
    YGWrap yoga_wrap = YGWrapNoWrap;
    if (f.flex_wrap == FlexWrap::wrap)              yoga_wrap = YGWrapWrap;
    else if (f.flex_wrap == FlexWrap::wrap_reverse) yoga_wrap = YGWrapWrapReverse;
    YGNodeStyleSetFlexWrap(node, yoga_wrap);

    // CSS box-sizing. Yoga 3.x's native `YGNodeStyleSetBoxSizing` does the
    // spec-correct math so we don't have to subtract padding+border from
    // declared dimensions ourselves. Default content_box matches the CSS spec.
    YGNodeStyleSetBoxSizing(node,
        f.box_sizing == BoxSizing::border_box
            ? YGBoxSizingBorderBox
            : YGBoxSizingContentBox);

    // Gap
    float gap = f.gap;
    float rg = f.row_gap >= 0 ? f.row_gap : gap;
    float cg = f.column_gap >= 0 ? f.column_gap : gap;
    if (rg > 0) YGNodeStyleSetGap(node, YGGutterRow, rg);
    if (cg > 0) YGNodeStyleSetGap(node, YGGutterColumn, cg);

    // Padding dispatches on dim_padding_*.unit when set.
    // percent → YGNodeStyleSetPaddingPercent.
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

    // Margin dispatches on dim_margin_*.unit.
    // percent → YGNodeStyleSetMarginPercent;
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
        // When the bridge explicitly set a px dimension, route the typed
        // value through — preserves the sign so CSS-spec negative margins
        // actually reach Yoga. The legacy float path below only handled
        // positives and silently dropped negatives.
        if (dim.unit == DimensionUnit::px && dim.value != 0) {
            YGNodeStyleSetMargin(node, edge, dim.value);
            return;
        }
        float v = legacy_per_edge >= 0 ? legacy_per_edge : uniform;
        if (v > 0) YGNodeStyleSetMargin(node, edge, v);
    };
    apply_margin(YGEdgeTop,    f.dim_margin_top,    f.margin_top,    f.margin);
    apply_margin(YGEdgeRight,  f.dim_margin_right,  f.margin_right,  f.margin);
    apply_margin(YGEdgeBottom, f.dim_margin_bottom, f.margin_bottom, f.margin);
    apply_margin(YGEdgeLeft,   f.dim_margin_left,   f.margin_left,   f.margin);

    // Yoga logical-edge fan-out. CSS / RN
    // `marginStart` / `marginEnd` / `paddingStart` / `paddingEnd` /
    // `start` / `end` translate to Yoga's `YGEdgeStart` / `YGEdgeEnd`
    // which Yoga resolves against the node's writing direction (set by
    // build_yoga_subtree via YGNodeStyleSetDirection). Margins and positions dispatch
    // auto_ plus non-zero percent/px values; padding has no auto API and only
    // dispatches positive percent/px values. An unset start/end must not
    // override the per-side left/right that ran above.
    auto apply_logical_margin = [&](YGEdge edge, const Dimension& dim) {
        if (dim.unit == DimensionUnit::auto_) {
            YGNodeStyleSetMarginAuto(node, edge);
            return;
        }
        if (dim.unit == DimensionUnit::percent && dim.value != 0) {
            YGNodeStyleSetMarginPercent(node, edge, dim.value);
            return;
        }
        if (dim.unit == DimensionUnit::px && dim.value != 0) {
            YGNodeStyleSetMargin(node, edge, dim.value);
        }
    };
    auto apply_logical_padding = [&](YGEdge edge, const Dimension& dim) {
        // Yoga padding has no `auto` API (margin only); the bridge
        // already rejects 'auto' on padding edges. Be defensive.
        if (dim.unit == DimensionUnit::percent && dim.value > 0) {
            YGNodeStyleSetPaddingPercent(node, edge, dim.value);
            return;
        }
        if (dim.unit == DimensionUnit::px && dim.value > 0) {
            YGNodeStyleSetPadding(node, edge, dim.value);
        }
    };
    auto apply_logical_position = [&](YGEdge edge, const Dimension& dim) {
        // Yoga has a position-auto API (`YGNodeStyleSetPositionAuto`).
        // `inset-inline-start: auto` is the CSS default ("not anchored to
        // that edge"); routing the keyword through here lets it take effect
        // instead of being silently dropped.
        if (dim.unit == DimensionUnit::auto_) {
            YGNodeStyleSetPositionAuto(node, edge);
            return;
        }
        if (dim.unit == DimensionUnit::percent && dim.value != 0) {
            YGNodeStyleSetPositionPercent(node, edge, dim.value);
            return;
        }
        if (dim.unit == DimensionUnit::px && dim.value != 0) {
            YGNodeStyleSetPosition(node, edge, dim.value);
        }
    };
    apply_logical_margin (YGEdgeStart, f.dim_margin_start);
    apply_logical_margin (YGEdgeEnd,   f.dim_margin_end);
    apply_logical_padding(YGEdgeStart, f.dim_padding_start);
    apply_logical_padding(YGEdgeEnd,   f.dim_padding_end);
    apply_logical_position(YGEdgeStart, f.dim_start);
    apply_logical_position(YGEdgeEnd,   f.dim_end);

    // Dimensions dispatch on dim_*.unit so width/height
    // accept percentage values. The bridge's setFlex(width|height, ...)
    // path populates dim_width / dim_height with the unit info; this
    // adapter routes percent values to Yoga's native percent API
    // instead of treating "100%" as 100 px.
    // `width: 'auto'` / `height: 'auto'` route to Yoga's YGNodeStyleSetWidthAuto /
    // SetHeightAuto so the node sizes to its content (Figma "hug
    // contents", v0 intrinsic-sizing cards, Claude Design responsive
    // containers). The explicit Auto API matches user intent and keeps the
    // requested auto sizing explicit in the Yoga tree.
    if (f.dim_width.unit == DimensionUnit::auto_) {
        YGNodeStyleSetWidthAuto(node);
    } else if (f.dim_width.unit == DimensionUnit::percent && f.dim_width.value >= 0) {
        YGNodeStyleSetWidthPercent(node, f.dim_width.value);
    } else if (f.preferred_width > 0) {
        YGNodeStyleSetWidth(node, f.preferred_width);
    }
    if (f.dim_height.unit == DimensionUnit::auto_) {
        YGNodeStyleSetHeightAuto(node);
    } else if (f.dim_height.unit == DimensionUnit::percent && f.dim_height.value >= 0) {
        YGNodeStyleSetHeightPercent(node, f.dim_height.value);
    } else if (f.preferred_height > 0) {
        YGNodeStyleSetHeight(node, f.preferred_height);
    }
    // min/max width/height dispatch on dim_*.unit
    // for the percent path; existing px path stays for numeric values.
    if (f.dim_min_width.unit == DimensionUnit::percent && f.dim_min_width.value >= 0) {
        YGNodeStyleSetMinWidthPercent(node, f.dim_min_width.value);
    } else if (f.min_width > 0) YGNodeStyleSetMinWidth(node, f.min_width);
    if (f.dim_min_height.unit == DimensionUnit::percent && f.dim_min_height.value >= 0) {
        YGNodeStyleSetMinHeightPercent(node, f.dim_min_height.value);
    } else if (f.min_height > 0) YGNodeStyleSetMinHeight(node, f.min_height);
    if (f.dim_max_width.unit == DimensionUnit::percent && f.dim_max_width.value >= 0) {
        YGNodeStyleSetMaxWidthPercent(node, f.dim_max_width.value);
    } else if (f.max_width > 0) YGNodeStyleSetMaxWidth(node, f.max_width);
    if (f.dim_max_height.unit == DimensionUnit::percent && f.dim_max_height.value >= 0) {
        YGNodeStyleSetMaxHeightPercent(node, f.dim_max_height.value);
    } else if (f.max_height > 0) YGNodeStyleSetMaxHeight(node, f.max_height);

    // Aspect ratio: Yoga sizes the cross axis from the main
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

    // Dispatch on per-edge unit so top/right/bottom/left
    // accept percent values. The bridge's setTop / setRight / setBottom /
    // setLeft path populates View::top_unit_ / etc. with the unit info; this
    // adapter routes percent values to Yoga's native percent API instead of
    // treating "50%" as 50 px. Mirrors the FlexStyle::dim_width path for the
    // View positional fields.
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

    // Label-specific width-aware height. When a
    // multi-line Label is laid out in a bounded-width parent, the
    // intrinsic (\n-counted) height is a lower bound that misses
    // soft-wrap line additions. `measured_height(width)` consults the
    // TextShaper to count actual wrapped lines and returns the true
    // painted block height so Yoga reserves enough room for every line.
    // For single-line labels (multi_line_ == false), measured_height()
    // returns the same `intrinsic_height()` value — no behavior change.
    if (auto* label = dynamic_cast<Label*>(view)) {
        float wrapped = label->measured_height(w);
        if (wrapped > h) h = wrapped;
    }

    if (h <= 0) h = height;
    return {w, h};
}

// Baseline callback for text-bearing nodes. Yoga's `align-items: baseline`
// walks each child and asks for its baseline offset; the parent then aligns
// children so their
// baselines sit on a common line. Without this callback, Yoga falls
// back to "baseline == node height", which is the bottom of the box —
// effectively top-align of unequal-height children, which is why bold labels
// and description text in the same flex row would render top-aligned when the
// measure callback only reports width+height. See
// Label::baseline_y() for the ascent computation.
static float yoga_baseline(YGNodeConstRef node, float width, float height) {
    (void) width;
    (void) height;
    auto* view = const_cast<View*>(static_cast<const View*>(YGNodeGetContext(node)));
    if (auto* label = dynamic_cast<Label*>(view)) {
        return label->baseline_y();
    }
    // Non-text nodes: fall through to Yoga's default (height = bottom).
    // For non-text widgets that contain text (Button, Toggle label
    // strip, etc.) the painter's vertical-centering math takes care of
    // alignment internally; they can add a real baseline callback if needed.
    return height;
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

// Wire View's border-width state through to Yoga so the box-sizing math is
// correct. With box-sizing default `border-box`, Yoga subtracts
// (padding + border)
// from the declared dimensions to compute the inner content area. If
// the border slot is 0 in Yoga's view, the content area is too large
// by 2 * border_width and children leak under the painted stroke.
//
// Per-edge resolution: an explicitly set per-side value wins, even if it's 0.
// CSS / RN semantics: a
// shorthand `borderWidth: 10` with `borderTopWidth: 0` must yield a
// 0-px top border. Only when the per-edge `set` flag is false do we
// fall back to the uniform `border_width()`. Yoga's
// `YGNodeStyleSetBorder` takes a px float — Yoga has no percent border
// API (matches the CSS spec, where `border-width: <%>` is invalid).
//
// Negative widths are nonsensical — clamp to 0 (which under the new
// semantics means "no border on this edge", same as an explicit 0).
static void apply_border_widths(YGNodeRef node, const View& view) {
    const bool has_sides   = view.has_border_sides();
    const bool has_uniform = view.has_border();
    if (!has_sides && !has_uniform) return;

    const float uniform = has_uniform ? std::max(0.0f, view.border_width()) : 0.0f;
    auto resolve_edge = [&](bool edge_set, float per_side) -> float {
        if (edge_set) return std::max(0.0f, per_side);   // explicit wins (incl. 0)
        return uniform;                                  // fall back to shorthand
    };
    const float top    = resolve_edge(view.has_border_top_set(),    view.border_top_width());
    const float right  = resolve_edge(view.has_border_right_set(),  view.border_right_width());
    const float bottom = resolve_edge(view.has_border_bottom_set(), view.border_bottom_width());
    const float left   = resolve_edge(view.has_border_left_set(),   view.border_left_width());
    // Always emit; Yoga short-circuits 0-px borders internally and we
    // need 0 to be explicit so it overrides any prior style state.
    YGNodeStyleSetBorder(node, YGEdgeTop,    top);
    YGNodeStyleSetBorder(node, YGEdgeRight,  right);
    YGNodeStyleSetBorder(node, YGEdgeBottom, bottom);
    YGNodeStyleSetBorder(node, YGEdgeLeft,   left);
}

// ── Layout-pass instrumentation ─────────────────────────────────────────────
//
// Measurement hooks for the layout-cost investigation (see
// test/test_yoga_layout_bench.cpp). Cost is one relaxed atomic store per PASS
// (not per node — the node tally is a plain local counter threaded through
// build_yoga_subtree), so this is not measurable against a pass that already
// costs hundreds of microseconds.
//
// `PULP_LAYOUT_STATS=1` additionally prints one line per pass to stderr, which
// is how a real (non-synthetic) UI's managed-node count is measured: run any
// example headlessly with the env var set and read the node count off the
// last-painted frame.
namespace {
std::atomic<uint64_t> g_layout_pass_count{0};
std::atomic<uint32_t> g_layout_last_nodes{0};
std::atomic<uint32_t> g_layout_max_nodes{0};

bool layout_stats_enabled() {
    static const bool on = std::getenv("PULP_LAYOUT_STATS") != nullptr;
    return on;
}
} // namespace

uint64_t yoga_layout_pass_count()      { return g_layout_pass_count.load(std::memory_order_relaxed); }
uint32_t yoga_layout_last_node_count() { return g_layout_last_nodes.load(std::memory_order_relaxed); }
uint32_t yoga_layout_max_node_count()  { return g_layout_max_nodes.load(std::memory_order_relaxed); }
void     yoga_layout_reset_stats() {
    g_layout_pass_count.store(0, std::memory_order_relaxed);
    g_layout_last_nodes.store(0, std::memory_order_relaxed);
    g_layout_max_nodes.store(0, std::memory_order_relaxed);
}

static void build_yoga_subtree(View& view, YGNodeRef node, uint32_t& node_tally,
                               YGConfigRef config, bool& wants_subpixel) {
    ++node_tally;
    if (view.subpixel_layout()) wants_subpixel = true;
    // Position-type wins ordering: tell Yoga "this is absolute" BEFORE
    // any flex-flow attributes are applied, so flex_grow/flex_shrink/
    // flex_basis can be gated on absolute-ness in apply_flex_style and
    // never contribute to the parent's flex line.
    apply_position_style(node, view);

    const bool is_absolute = view.position() == View::Position::absolute
                          || view.position() == View::Position::fixed;
    apply_flex_style(node, view.flex(), is_absolute);
    apply_border_widths(node, view);
    // Wire View::Overflow through to Yoga so the engine knows about clipping
    // context. Yoga's overflow has 3
    // values (visible/hidden/scroll) matching CSS — for layout the only
    // observable difference is in descendant-overflow contribution to
    // the box's preferred size; paint-side clipping is in view.cpp.
    {
        YGOverflow yo = YGOverflowVisible;
        switch (view.overflow()) {
            case View::Overflow::visible: yo = YGOverflowVisible; break;
            case View::Overflow::hidden:  yo = YGOverflowHidden;  break;
            case View::Overflow::scroll:  yo = YGOverflowScroll;  break;
        }
        YGNodeStyleSetOverflow(node, yo);
    }
    // Writing direction propagates into Yoga's YGDirection, which controls how
    // `start`/`end` resolve and whether `flexDirection: row` visually reverses
    // under RTL. Use the raw
    // `direction()` (not `resolved_direction()`, which collapses
    // `auto_` to LTR unconditionally) so that:
    //   - explicit ltr/rtl on a node maps to YGDirectionLTR/RTL,
    //   - `auto_` on a non-root falls back to flex().writing_direction, then
    //     YGDirectionInherit, allowing descendants to pick up an RTL ancestor,
    //   - `auto_` on the root falls back to LTR because there is no parent to
    //     inherit from and no first-strong-character scan here.
    {
        YGDirection ydir;
        switch (view.direction()) {
            case View::WritingDirection::ltr: ydir = YGDirectionLTR; break;
            case View::WritingDirection::rtl: ydir = YGDirectionRTL; break;
            case View::WritingDirection::auto_:
            default:
                // When View::direction is auto_, fall back to the
                // FlexStyle::writing_direction set via the bridge's
                // direction_writing sub-key. If both are unset, inherit from
                // parent (or LTR at root).
                switch (view.flex().writing_direction) {
                    case FlexStyle::WritingDirection::ltr: ydir = YGDirectionLTR; break;
                    case FlexStyle::WritingDirection::rtl: ydir = YGDirectionRTL; break;
                    case FlexStyle::WritingDirection::inherit:
                    default:
                        ydir = view.parent() ? YGDirectionInherit : YGDirectionLTR;
                        break;
                }
                break;
        }
        YGNodeStyleSetDirection(node, ydir);
    }
    YGNodeSetContext(node, &view);

    auto children = ordered_visible_children(view);
    bool has_managed_children = !children.empty() && view.layout_mode() != LayoutMode::grid &&
                                !view.owns_child_layout();

    if (!has_managed_children && (view.intrinsic_width() > 0 || view.intrinsic_height() > 0)) {
        YGNodeSetMeasureFunc(node, yoga_measure);
        // Wire Yoga's baseline channel for text-bearing leaves so flex
        // containers can honor
        // `align-items: baseline`. Today only Labels report a real
        // baseline; non-Label leaves fall through to Yoga's default in
        // yoga_baseline().
        if (dynamic_cast<const Label*>(&view)) {
            YGNodeSetBaselineFunc(node, yoga_baseline);
        }
    }

    if (!has_managed_children)
        return;

    for (size_t i = 0; i < children.size(); ++i) {
        auto* child = children[i];
        YGNodeRef ygChild = YGNodeNewWithConfig(config);
        build_yoga_subtree(*child, ygChild, node_tally, config, wants_subpixel);
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

        if (child->owns_child_layout()) {
            child->layout_children();
            continue;
        }

        apply_yoga_results(*child, childNode);
    }
}

// Build YGNode tree from View tree, compute layout, apply results
void yoga_layout(View& root) {
    auto rootBounds = root.local_bounds();

    const auto stats_t0 = std::chrono::steady_clock::now();
    uint32_t node_tally = 0;

    // One config per pass. Nodes hold a pointer to it, so its
    // pointScaleFactor can be decided AFTER the build walk has seen every
    // view's subpixel_layout() flag — Yoga reads the factor at the
    // round-to-pixel-grid step at the end of YGNodeCalculateLayout.
    YGConfigRef ygConfig = YGConfigNew();
    bool wants_subpixel = root.subpixel_layout();

    YGNodeRef ygRoot = YGNodeNewWithConfig(ygConfig);
    YGNodeStyleSetWidth(ygRoot, rootBounds.width);
    YGNodeStyleSetHeight(ygRoot, rootBounds.height);
    build_yoga_subtree(root, ygRoot, node_tally, ygConfig, wants_subpixel);
    // 0 disables the pixel-grid pass entirely, preserving the fractional
    // solved geometry (see View::set_subpixel_layout for why imported
    // designs need this). Default 1.0f keeps Yoga's stock whole-pixel
    // rounding for everything else.
    if (wants_subpixel) YGConfigSetPointScaleFactor(ygConfig, 0.0f);

    // Root direction follows the View's own writing_direction. When `inherit`
    // (the default), Yoga falls back to LTR at the root, matching CSS / RN
    // fallback behavior when content provides no directional signal.
    // Setting `rtl` on the root flips the
    // row-axis layout and resolves YGEdgeStart to the right edge for
    // every descendant that also inherits.
    YGDirection rootDir = YGDirectionLTR;
    switch (root.flex().writing_direction) {
        case FlexStyle::WritingDirection::rtl:    rootDir = YGDirectionRTL; break;
        case FlexStyle::WritingDirection::ltr:    rootDir = YGDirectionLTR; break;
        case FlexStyle::WritingDirection::inherit:
        default:                                   rootDir = YGDirectionLTR; break;
    }
    YGNodeCalculateLayout(ygRoot, rootBounds.width, rootBounds.height, rootDir);
    apply_yoga_results(root, ygRoot);

    if (std::getenv("PULP_DUMP_BOUNDS")) {
        std::fprintf(stderr, "\n=== [PULP_DUMP_BOUNDS] root @ %.0fx%.0f ===\n",
                     rootBounds.width, rootBounds.height);
        dump_bounds_tree(root, 0);
        std::fprintf(stderr, "=== [PULP_DUMP_BOUNDS] end ===\n\n");
    }

    YGNodeFreeRecursive(ygRoot);
    YGConfigFree(ygConfig);

    const uint64_t pass = g_layout_pass_count.fetch_add(1, std::memory_order_relaxed) + 1;
    g_layout_last_nodes.store(node_tally, std::memory_order_relaxed);
    if (node_tally > g_layout_max_nodes.load(std::memory_order_relaxed))
        g_layout_max_nodes.store(node_tally, std::memory_order_relaxed);
    if (layout_stats_enabled()) {
        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - stats_t0).count();
        std::fprintf(stderr, "[layout-stats] pass=%llu nodes=%u root=%.0fx%.0f us=%.1f\n",
                     static_cast<unsigned long long>(pass), node_tally,
                     rootBounds.width, rootBounds.height, us);
    }
}

} // namespace pulp::view

#endif // PULP_HAS_YOGA
