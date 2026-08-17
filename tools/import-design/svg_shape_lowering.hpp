// SPDX-License-Identifier: MIT
#pragma once

#include "browser_capture_styles.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pulp::import_design {

/// Every node's parent, inverted once: node index → its children in document
/// order. Walking an `<svg>` subtree needs the elements that never painted
/// (a `<defs>`, a `<g>` wrapper), and the snapshot only stores the upward
/// edge.
std::vector<std::vector<int>> build_child_index(const CapturedStyleIndex& index);

/// Why an `<svg>` subtree keeps arriving as a captured element rather than as
/// drawn geometry. Named rather than boolean because the fixes differ: an
/// unsupported element is a shape vocabulary gap, a paint reference is a
/// `<defs>` gradient, and a transform is arithmetic the path data would have to
/// be baked through.
enum class SvgRefusal {
    none,
    /// An element outside the drawable shape vocabulary — `<text>`, `<image>`,
    /// `<use>`, `<filter>`, a nested `<svg>`, `<foreignObject>`.
    element,
    /// A `transform` on the root, a group, or a shape. The lowered nodes carry
    /// path data in the root's user space with no per-node matrix, so a
    /// transform would silently draw the shape in the wrong place.
    transform,
    /// A `fill`/`stroke` of `url(#…)` — a gradient, pattern, or paint server
    /// defined in `<defs>`. The lowered node carries a colour, not a reference.
    paint_reference,
    /// A `stroke-dasharray` other than `none`. Drawn solid it is a wrong
    /// picture, which is worse than an honest capture.
    dashed_stroke,
    /// A group whose `opacity` is not 1. Group opacity composites the group as
    /// a unit; per-shape alpha double-darkens every overlap.
    group_opacity,
    /// A shape attribute the synthesis cannot read — a percentage length, a
    /// missing `d`, a malformed `points` list.
    shape_geometry,
    /// The subtree paints nothing at all. Not a failure to draw; there is
    /// simply no ink, so there is nothing to gain by capturing it either.
    empty,
    /// The capture never collected `fill` / `stroke`, so no colour for the
    /// geometry exists anywhere in it. Distinct from every other refusal
    /// because nothing about the design is at fault: the capture predates the
    /// protocol that records SVG paint, and re-capturing fixes it.
    paint_unavailable,
};

std::string_view to_string(SvgRefusal refusal);

/// One drawable shape, already in the root `<svg>`'s user-coordinate space.
struct SvgShape {
    int node_index = -1;
    std::string tag;
    /// SVG path data. Synthesized for the primitives (`rect`, `circle`,
    /// `ellipse`, `line`, `polygon`, `polyline`) so every shape reaches the
    /// renderer through one path.
    std::string path_data;
    /// A CSS colour, or nullopt when the shape does not fill / stroke.
    /// `fill-opacity` / `stroke-opacity` are already folded into the alpha, so
    /// a consumer never has to know they existed.
    std::optional<std::string> fill;
    std::optional<std::string> stroke;
    /// In the root's user units, matching `SvgPathWidget::set_stroke_width`.
    double stroke_width = 1.0;
    bool even_odd_fill = false;
};

/// What an `<svg>` subtree lowers to.
struct SvgSubtree {
    SvgRefusal refusal = SvgRefusal::none;
    /// The element name or value that refused, so a report says *which*
    /// `<use>` rather than "some element".
    std::string refusal_detail;
    /// The user-coordinate space every shape's path data is expressed in.
    /// Zero when the root declares no `viewBox`, which means "the shapes are
    /// already in CSS pixels" — the renderer's own 1:1 fallback.
    double viewbox_width = 0.0;
    double viewbox_height = 0.0;
    /// `preserveAspectRatio="none"`: map the viewBox onto the element's box on
    /// each axis independently, with no uniform scale and no centering.
    ///
    /// False is the SVG default (`xMidYMid meet`). The distinction is not
    /// cosmetic for a wide, short drawing: a waveform authored in a 380x108
    /// viewBox and stretched across its box is the shape the author drew, and
    /// fitting it uniformly instead letterboxes it into a band with the trace
    /// squeezed away from the box it is supposed to span.
    bool stretch_to_box = false;
    /// Document order, which for a well-formed SVG subtree is paint order.
    std::vector<SvgShape> shapes;

    bool drawable() const { return refusal == SvgRefusal::none; }
};

/// Read one `<svg>` element and everything under it out of the capture.
///
/// Geometry comes from the AUTHORED attributes (`d`, `points`, `cx`) because
/// that is where it lives; paint comes from Chrome's COMPUTED style, because
/// an icon's colour is set three different ways — presentation attribute,
/// stylesheet rule, or `currentColor` inherited from the box around it — and
/// only the browser knows which one won.
SvgSubtree lower_svg_subtree(const CapturedStyleIndex& index,
                             const std::vector<std::vector<int>>& children,
                             int svg_node_index);

}  // namespace pulp::import_design
