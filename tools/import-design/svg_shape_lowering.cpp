// SPDX-License-Identifier: MIT
#include "svg_shape_lowering.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::import_design {
namespace {

constexpr int kElementNode = 1;
constexpr int kTextNode = 3;

/// Elements that carry no ink of their own, subtree included.
///
/// `<title>`/`<desc>` are accessibility text the renderer never paints;
/// `<metadata>` is inert; `<defs>` holds definitions that paint only where
/// something REFERENCES them. Walking into a `<defs>` would refuse on the
/// `<linearGradient>` inside it and report the definition as the problem — the
/// reference is, and it is caught where it is used.
bool is_ignorable_subtree(std::string_view tag) {
    return tag == "title" || tag == "desc" || tag == "metadata" ||
           tag == "defs";
}

/// Grouping elements: no ink of their own, but their attributes are inherited
/// by the shapes below — which Chrome has already resolved for us, so a group
/// is transparent here as long as it does not composite (`opacity`) or move
/// (`transform`) its children as a unit.
bool is_group_element(std::string_view tag) {
    return tag == "g" || tag == "a" || tag == "switch";
}

bool is_shape_element(std::string_view tag) {
    return tag == "path" || tag == "rect" || tag == "circle" ||
           tag == "ellipse" || tag == "line" || tag == "polygon" ||
           tag == "polyline";
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n\f\v");
    return std::string(value.substr(first, last - first + 1));
}

/// A CSS/SVG length as a user-unit number. Only unitless values and `px` are
/// accepted: every other unit (`%`, `em`, `mm`) resolves against a context the
/// shape synthesis does not have, and guessing would draw the shape at the
/// wrong size rather than refusing.
std::optional<double> parse_length(std::string_view raw) {
    const std::string text = trim(raw);
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return std::nullopt;
    const std::string suffix = trim(std::string_view(end));
    if (!suffix.empty() && suffix != "px") return std::nullopt;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

/// Shortest round-trippable text for a coordinate. Path data is written once
/// and parsed once, so the only thing that matters is that the number that
/// comes back out is the one that went in.
std::string number(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

/// One elliptical-arc segment in the `A` command's argument order.
std::string arc_to(double rx, double ry, int sweep, double x, double y) {
    return " A" + number(rx) + " " + number(ry) + " 0 0 " +
           std::to_string(sweep) + " " + number(x) + " " + number(y);
}

/// `<rect>` → a path, with the corner-radius rules SVG specifies: a missing
/// `rx` takes `ry` (and vice versa), and both are clamped to half the side.
std::optional<std::string> rect_path(const CapturedStyleIndex& index,
                                     int node_index) {
    const auto x = parse_length(index.attribute(node_index, "x")).value_or(0.0);
    const auto y = parse_length(index.attribute(node_index, "y")).value_or(0.0);
    const auto width = parse_length(index.attribute(node_index, "width"));
    const auto height = parse_length(index.attribute(node_index, "height"));
    if (!width || !height) return std::nullopt;
    if (*width <= 0.0 || *height <= 0.0) return std::string{};

    auto rx = parse_length(index.attribute(node_index, "rx"));
    auto ry = parse_length(index.attribute(node_index, "ry"));
    if (!rx && ry) rx = ry;
    if (!ry && rx) ry = rx;
    double corner_x = std::clamp(rx.value_or(0.0), 0.0, *width / 2.0);
    double corner_y = std::clamp(ry.value_or(0.0), 0.0, *height / 2.0);
    if (corner_x <= 0.0 || corner_y <= 0.0) {
        return "M" + number(x) + " " + number(y) + " H" + number(x + *width) +
               " V" + number(y + *height) + " H" + number(x) + " Z";
    }
    const double right = x + *width;
    const double bottom = y + *height;
    std::string d = "M" + number(x + corner_x) + " " + number(y);
    d += " H" + number(right - corner_x);
    d += arc_to(corner_x, corner_y, 1, right, y + corner_y);
    d += " V" + number(bottom - corner_y);
    d += arc_to(corner_x, corner_y, 1, right - corner_x, bottom);
    d += " H" + number(x + corner_x);
    d += arc_to(corner_x, corner_y, 1, x, bottom - corner_y);
    d += " V" + number(y + corner_y);
    d += arc_to(corner_x, corner_y, 1, x + corner_x, y);
    d += " Z";
    return d;
}

/// `<circle>` / `<ellipse>` → two half-arcs. One full-turn arc is degenerate
/// (start and end coincide, so no ellipse is determined) and renders as
/// nothing; splitting at the opposite side is the standard construction.
std::optional<std::string> ellipse_path(const CapturedStyleIndex& index,
                                        int node_index, bool circle) {
    const auto cx =
        parse_length(index.attribute(node_index, "cx")).value_or(0.0);
    const auto cy =
        parse_length(index.attribute(node_index, "cy")).value_or(0.0);
    double rx = 0.0;
    double ry = 0.0;
    if (circle) {
        const auto r = parse_length(index.attribute(node_index, "r"));
        if (!r) return std::nullopt;
        rx = ry = *r;
    } else {
        const auto x_radius = parse_length(index.attribute(node_index, "rx"));
        const auto y_radius = parse_length(index.attribute(node_index, "ry"));
        if (!x_radius || !y_radius) return std::nullopt;
        rx = *x_radius;
        ry = *y_radius;
    }
    if (rx <= 0.0 || ry <= 0.0) return std::string{};
    std::string d = "M" + number(cx - rx) + " " + number(cy);
    d += arc_to(rx, ry, 1, cx + rx, cy);
    d += arc_to(rx, ry, 1, cx - rx, cy);
    d += " Z";
    return d;
}

std::optional<std::string> line_path(const CapturedStyleIndex& index,
                                     int node_index) {
    const auto x1 = parse_length(index.attribute(node_index, "x1")).value_or(0.0);
    const auto y1 = parse_length(index.attribute(node_index, "y1")).value_or(0.0);
    const auto x2 = parse_length(index.attribute(node_index, "x2")).value_or(0.0);
    const auto y2 = parse_length(index.attribute(node_index, "y2")).value_or(0.0);
    return "M" + number(x1) + " " + number(y1) + " L" + number(x2) + " " +
           number(y2);
}

/// `points` is a flat coordinate list separated by whitespace and/or commas.
/// An odd count is malformed — SVG says to drop the trailing value, but a
/// truncated polygon is a wrong picture, so it refuses instead.
std::optional<std::string> points_path(const CapturedStyleIndex& index,
                                       int node_index, bool close) {
    const std::string raw = index.attribute(node_index, "points");
    std::vector<double> values;
    const char* cursor = raw.c_str();
    while (*cursor != '\0') {
        while (*cursor != '\0' &&
               (std::isspace(static_cast<unsigned char>(*cursor)) != 0 ||
                *cursor == ','))
            ++cursor;
        if (*cursor == '\0') break;
        char* end = nullptr;
        const double value = std::strtod(cursor, &end);
        if (end == cursor || !std::isfinite(value)) return std::nullopt;
        values.push_back(value);
        cursor = end;
    }
    if (values.size() < 4 || values.size() % 2 != 0) return std::nullopt;
    std::string d = "M" + number(values[0]) + " " + number(values[1]);
    for (std::size_t i = 2; i + 1 < values.size(); i += 2)
        d += " L" + number(values[i]) + " " + number(values[i + 1]);
    if (close) d += " Z";
    return d;
}

/// Fold `fill-opacity` / `stroke-opacity` into the colour's own alpha, so a
/// consumer that only understands a CSS colour still gets the right pixel.
/// Chrome serializes an SVG paint as `rgb(…)` / `rgba(…)` / `none`.
std::optional<std::string> paint_colour(const std::string& colour,
                                        const std::string& opacity) {
    const std::string value = trim(colour);
    if (value.empty() || value == "none" || value == "transparent")
        return std::nullopt;
    double alpha = 1.0;
    if (const auto parsed = parse_length(opacity)) alpha = *parsed;
    alpha = std::clamp(alpha, 0.0, 1.0);
    if (alpha >= 1.0) return value;
    if (alpha <= 0.0) return std::nullopt;
    // `rgb(r, g, b)` → `rgba(r, g, b, a)`; anything already carrying an alpha
    // (`rgba(…)`) is left alone rather than multiplied blind, because the
    // arithmetic would have to re-serialize a colour we did not parse.
    if (value.rfind("rgb(", 0) == 0 && value.back() == ')') {
        return "rgba(" + value.substr(4, value.size() - 5) + ", " +
               number(alpha) + ")";
    }
    return value;
}

/// `viewBox` is "min-x min-y width height". Only the size is read: the
/// renderer scales path coordinates from that box into the node's bounds, and
/// a non-zero origin is carried by the path coordinates themselves.
void parse_viewbox(std::string_view raw, SvgSubtree& out) {
    double values[4] = {0.0, 0.0, 0.0, 0.0};
    const std::string text(raw);
    const char* cursor = text.c_str();
    int got = 0;
    while (got < 4) {
        while (*cursor != '\0' &&
               (std::isspace(static_cast<unsigned char>(*cursor)) != 0 ||
                *cursor == ','))
            ++cursor;
        char* end = nullptr;
        const double value = std::strtod(cursor, &end);
        if (end == cursor) break;
        values[got++] = value;
        cursor = end;
    }
    if (got != 4 || values[2] <= 0.0 || values[3] <= 0.0) return;
    out.viewbox_width = values[2];
    out.viewbox_height = values[3];
}

/// A transform on the element, from EITHER source. Chrome's computed value is
/// the authority when the element produced a layout object, but an SVG
/// container that produced none has no style row at all — and reading only the
/// computed value there would report "no transform" for a `<g transform=…>`
/// and draw its whole subtree in the wrong place.
bool has_transform(const CapturedStyleIndex& index, int node_index,
                   const std::map<std::string, std::string>& computed) {
    const auto found = computed.find("transform");
    if (found != computed.end()) {
        const std::string value = trim(found->second);
        if (!value.empty() && value != "none") return true;
    }
    const std::string authored = trim(index.attribute(node_index, "transform"));
    return !authored.empty() && authored != "none";
}

void refuse(SvgSubtree& out, SvgRefusal reason, std::string detail) {
    if (out.refusal != SvgRefusal::none) return;
    out.refusal = reason;
    out.refusal_detail = std::move(detail);
}

}  // namespace

std::string_view to_string(SvgRefusal refusal) {
    switch (refusal) {
        case SvgRefusal::none: return "none";
        case SvgRefusal::element: return "element";
        case SvgRefusal::transform: return "transform";
        case SvgRefusal::paint_reference: return "paint-reference";
        case SvgRefusal::dashed_stroke: return "dashed-stroke";
        case SvgRefusal::group_opacity: return "group-opacity";
        case SvgRefusal::shape_geometry: return "shape-geometry";
        case SvgRefusal::empty: return "empty";
        case SvgRefusal::paint_unavailable: return "paint-unavailable";
    }
    return "none";
}

std::vector<std::vector<int>> build_child_index(
    const CapturedStyleIndex& index) {
    const int count = index.node_count();
    std::vector<std::vector<int>> children(static_cast<std::size_t>(
        std::max(count, 0)));
    for (int node = 0; node < count; ++node) {
        const int parent = index.parent_of(node);
        if (parent < 0 || parent >= count || parent == node) continue;
        children[static_cast<std::size_t>(parent)].push_back(node);
    }
    return children;
}

SvgSubtree lower_svg_subtree(const CapturedStyleIndex& index,
                             const std::vector<std::vector<int>>& children,
                             int svg_node_index) {
    SvgSubtree out;
    parse_viewbox(index.attribute(svg_node_index, "viewBox"), out);

    // Without the resolved paint there is no colour to draw the geometry in,
    // and defaulting it would paint a black icon over a dark panel — a wrong
    // picture that reads as a rendering bug rather than a stale capture.
    if (!index.has_property("fill") || !index.has_property("stroke")) {
        refuse(out, SvgRefusal::paint_unavailable, "fill");
        return out;
    }

    // The root's own transform moves the whole icon; a shape's moves one piece.
    // Both are unrepresentable in a lowering that carries no per-node matrix.
    if (has_transform(index, svg_node_index,
                      index.styles_for_node(svg_node_index)))
        refuse(out, SvgRefusal::transform, "svg");

    // Explicit stack rather than recursion: an SVG subtree is authored data and
    // a `parentIndex` cycle in a malformed snapshot would otherwise run the
    // stack out. `visited` bounds it to the nodes that exist.
    std::vector<int> stack;
    std::vector<char> visited(children.size(), 0);
    const auto push_children = [&](int node) {
        const auto& kids = children[static_cast<std::size_t>(node)];
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back(*it);
    };
    push_children(svg_node_index);

    while (!stack.empty() && out.refusal == SvgRefusal::none) {
        const int node = stack.back();
        stack.pop_back();
        if (node < 0 || node >= static_cast<int>(children.size())) continue;
        if (visited[static_cast<std::size_t>(node)] != 0) continue;
        visited[static_cast<std::size_t>(node)] = 1;

        const int node_type = index.node_type_of(node);
        if (node_type == kTextNode) {
            // Whitespace between elements is the authored indentation. Text
            // with content belongs to a `<text>` element, which is refused
            // above as an unsupported element before its child is reached.
            continue;
        }
        if (node_type != kElementNode) continue;

        const std::string tag = index.tag_name(node);
        if (is_ignorable_subtree(tag)) continue;

        const auto computed = index.styles_for_node(node);
        if (has_transform(index, node, computed)) {
            refuse(out, SvgRefusal::transform, tag);
            break;
        }

        if (is_group_element(tag)) {
            const auto opacity = computed.find("opacity");
            if (opacity != computed.end()) {
                const auto value = parse_length(opacity->second);
                if (value && *value < 1.0) {
                    refuse(out, SvgRefusal::group_opacity, tag);
                    break;
                }
            }
            push_children(node);
            continue;
        }

        if (!is_shape_element(tag)) {
            refuse(out, SvgRefusal::element, tag);
            break;
        }

        const auto style = [&](const char* name) -> std::string {
            const auto found = computed.find(name);
            return found == computed.end() ? std::string{} : found->second;
        };

        const std::string fill_value = trim(style("fill"));
        const std::string stroke_value = trim(style("stroke"));
        // A paint server (`url(#grad)`) is a reference into `<defs>`; the
        // lowered node carries a colour, so honouring it would need the
        // gradient lowered too.
        if (fill_value.rfind("url(", 0) == 0 ||
            stroke_value.rfind("url(", 0) == 0) {
            refuse(out, SvgRefusal::paint_reference, tag);
            break;
        }
        const std::string dash = trim(style("stroke-dasharray"));
        if (!dash.empty() && dash != "none") {
            refuse(out, SvgRefusal::dashed_stroke, tag);
            break;
        }
        // The other three references into `<defs>`. A shape drawn without the
        // clip, mask, or filter it was authored with is not a smaller version
        // of the icon — it is a different one, usually a solid slab where a
        // cut-out was intended.
        bool referenced = false;
        for (const char* property : {"clip-path", "mask-image", "filter"}) {
            const std::string value = trim(style(property));
            if (!value.empty() && value != "none") referenced = true;
        }
        if (referenced) {
            refuse(out, SvgRefusal::paint_reference, tag);
            break;
        }

        std::optional<std::string> path_data;
        if (tag == "path") {
            const std::string d = trim(index.attribute(node, "d"));
            if (!d.empty()) path_data = d;
        } else if (tag == "rect") {
            path_data = rect_path(index, node);
        } else if (tag == "circle") {
            path_data = ellipse_path(index, node, /*circle=*/true);
        } else if (tag == "ellipse") {
            path_data = ellipse_path(index, node, /*circle=*/false);
        } else if (tag == "line") {
            path_data = line_path(index, node);
        } else if (tag == "polygon") {
            path_data = points_path(index, node, /*close=*/true);
        } else if (tag == "polyline") {
            path_data = points_path(index, node, /*close=*/false);
        }
        if (!path_data) {
            refuse(out, SvgRefusal::shape_geometry, tag);
            break;
        }
        // A shape whose geometry is well-formed but encloses nothing (a
        // zero-radius circle, a `<path>` with no `d`) draws no pixels. Not a
        // refusal: there is nothing to capture either.
        if (path_data->empty()) continue;

        SvgShape shape;
        shape.node_index = node;
        shape.tag = tag;
        shape.path_data = *std::move(path_data);
        shape.fill = paint_colour(fill_value, style("fill-opacity"));
        shape.stroke = paint_colour(stroke_value, style("stroke-opacity"));
        if (const auto width = parse_length(style("stroke-width")))
            shape.stroke_width = *width;
        shape.even_odd_fill = trim(style("fill-rule")) == "evenodd";
        if (!shape.fill && !shape.stroke) continue;  // invisible, not refused
        out.shapes.push_back(std::move(shape));
    }

    if (out.refusal == SvgRefusal::none && out.shapes.empty())
        refuse(out, SvgRefusal::empty, "svg");
    if (out.refusal != SvgRefusal::none) out.shapes.clear();
    return out;
}

}  // namespace pulp::import_design
