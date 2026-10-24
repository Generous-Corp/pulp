// SPDX-License-Identifier: MIT
#include "svg_shape_lowering.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
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
/// `<switch>` is deliberately NOT here: it renders the FIRST child whose
/// requirement attributes evaluate true, not all of them, so treating it as a
/// plain group draws every alternative at once.
bool is_group_element(std::string_view tag) {
    return tag == "g" || tag == "a";
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

/// Trimmed, with every internal run of whitespace collapsed to one space.
///
/// `preserveAspectRatio` is two space-separated tokens in the source, and an
/// author who wrapped the attribute across lines wrote a newline between them.
/// Comparing the raw string against `"none meet"` reads that as a third,
/// unknown value and refuses a drawing this lowering can draw.
std::string collapse_whitespace(std::string_view value) {
    std::string out;
    bool pending_space = false;
    for (const unsigned char c : value) {
        if (std::isspace(c) != 0) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(c));
    }
    return out;
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

/// A plain number, as `stroke-miterlimit` and `pathLength` are written.
std::optional<double> parse_number(std::string_view raw) {
    const std::string text = trim(raw);
    if (text.empty())
        return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(value))
        return std::nullopt;
    return value;
}

/// An `<alpha-value>`: a number, or a percentage of 1. Clamped to [0, 1] as
/// CSS clamps it at computed-value time.
std::optional<double> parse_alpha(std::string_view raw) {
    std::string text = trim(raw);
    bool percent = false;
    if (!text.empty() && text.back() == '%') {
        percent = true;
        text.pop_back();
    }
    auto value = parse_number(text);
    if (!value)
        return std::nullopt;
    return std::clamp(percent ? *value / 100.0 : *value, 0.0, 1.0);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool is_paint(const std::string& value) {
    return !value.empty() && value != "none" && value != "transparent";
}

/// `colour` with `alpha` multiplied into its own alpha, or nullopt when the
/// colour is not one Chrome serializes as `rgb(r, g, b)` / `rgba(r, g, b, a)`
/// and so cannot be re-serialized without a colour parser this lowering does
/// not carry.
std::optional<std::string> with_alpha(const std::string& colour, double alpha) {
    if (alpha >= 1.0)
        return colour;
    const bool rgba = colour.rfind("rgba(", 0) == 0;
    if ((!rgba && colour.rfind("rgb(", 0) != 0) || colour.back() != ')')
        return std::nullopt;
    const std::size_t open = colour.find('(');
    std::vector<std::string> parts;
    std::string_view body(colour);
    body = body.substr(open + 1, body.size() - open - 2);
    while (true) {
        const auto comma = body.find(',');
        parts.push_back(trim(body.substr(0, comma)));
        if (comma == std::string_view::npos)
            break;
        body.remove_prefix(comma + 1);
    }
    if (parts.size() == 4) {
        const auto own = parse_alpha(parts[3]);
        if (!own)
            return std::nullopt;
        alpha *= *own;
    } else if (parts.size() != 3) {
        return std::nullopt;
    }
    return "rgba(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + number(alpha) + ")";
}

/// One `name: value` declaration out of an inline `style` attribute, with
/// `!important` dropped. Empty when the attribute does not declare `name`;
/// the LAST declaration wins, as it does in the cascade.
std::string inline_declaration(std::string_view style, std::string_view name) {
    std::string found;
    while (!style.empty()) {
        const auto semicolon = style.find(';');
        const std::string_view declaration = style.substr(0, semicolon);
        style.remove_prefix(semicolon == std::string_view::npos ? style.size() : semicolon + 1);
        const auto colon = declaration.find(':');
        if (colon == std::string_view::npos)
            continue;
        if (lowercase(trim(declaration.substr(0, colon))) != name)
            continue;
        std::string value = trim(declaration.substr(colon + 1));
        if (const auto bang = value.find('!'); bang != std::string::npos)
            value = trim(std::string_view(value).substr(0, bang));
        found = std::move(value);
    }
    return found;
}

/// How one presentation property resolved for a shape.
///
/// Chrome's computed value is the authority whenever the capture collected
/// the property: it alone has seen the stylesheet. A capture that did not
/// collect it still carries the AUTHORED declarations, so those are walked the
/// way the cascade walks them — an inline `style` declaration outranks a
/// presentation attribute on the same element, a nearer element outranks a
/// farther one, and an inherited property keeps climbing past the shape (icon
/// sets declare `stroke-linecap` once on the root `<svg>`). Presentation
/// attributes are read only on the root and inside it; above it only an
/// inline `style` can reach the shape.
///
/// `valid` rejects a value the browser would reject, so the walk continues to
/// the inherited value exactly as an invalid declaration is dropped by the
/// cascade. nullopt means the initial value applies.
template <typename Valid>
std::optional<std::string> presentation_value(const CapturedStyleIndex& index,
                                              const std::map<std::string, std::string>& computed,
                                              int node, int svg_root, std::string_view name,
                                              bool inherited, Valid valid) {
    if (index.has_property(name)) {
        const auto found = computed.find(std::string(name));
        if (found != computed.end()) {
            const std::string value = trim(found->second);
            if (valid(value))
                return value;
            return std::nullopt;
        }
    }
    bool in_svg = true;
    for (int current = node, steps = 0; current >= 0 && steps < index.node_count(); ++steps) {
        if (index.node_type_of(current) == kElementNode) {
            for (const bool from_style : {true, false}) {
                if (!from_style && !in_svg)
                    continue;
                const std::string raw =
                    from_style ? inline_declaration(index.attribute(current, "style"), name)
                               : trim(index.attribute(current, name));
                if (raw.empty())
                    continue;
                const std::string keyword = lowercase(raw);
                if (keyword == "initial")
                    return std::nullopt;
                if (keyword == "inherit" || keyword == "unset")
                    break;
                if (valid(raw))
                    return raw;
            }
        }
        if (!inherited)
            break;
        if (current == svg_root)
            in_svg = false;
        current = index.parent_of(current);
    }
    return std::nullopt;
}

bool is_keyword_of(const std::string& value, std::initializer_list<std::string_view> keywords) {
    const std::string keyword = lowercase(value);
    return std::find(keywords.begin(), keywords.end(), keyword) != keywords.end();
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
        case SvgRefusal::shape_opacity:
            return "shape-opacity";
        case SvgRefusal::paint_alpha:
            return "paint-alpha";
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

    // How the viewBox maps into the node's box. Two of the values are
    // representable as a transform the renderer can build from the box and the
    // viewBox alone, and both are carried rather than refused:
    //
    //   xMidYMid meet  — SVG's default: uniform scale, centred. Implicit when
    //                    the attribute is absent.
    //   none           — the two axes scale independently, no centring. Per
    //                    SVG, `meetOrSlice` is ignored once the alignment is
    //                    `none`, so all three spellings are the same mapping.
    //
    // Everything else still refuses. A non-centre alignment needs an offset
    // rule per axis, and `slice` overflows the box and crops — neither is the
    // single scale-and-translate this lowering hands the renderer, and drawing
    // one as if it were the other puts the geometry somewhere the author did
    // not put it. The refusal names the value so the gap stays readable.
    if (const std::string aspect =
            collapse_whitespace(index.attribute(svg_node_index,
                                                "preserveAspectRatio"));
        !aspect.empty() && aspect != "xMidYMid" && aspect != "xMidYMid meet") {
        if (aspect == "none" || aspect == "none meet" ||
            aspect == "none slice") {
            out.stretch_to_box = true;
        } else {
            refuse(out, SvgRefusal::element, "preserveAspectRatio=" + aspect);
            return out;
        }
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
        const auto resolved = [&](std::string_view name, bool inherited, auto valid) {
            return presentation_value(index, computed, node, svg_node_index, name, inherited,
                                      valid);
        };
        const auto any_value = [](const std::string& value) { return !value.empty(); };
        const auto dash = resolved("stroke-dasharray", /*inherited=*/true, any_value);
        if (dash && lowercase(*dash) != "none") {
            refuse(out, SvgRefusal::dashed_stroke, tag);
            break;
        }
        // The other references into `<defs>`. A shape drawn without the clip,
        // mask, filter, or marker it was authored with is not a smaller
        // version of the icon — it is a different one, usually a solid slab
        // where a cut-out was intended, or a line missing its arrowhead.
        bool referenced = false;
        for (const char* property : {"clip-path", "mask-image", "filter"}) {
            const std::string value = trim(style(property));
            if (!value.empty() && value != "none") referenced = true;
        }
        for (const char* property : {"marker-start", "marker-mid", "marker-end"}) {
            const auto marker = resolved(property, /*inherited=*/true, any_value);
            if (marker && lowercase(*marker) != "none")
                referenced = true;
        }
        if (referenced) {
            refuse(out, SvgRefusal::paint_reference, tag);
            break;
        }

        std::optional<std::string> path_data;
        if (tag == "path") {
            // An empty `d` is legal and draws nothing, so it falls through to
            // the no-ink skip below rather than refusing the whole icon.
            path_data = trim(index.attribute(node, "d"));
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

        const auto alpha_of = [&](std::string_view name, bool inherited) {
            const auto value = resolved(
                name, inherited, [](const std::string& v) { return parse_alpha(v).has_value(); });
            return value ? *parse_alpha(*value) : 1.0;
        };
        const double opacity = alpha_of("opacity", /*inherited=*/false);
        const double fill_alpha = alpha_of("fill-opacity", /*inherited=*/true);
        const double stroke_alpha = alpha_of("stroke-opacity", /*inherited=*/true);
        // A `<line>` encloses no area, so its fill (black by default) paints
        // nothing and must not count as a second paint under `opacity`.
        const bool fills = tag != "line" && is_paint(fill_value) && fill_alpha > 0.0;
        const bool strokes = is_paint(stroke_value) && stroke_alpha > 0.0;
        if (opacity <= 0.0 || (!fills && !strokes))
            continue; // invisible, not refused
        // With one paint, the shape's layer holds exactly that paint, so
        // compositing the layer at `opacity` is the same pixel as drawing the
        // paint at `opacity` times its own alpha. With two it is not.
        if (opacity < 1.0 && fills && strokes) {
            refuse(out, SvgRefusal::shape_opacity, tag);
            break;
        }

        SvgShape shape;
        shape.node_index = node;
        shape.tag = tag;
        shape.path_data = *std::move(path_data);
        if (fills) {
            shape.fill = with_alpha(fill_value, fill_alpha * opacity);
            if (!shape.fill) {
                refuse(out, SvgRefusal::paint_alpha, tag);
                break;
            }
        }
        if (strokes) {
            shape.stroke = with_alpha(stroke_value, stroke_alpha * opacity);
            if (!shape.stroke) {
                refuse(out, SvgRefusal::paint_alpha, tag);
                break;
            }
        }
        if (const auto width = parse_length(style("stroke-width")))
            shape.stroke_width = *width;
        shape.even_odd_fill = trim(style("fill-rule")) == "evenodd";

        if (const auto cap =
                resolved("stroke-linecap", /*inherited=*/true, [](const std::string& v) {
                    return is_keyword_of(v, {"butt", "round", "square"});
                }))
            shape.stroke_linecap = lowercase(*cap);
        if (const auto join =
                resolved("stroke-linejoin", /*inherited=*/true, [](const std::string& v) {
                    return is_keyword_of(v, {"miter", "round", "bevel"});
                }))
            shape.stroke_linejoin = lowercase(*join);
        if (const auto limit =
                resolved("stroke-miterlimit", /*inherited=*/true, [](const std::string& v) {
                    const auto n = parse_number(v);
                    return n && *n >= 1.0;
                }))
            shape.stroke_miterlimit = *parse_number(*limit);
        if (const auto offset =
                resolved("stroke-dashoffset", /*inherited=*/true,
                         [](const std::string& v) { return parse_length(v).has_value(); }))
            shape.stroke_dashoffset = *parse_length(*offset);
        if (const auto length = parse_number(index.attribute(node, "pathLength"));
            length && *length > 0.0)
            shape.path_length = *length;
        out.shapes.push_back(std::move(shape));
    }

    if (out.refusal == SvgRefusal::none && out.shapes.empty())
        refuse(out, SvgRefusal::empty, "svg");
    if (out.refusal != SvgRefusal::none) out.shapes.clear();
    return out;
}

}  // namespace pulp::import_design
