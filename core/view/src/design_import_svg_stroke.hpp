// SPDX-License-Identifier: MIT
#pragma once

// The SVG stroke style a lowered vector carries (`svg_stroke_linecap`,
// `svg_stroke_linejoin`, `svg_stroke_miterlimit`), parsed once so the native
// lane and the baked C++ lane stroke a path identically.

#include "design_ir_helpers.hpp"

#include <pulp/canvas/path.hpp>
#include <pulp/view/design_ir.hpp>

#include <cmath>
#include <cstdlib>
#include <optional>
#include <ostream>
#include <string>

namespace pulp::view {

struct SvgStrokeStyleAttributes {
    std::optional<canvas::LineCap> cap;
    std::optional<canvas::LineJoin> join;
    std::optional<float> miter_limit;
};

inline SvgStrokeStyleAttributes svg_stroke_style_attributes(const IRNode& node) {
    SvgStrokeStyleAttributes out;
    if (const auto cap = attr(node, "svg_stroke_linecap")) {
        if (*cap == "butt")
            out.cap = canvas::LineCap::butt;
        else if (*cap == "round")
            out.cap = canvas::LineCap::round;
        else if (*cap == "square")
            out.cap = canvas::LineCap::square;
    }
    if (const auto join = attr(node, "svg_stroke_linejoin")) {
        // `miter-clip` and `arcs` are SVG 2 joins no renderer here draws; a
        // browser without them falls back to `miter`, and so does this.
        if (*join == "miter" || *join == "miter-clip" || *join == "arcs")
            out.join = canvas::LineJoin::miter;
        else if (*join == "round")
            out.join = canvas::LineJoin::round;
        else if (*join == "bevel")
            out.join = canvas::LineJoin::bevel;
    }
    if (const auto limit = attr(node, "svg_stroke_miterlimit")) {
        char* end = nullptr;
        const float value = std::strtof(limit->c_str(), &end);
        if (end == limit->c_str() + limit->size() && std::isfinite(value) && value >= 1.0f)
            out.miter_limit = value;
    }
    return out;
}

/// The JS lane's form of the same style: one `setSvgStrokeStyle` call, emitted
/// only when the node carries any of the three.
inline void emit_svg_stroke_style_js(std::ostream& out, const std::string& indent,
                                     const std::string& id, const IRNode& node) {
    const auto style = svg_stroke_style_attributes(node);
    if (!style.cap && !style.join && !style.miter_limit)
        return;
    const auto cap = !style.cap                              ? ""
                     : *style.cap == canvas::LineCap::round  ? "round"
                     : *style.cap == canvas::LineCap::square ? "square"
                                                             : "butt";
    const auto join = !style.join                              ? ""
                      : *style.join == canvas::LineJoin::round ? "round"
                      : *style.join == canvas::LineJoin::bevel ? "bevel"
                                                               : "miter";
    out << indent << "setSvgStrokeStyle('" << id << "', '" << cap << "', '" << join << "'";
    if (style.miter_limit)
        out << ", " << *style.miter_limit;
    out << ");\n";
}

} // namespace pulp::view
