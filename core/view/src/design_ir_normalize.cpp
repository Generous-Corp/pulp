// design_ir_normalize.cpp — heuristic geometry rewrites on a parsed IRNode.
//
// normalize_design_ir() is the post-parse structural normalization pass:
// named rules that rewrite node geometry to close the gap between what a
// design tool stores and what Pulp's renderer paints. parse_ir_node
// (design_ir_json.cpp) calls it once per node, after that node's children
// and alternate frames are parsed, so every rule sees final child geometry.
// The rules are deliberately separate from field deserialization: they read
// and write only the IR tree, never the source JSON, so they can be unit
// tested in isolation (test_design_ir_normalize.cpp).

#include <pulp/view/design_import.hpp>

#include "design_import_internal.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pulp::view {

// Shadow-driven sibling snap. When a frame has a downward drop shadow and an
// absolutely-positioned sibling sits just below it with a small gap, the gap
// exposes the grandparent's canvas color through the shadow zone — a thin
// lighter band between the panel and whatever sits below. Figma's designer
// places these tightly because Figma's shadow extends visually ONTO the
// sibling below (shadow is drawn on top); the intent is visual continuity.
// Pulp paints the lower sibling ABOVE the shadow (later in z-order) so closing
// the geometric gap gives us the same continuity. Rule: for each absolute
// child F with a downward drop shadow, look at the next absolute sibling S
// beneath it; if 0 < gap < (oy + blur/2), snap S up — but leave the shadow's
// y-offset worth of room so Pulp's same-z-layer shadow still has somewhere to
// render (otherwise S overpaints it).
// The snap reads parsed IRBoxShadow layers instead of re-parsing the raw CSS
// string each time.
static void snap_absolute_siblings_under_shadow(IRNode& node) {
    // First non-inset downward drop-shadow layer of a node, if any.
    auto down_shadow = [](const IRStyle& st) -> const IRBoxShadow* {
        for (const auto& sh : st.box_shadow)
            if (!sh.inset && sh.offset_y > 0.0f) return &sh;
        return nullptr;
    };
    struct SibRect { size_t idx; float top, bottom; bool has_down_shadow; float shadow_reach; };
    std::vector<SibRect> abs_siblings;
    for (size_t i = 0; i < node.children.size(); ++i) {
        auto& c = node.children[i];
        bool is_abs = c.style.position && *c.style.position == "absolute";
        if (!is_abs) continue;
        float top = c.style.top.value_or(0.0f);
        float h   = c.style.height.value_or(0.0f);
        if (h <= 0.0f) continue;
        const IRBoxShadow* sh = down_shadow(c.style);
        float oy = sh ? sh->offset_y : 0.0f;
        float blur = sh ? sh->blur : 0.0f;
        abs_siblings.push_back({i, top, top + h, sh != nullptr, oy + blur * 0.5f});
    }
    std::sort(abs_siblings.begin(), abs_siblings.end(),
              [](const SibRect& a, const SibRect& b){ return a.top < b.top; });
    for (size_t k = 0; k + 1 < abs_siblings.size(); ++k) {
        const auto& F = abs_siblings[k];
        auto& S       = abs_siblings[k + 1];
        if (!F.has_down_shadow) continue;
        float gap = S.top - F.bottom;
        if (gap <= 0.0f) continue;
        if (gap >= F.shadow_reach) continue;
        const IRBoxShadow* fsh = down_shadow(node.children[F.idx].style);
        float preserve = std::max(0.0f, fsh ? fsh->offset_y : 0.0f);
        float close = std::max(0.0f, gap - preserve);
        if (close <= 0.0f) continue;
        float new_top = S.top - close;
        node.children[S.idx].style.top = new_top;
        S.bottom -= (S.top - new_top);
        S.top = new_top;
    }
}

void normalize_design_ir(IRNode& node) {
    // ── Separator promotion ─────────────────────────────────────────────
    // Figma stores 1-pixel vertical lines (column separators, etc.) as a
    // VECTOR node with effectively-zero width or height plus a 1px stroke.
    // The figma-plugin extractor captures the bounding-box dims faithfully,
    // which means we end up with width ≈ 5e-06 — invisible in any renderer.
    // Promote the stroke weight to the visible dimension and turn the
    // node into a colored rect (drop the empty PNG fill) so it actually
    // shows up. Trigger: width < 0.5 or height < 0.5 AND border_width >= 0.5.
    {
        constexpr float kDegenerateAxis = 0.5f;
        constexpr float kMinStrokeWeight = 0.5f;  // Figma "1px" strokes
                                                   // often come through at
                                                   // 0.97 due to fractional
                                                   // raster alignment.
        float bw = node.style.border_width.value_or(0.0f);
        // Only fire when BOTH dimensions are explicitly set AND at least
        // one is degenerate. A nullopt (auto-sized) dim must NOT be
        // treated as 0 — that would misfire on round-trip parses of
        // legitimately auto-sized stroked frames.
        bool has_w = node.style.width.has_value();
        bool has_h = node.style.height.has_value();
        float w = node.style.width.value_or(0.0f);
        float h = node.style.height.value_or(0.0f);
        bool degenerate_w = has_w && w < kDegenerateAxis;
        bool degenerate_h = has_h && h < kDegenerateAxis;
        if (bw >= kMinStrokeWeight && has_w && has_h && (degenerate_w || degenerate_h)) {
            if (degenerate_w) node.style.width  = std::max(bw, 1.0f);
            if (degenerate_h) node.style.height = std::max(bw, 1.0f);
            // Use the stroke color as the rect fill; the captured PNG is
            // a zero-area image and would render nothing anyway.
            if (!node.style.background_color && node.style.border_color)
                node.style.background_color = node.style.border_color;
            // The fill now IS the hairline. Drop the stroke so codegen does
            // not ALSO emit a border — a 1.5px line + a 1.5px border draws on
            // both edges and renders ~3× too wide (e.g. a knob pointer line).
            // The width was already set to the stroke weight above, so the
            // filled rect alone reproduces the line at its true thickness.
            node.style.border.reset();
            node.style.border_color.reset();
            node.style.border_width.reset();
            node.style.border_style.reset();
            // Strip the asset_ref so codegen's image branch doesn't try to
            // emit setImageSource on a degenerate PNG.
            node.attributes.erase("asset_ref");
            // Demote from "image" to "frame" so codegen emits a styled
            // container instead of an <img>-style image element.
            if (node.type == "image") node.type = "frame";
            // Tag it as a decorative stroke so a parent widget's recognition
            // gate treats it as ornamentation, not as a disqualifying nested
            // container (a knob keeps its pointer hairline AND its widget-ness).
            node.attributes["__stroke_demoted"] = "1";
        }
    }

    // ── Inherit rounded corners from rounded parent ─────────────────────
    // Figma stores a corner radius on the CONTAINER frame and relies on
    // overflow:clip to round the children that fill it. Pulp's renderer
    // doesn't clip children to a parent's border-radius, so a gradient
    // rect that exactly fills a rounded parent ends up with hard corners.
    // Propagate the parent's radius to any child that has position:abs
    // at (0,0) and matches the parent's size (or any axis matches and
    // the other is close), so the gradient/fill child also paints with
    // rounded corners.
    if (node.style.border_radius && *node.style.border_radius > 0.0f) {
        float pr = *node.style.border_radius;
        float pw = node.style.width.value_or(0.0f);
        float ph = node.style.height.value_or(0.0f);
        for (auto& c : node.children) {
            if (c.style.border_radius && *c.style.border_radius > 0.0f)
                continue;  // child already has its own radius — respect it
            float cl = c.style.left.value_or(-1.0f);
            float ct = c.style.top.value_or(-1.0f);
            float cw = c.style.width.value_or(0.0f);
            float ch = c.style.height.value_or(0.0f);
            constexpr float kFillTol = 1.0f;
            bool fills_origin = (std::abs(cl) < kFillTol) && (std::abs(ct) < kFillTol);
            bool fills_size = (pw > 0.0f && std::abs(cw - pw) < kFillTol) &&
                              (ph > 0.0f && std::abs(ch - ph) < kFillTol);
            if (fills_origin && fills_size) {
                c.style.border_radius = pr;
            }
        }
    }

    // Shadow-driven sibling snap.
    snap_absolute_siblings_under_shadow(node);

    // ── Connector-line spanning rule ────────────────────────────────────
    // Pattern: a flex row whose FIRST child is a horizontal hairline (height
    // ≤ a couple of px, width > 1) and whose SUBSEQUENT children are
    // boxes/widgets/buttons that visually sit ON TOP of the line. Figma
    // designers use this to communicate a connected pipeline — the line
    // threads BEHIND the items so the visible bits are the gaps between
    // boxes. Without a fix, our flex layout puts the line in the
    // first-item slot (compressed to its 106-ish px width on the left),
    // breaking the connection visual. Convert the line to absolute,
    // span the full row width, and center it vertically. Because it
    // stays first in z-order, subsequent children draw on top — the
    // visible segments emerge as gaps. Generalises a connector-rail
    // pattern (e.g. an FX-rack row: a hairline + chained dropdowns + "+")
    // to any flex row with a connector hairline + ≥ 2 sibling widgets.
    if (node.layout.direction == LayoutDirection::row && node.children.size() >= 3) {
        auto& first = node.children.front();
        float fw = first.style.width.value_or(0.0f);
        float fh = first.style.height.value_or(0.0f);
        size_t non_line_followers = 0;
        std::vector<float> follower_widths;
        float max_follower_h = 0.0f;
        for (size_t i = 1; i < node.children.size(); ++i) {
            const auto& c = node.children[i];
            float cw = c.style.width.value_or(0.0f);
            float ch = c.style.height.value_or(0.0f);
            if (cw >= 8.0f && ch >= 8.0f) {
                ++non_line_followers;
                follower_widths.push_back(cw);
                max_follower_h = std::max(max_follower_h, ch);
            }
        }
        float row_w = node.style.width.value_or(0.0f);
        float row_h = node.style.height.value_or(0.0f);
        // Trigger conditions for the connector promotion. Each gate exists
        // to NOT misfire on legitimate "thin first child" patterns:
        //   - is_horizontal_hairline: 0 < height ≤ 2px, width > 4px
        //   - fits_below_half_row: a real CONNECTOR has the line drawn
        //     much shorter than the row width (Figma stores the
        //     SEGMENT length, not the spanning extent). A flex row
        //     whose first child has width >= 50% of the row is almost
        //     certainly a content element (progress bar, slider track,
        //     divider) that should participate in flex sizing.
        //   - row_much_taller: a 2px first child inside a 4-6px tall
        //     row is geometry; a 2px first child inside a ≥ 6× tall
        //     row is a connector because there's vertical headroom
        //     for the dropdowns/buttons to sit ON TOP of it.
        bool is_horizontal_hairline = (fh > 0.0f && fh <= 2.0f && fw > 4.0f);
        bool fits_below_half_row = (row_w > 0.0f && fw <= row_w * 0.5f);
        bool row_much_taller = (max_follower_h > 0.0f && row_h >= max_follower_h * 1.5f)
                             || (row_h >= fh * 6.0f);
        if (is_horizontal_hairline && fits_below_half_row && row_much_taller &&
            non_line_followers >= 2 && row_w > 0.0f && row_h > 0.0f) {
            // Compute the span. Default: full row width. Refinement: if the
            // LAST follower is significantly smaller than the others (≤ 60%
            // of the median width), it's most likely a trailing "add" /
            // "more" affordance ("+", "settings cog", etc.) — NOT part of
            // the connected pipeline. Pull the line's right edge back to
            // just-past the penultimate widget so the connection visual
            // ends at the last real item.
            float span_w = row_w;
            if (follower_widths.size() >= 3) {
                std::vector<float> sorted = follower_widths;
                std::sort(sorted.begin(), sorted.end());
                float median = sorted[sorted.size() / 2];
                float last = follower_widths.back();
                if (median > 0.0f && (last / median) < 0.6f) {
                    // Trailing widget is most likely an "add" / "more"
                    // affordance, not part of the connected pipeline.
                    // Pull the line's right edge back by that widget's
                    // width + the gap before it, so the connection
                    // visual ends at the last real item.
                    float gap = node.layout.gap;
                    span_w = std::max(median, row_w - last - gap);
                }
            }
            first.style.position = "absolute";
            first.style.left = 0.0f;
            first.style.top  = (row_h - fh) * 0.5f;
            first.style.width  = span_w;
            first.style.height = fh;             // keep stroke weight
        }
    }
}

}  // namespace pulp::view
