#include <pulp/view/svg_fragment.hpp>

#include <cmath>
#include <cstdio>

namespace pulp::view {

bool FragmentTransform::is_identity() const {
    return dx == 0.0f && dy == 0.0f && rotate_deg == 0.0f && scale == 1.0f;
}

std::string FragmentTransform::to_svg_transform() const {
    if (is_identity()) return {};
    std::string out;
    char buf[128];
    if (dx != 0.0f || dy != 0.0f) {
        std::snprintf(buf, sizeof(buf), "translate(%.3f %.3f)", dx, dy);
        out += buf;
    }
    if (rotate_deg != 0.0f) {
        if (!out.empty()) out += ' ';
        std::snprintf(buf, sizeof(buf), "rotate(%.3f %.3f %.3f)",
                      rotate_deg, pivot_x, pivot_y);
        out += buf;
    }
    if (scale != 1.0f) {
        if (!out.empty()) out += ' ';
        // Scale about the pivot: translate to pivot, scale, translate back — the
        // idiomatic SVG "scale around a point" since <g> scale() is origin-based.
        std::snprintf(buf, sizeof(buf),
                      "translate(%.3f %.3f) scale(%.4f) translate(%.3f %.3f)",
                      pivot_x, pivot_y, scale, -pivot_x, -pivot_y);
        out += buf;
    }
    return out;
}

std::string extract_svg_fragment(const std::string& svg, const std::string& marker) {
    if (marker.empty()) return {};
    const auto mp = svg.find(marker);
    if (mp == std::string::npos) return {};
    // Element start: the '<' at or before the marker (matches the
    // wrap_needle_rotation element-start convention — any tag: path/rect/g/…).
    const auto start = svg.rfind('<', mp);
    if (start == std::string::npos) return {};
    // End of the OPENING tag.
    const auto gt = svg.find('>', mp < start ? start : mp);
    if (gt == std::string::npos) return {};
    // Self-closing element ("… />"): the fragment is [start, gt].
    if (gt > start && svg[gt - 1] == '/')
        return svg.substr(start, gt - start + 1);

    // Container element: read the tag name, then depth-match its close tag so a
    // <g>…nested <g>…</g>…</g> subtree comes out whole.
    std::size_t np = start + 1;
    std::string name;
    while (np < svg.size()) {
        const char c = svg[np];
        if (c == ' ' || c == '>' || c == '/' || c == '\t' || c == '\n') break;
        name += c;
        ++np;
    }
    if (name.empty()) return {};
    const std::string open_tok = "<" + name;
    const std::string close_tok = "</" + name;
    std::size_t i = gt + 1;
    int depth = 1;
    while (i < svg.size() && depth > 0) {
        const auto no = svg.find(open_tok, i);
        const auto nc = svg.find(close_tok, i);
        if (nc == std::string::npos) return {};   // unbalanced
        if (no != std::string::npos && no < nc) {
            // Only count as a nested open if it is a real element start
            // (followed by whitespace or '>'), not a prefix collision.
            const char after = no + open_tok.size() < svg.size()
                                   ? svg[no + open_tok.size()] : '\0';
            if (after == ' ' || after == '>' || after == '/' ||
                after == '\t' || after == '\n') {
                ++depth;
            }
            i = no + open_tok.size();
        } else {
            --depth;
            i = svg.find('>', nc);
            if (i == std::string::npos) return {};
            ++i;
        }
    }
    if (depth != 0) return {};
    return svg.substr(start, i - start);
}

std::string svg_open_tag(const std::string& svg) {
    const auto p = svg.find("<svg");
    if (p == std::string::npos) return {};
    const auto gt = svg.find('>', p);
    if (gt == std::string::npos) return {};
    return svg.substr(p, gt - p + 1);
}

namespace {
// Replace the value of every `key="…"` attribute whose value is not "none"
// with `hex`. Leaves other attributes untouched.
void replace_paint_attr(std::string& frag, const char* key, const std::string& hex) {
    const std::string needle = std::string(key) + "=\"";
    std::size_t pos = 0;
    while ((pos = frag.find(needle, pos)) != std::string::npos) {
        const std::size_t vs = pos + needle.size();
        const std::size_t ve = frag.find('"', vs);
        if (ve == std::string::npos) break;
        const std::string value = frag.substr(vs, ve - vs);
        if (value != "none") {
            frag.replace(vs, ve - vs, hex);
            pos = vs + hex.size() + 1;  // past the closing quote we didn't move
        } else {
            pos = ve + 1;
        }
    }
}
}  // namespace

std::string recolor_svg_fragment(std::string fragment, const std::string& hex,
                                 bool include_stroke) {
    if (hex.empty()) return fragment;
    replace_paint_attr(fragment, "fill", hex);
    if (include_stroke) replace_paint_attr(fragment, "stroke", hex);
    return fragment;
}

std::string build_svg_fragment_document(const std::string& source_svg,
                                        const std::string& fragment,
                                        const FragmentTransform& xform,
                                        float opacity,
                                        const std::string& recolor_hex) {
    const std::string header = svg_open_tag(source_svg);
    if (header.empty() || fragment.empty()) return {};

    // A fragment "recolor" tints the whole sub-tree, so recolor both fill AND
    // stroke (a needle/thumb is often stroke-only). fill="none" is still left
    // alone so an intentional outline stays an outline.
    std::string body = recolor_hex.empty()
                           ? fragment
                           : recolor_svg_fragment(fragment, recolor_hex,
                                                   /*include_stroke=*/true);

    const std::string xf = xform.to_svg_transform();
    const bool wrap_opacity = opacity < 1.0f;
    std::string out = header;
    if (!xf.empty() || wrap_opacity) {
        out += "<g";
        if (!xf.empty()) { out += " transform=\""; out += xf; out += "\""; }
        if (wrap_opacity) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), " opacity=\"%.4f\"",
                          opacity < 0.0f ? 0.0f : opacity);
            out += buf;
        }
        out += ">";
        out += body;
        out += "</g>";
    } else {
        out += body;
    }
    out += "</svg>";
    return out;
}

}  // namespace pulp::view
