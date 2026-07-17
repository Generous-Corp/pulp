#include <pulp/view/css_gradient.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <optional>
#include <sstream>
#include <vector>

#include <pulp/view/view.hpp>

// Shared CSS-gradient parser. The logic here was lifted verbatim from the JS
// WidgetBridge's setBackgroundGradient handler so the native materializer and
// baked C++ codegen resolve gradients byte-for-byte the same way the scripted
// UI does. See core/view/src/widget_bridge.cpp (setBackgroundGradient), which
// now delegates here.
namespace pulp::view {

// Built-in CSS color parser: #RGB / #RRGGBB / #RRGGBBAA, rgb(), rgba(),
// `transparent`. Mirrors WidgetBridge's parseColor minus named colors (the
// bridge passes its own parser to cover those). Exported via css_gradient.hpp.
canvas::Color parse_css_color(const std::string& str) {
    canvas::Color c = canvas::Color::rgba(1.0f, 1.0f, 1.0f, 1.0f);
    if (str.empty()) return c;
    if (str == "transparent") return canvas::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);

    if (str[0] == '#') {
        try {
            if (str.size() == 4) {  // #RGB -> #RRGGBB
                c.r = static_cast<float>(std::stoul(std::string(2, str[1]), nullptr, 16)) / 255.0f;
                c.g = static_cast<float>(std::stoul(std::string(2, str[2]), nullptr, 16)) / 255.0f;
                c.b = static_cast<float>(std::stoul(std::string(2, str[3]), nullptr, 16)) / 255.0f;
            } else if (str.size() >= 7) {
                c.r = static_cast<float>(std::stoul(str.substr(1, 2), nullptr, 16)) / 255.0f;
                c.g = static_cast<float>(std::stoul(str.substr(3, 2), nullptr, 16)) / 255.0f;
                c.b = static_cast<float>(std::stoul(str.substr(5, 2), nullptr, 16)) / 255.0f;
                if (str.size() >= 9)
                    c.a = static_cast<float>(std::stoul(str.substr(7, 2), nullptr, 16)) / 255.0f;
            }
        } catch (...) {}
        return c;
    }

    if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            try { vals[n] = std::stof(tok); } catch (...) { vals[n] = 0.0f; }
            ++n;
        }
        c.r = std::clamp(vals[0] / 255.0f, 0.0f, 1.0f);
        c.g = std::clamp(vals[1] / 255.0f, 0.0f, 1.0f);
        c.b = std::clamp(vals[2] / 255.0f, 0.0f, 1.0f);
        c.a = std::clamp(vals[3], 0.0f, 1.0f);  // alpha is already 0-1 in CSS
        return c;
    }

    if (str.compare(0, 4, "hsl(") == 0 || str.compare(0, 5, "hsla(") == 0) {
        // hsl() used to fall off the end of this function and return the opaque
        // WHITE default above. That is worse than refusing it: an unparseable
        // token leaves a paint site untouched, but a wrong colour is
        // indistinguishable downstream from a deliberate one, so an hsl() design
        // painted white and looked like a design decision.
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        // h is degrees (bare or `deg`), s/l are percentages, a is 0-1. Commas
        // and the modern space-separated form both appear in the wild; treat
        // both separators the same rather than parse CSS grammar properly.
        for (auto& ch : inner)
            if (ch == ',' || ch == '/') ch = ' ';
        float vals[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (ss >> tok && n < 4) {
            const auto pct = tok.find('%');
            if (pct != std::string::npos) tok.erase(pct);
            if (tok.size() > 3 && tok.compare(tok.size() - 3, 3, "deg") == 0)
                tok.erase(tok.size() - 3);
            try { vals[n] = std::stof(tok); } catch (...) { vals[n] = 0.0f; }
            ++n;
        }
        // Hue wraps rather than clamps — hsl(370) is hsl(10), and clamping it to
        // 360 would silently turn a red into a red-ish wrong.
        float h = std::fmod(vals[0], 360.0f);
        if (h < 0.0f) h += 360.0f;
        const float sat = std::clamp(vals[1] / 100.0f, 0.0f, 1.0f);
        const float light = std::clamp(vals[2] / 100.0f, 0.0f, 1.0f);
        const float chroma = (1.0f - std::fabs(2.0f * light - 1.0f)) * sat;
        const float hp = h / 60.0f;
        const float x = chroma * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if      (hp < 1.0f) { r = chroma; g = x;      b = 0.0f;   }
        else if (hp < 2.0f) { r = x;      g = chroma; b = 0.0f;   }
        else if (hp < 3.0f) { r = 0.0f;   g = chroma; b = x;      }
        else if (hp < 4.0f) { r = 0.0f;   g = x;      b = chroma; }
        else if (hp < 5.0f) { r = x;      g = 0.0f;   b = chroma; }
        else                { r = chroma; g = 0.0f;   b = x;      }
        const float m = light - chroma / 2.0f;
        c.r = std::clamp(r + m, 0.0f, 1.0f);
        c.g = std::clamp(g + m, 0.0f, 1.0f);
        c.b = std::clamp(b + m, 0.0f, 1.0f);
        c.a = std::clamp(vals[3], 0.0f, 1.0f);
        return c;
    }
    return c;
}

namespace {

// Paren-aware comma split (so rgba(...) stays intact) + trailing position peel
// (Npx / N%). Fills colors/positions in parallel.
void parse_stops(const std::string& colorStr,
                 const CssColorParser& parseColor,
                 std::vector<canvas::Color>& colors,
                 std::vector<float>& positions) {
    std::vector<std::string> tokens;
    std::string cur; int paren = 0;
    for (char c : colorStr) {
        if (c == '(') paren++;
        else if (c == ')') paren--;
        if (c == ',' && paren <= 0) {
            while (!cur.empty() && cur.front() == ' ') cur.erase(0, 1);
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (!cur.empty()) tokens.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    while (!cur.empty() && cur.front() == ' ') cur.erase(0, 1);
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) tokens.push_back(cur);
    for (size_t i = 0; i < tokens.size(); ++i) {
        std::string tok = tokens[i];
        std::optional<float> explicitPos;
        auto sp = tok.find_last_of(' ');
        if (sp != std::string::npos) {
            std::string tail = tok.substr(sp + 1);
            bool isPct = false;
            if (!tail.empty() && tail.back() == '%') { isPct = true; tail.pop_back(); }
            else if (tail.size() >= 2 && tail.substr(tail.size() - 2) == "px")
                tail = tail.substr(0, tail.size() - 2);
            if (!tail.empty() && (std::isdigit(static_cast<unsigned char>(tail[0])) ||
                                  tail[0] == '.' || tail[0] == '-')) {
                try {
                    float vv = std::stof(tail);
                    explicitPos = isPct ? vv / 100.0f : vv;
                    tok = tok.substr(0, sp);
                    while (!tok.empty() && tok.back() == ' ') tok.pop_back();
                } catch (...) {}
            }
        }
        colors.push_back(parseColor(tok));
        positions.push_back(explicitPos.value_or(
            tokens.size() > 1 ? static_cast<float>(i) / (tokens.size() - 1) : 0));
    }
}

// First top-level (paren-depth 0) comma — splits an optional shape/position/
// angle prefix from the color stops.
size_t top_level_comma(const std::string& s) {
    int paren = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') paren++;
        else if (s[i] == ')') paren--;
        else if (s[i] == ',' && paren <= 0) return i;
    }
    return std::string::npos;
}

// Parse "<n>%" / "left|right|top|bottom|center" into a [0,1] fraction.
float axis_frac(const std::string& t, float dflt) {
    if (t == "center") return 0.5f;
    if (t == "left" || t == "top") return 0.0f;
    if (t == "right" || t == "bottom") return 1.0f;
    if (!t.empty() && t.back() == '%') {
        try { return std::stof(t.substr(0, t.size() - 1)) / 100.0f; } catch (...) {}
    }
    return dflt;
}

// Pull "at X Y" out of a radial/conic prefix into cx/cy fractions.
void parse_at_center(const std::string& seg, float& cx, float& cy) {
    auto at = seg.find("at ");
    if (at == std::string::npos) return;
    std::istringstream is(seg.substr(at + 3));
    std::string a, b;
    if (is >> a) cx = axis_frac(a, cx);
    if (is >> b) cy = axis_frac(b, cy);
}

// CSS <angle> -> radians (deg default; rad/turn/grad honored).
float parse_angle(const std::string& t) {
    size_t i = 0;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) ||
                            t[i] == '.' || t[i] == '-' || t[i] == '+')) i++;
    float val = 0.0f;
    try { val = std::stof(t.substr(0, i)); } catch (...) { return 0.0f; }
    std::string unit = t.substr(i);
    if (unit == "rad")  return val;
    if (unit == "turn") return val * 6.28318531f;
    if (unit == "grad") return val * 3.14159265f / 200.0f;
    return val * 3.14159265f / 180.0f;  // deg
}

}  // namespace

bool apply_css_background_gradient(View& v, std::string_view css_view,
                                   const CssColorParser& parse_color) {
    std::string gradient(css_view);
    if (gradient.empty()) return false;
    const CssColorParser color_of = parse_color
        ? parse_color
        : CssColorParser(&parse_css_color);

    // Simple parser for "linear-gradient(to right, color1, color2, ...)"
    if (gradient.substr(0, 16) == "linear-gradient(") {
        auto inner = gradient.substr(16, gradient.size() - 17);
        float x0 = 0, y0 = 0, x1 = 0, y1 = 1;  // default: to bottom
        size_t color_start = 0;
        if (inner.substr(0, 8) == "to right") { x0=0; y0=0; x1=1; y1=0; color_start = inner.find(',') + 1; }
        else if (inner.substr(0, 9) == "to bottom") { x0=0; y0=0; x1=0; y1=1; color_start = inner.find(',') + 1; }
        else if (inner.substr(0, 7) == "to left") { x0=1; y0=0; x1=0; y1=0; color_start = inner.find(',') + 1; }
        else if (inner.substr(0, 6) == "to top") { x0=0; y0=1; x1=0; y1=0; color_start = inner.find(',') + 1; }

        std::vector<canvas::Color> colors;
        std::vector<float> positions;
        parse_stops(inner.substr(color_start), color_of, colors, positions);
        if (!colors.empty()) {
            v.set_background_gradient_linear(x0, y0, x1, y1, colors, positions);
            return true;
        }
        return false;
    }

    // radial-gradient([<shape>] [at <pos>],] stop, stop, ...). Sizing keywords
    // are best-effort (radius approximated as a fraction of max(w,h)).
    if (gradient.substr(0, 16) == "radial-gradient(") {
        std::string inner = gradient.substr(16, gradient.size() - 17);
        float cx = 0.5f, cy = 0.5f;
        float radius_frac = 0.7071f;  // ~farthest-corner of a square box (default)
        size_t color_start = 0;
        size_t fc = top_level_comma(inner);
        if (fc != std::string::npos) {
            std::string seg = inner.substr(0, fc);
            if (seg.find("at ") != std::string::npos ||
                seg.rfind("circle", 0) == 0 || seg.rfind("ellipse", 0) == 0 ||
                seg.rfind("closest", 0) == 0 || seg.rfind("farthest", 0) == 0) {
                parse_at_center(seg, cx, cy);
                if (seg.find("closest-side") != std::string::npos)        radius_frac = 0.5f;
                else if (seg.find("closest-corner") != std::string::npos) radius_frac = 0.6f;
                else if (seg.find("farthest-side") != std::string::npos)  radius_frac = 0.6f;
                else if (seg.find("farthest-corner") != std::string::npos) radius_frac = 0.7071f;
                color_start = fc + 1;
            }
        }
        std::vector<canvas::Color> colors;
        std::vector<float> positions;
        parse_stops(inner.substr(color_start), color_of, colors, positions);
        if (!colors.empty()) {
            v.set_background_gradient_radial(cx, cy, radius_frac, colors, positions);
            return true;
        }
        return false;
    }

    // conic-gradient([from <angle>] [at <pos>],] stop, stop, ...). CSS measures
    // `from` clockwise from the top (12 o'clock); the canvas sweep starts at +x
    // (3 o'clock), so we offset by -90deg to keep `from 0deg` pointing up.
    if (gradient.substr(0, 15) == "conic-gradient(") {
        std::string inner = gradient.substr(15, gradient.size() - 16);
        float cx = 0.5f, cy = 0.5f, from_rad = 0.0f;
        size_t color_start = 0;
        size_t fc = top_level_comma(inner);
        if (fc != std::string::npos) {
            std::string seg = inner.substr(0, fc);
            if (seg.rfind("from ", 0) == 0 || seg.find(" at ", 0) != std::string::npos ||
                seg.rfind("at ", 0) == 0) {
                auto fromPos = seg.find("from ");
                if (fromPos != std::string::npos) {
                    std::istringstream is(seg.substr(fromPos + 5));
                    std::string ang; if (is >> ang) from_rad = parse_angle(ang);
                }
                parse_at_center(seg, cx, cy);
                color_start = fc + 1;
            }
        }
        std::vector<canvas::Color> colors;
        std::vector<float> positions;
        parse_stops(inner.substr(color_start), color_of, colors, positions);
        if (!colors.empty()) {
            v.set_background_gradient_conic(cx, cy, from_rad - 1.57079633f, colors, positions);
            return true;
        }
        return false;
    }

    return false;
}

}  // namespace pulp::view
