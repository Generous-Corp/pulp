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
        // token leaves a paint site untouched, but a wrong color is
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

// Evaluate a `calc(...)` stop position far enough to place a stop: a chain of
// `+` / `-` terms, each a number carrying `%` or `px`.
//
// Percentages are already a fraction of the gradient line. Pixels are not —
// they need the line's LENGTH, which only the caller knows, so a px term
// without a length is REFUSED rather than approximated. A stop in the wrong
// place is indistinguishable downstream from one the author asked for.
//
// CSS requires whitespace around a binary `+`/`-` inside calc() (without it
// the `-` is part of the number), and that rule is enforced here rather than
// guessed at, so `calc(100%-7px)` is refused exactly as a browser refuses it.
// Anything richer — nested calc, var(), min/max/clamp, `*`, `/` — is refused
// too; this is a stop-position reader, not a CSS engine.
std::optional<float> eval_calc_position(const std::string& token,
                                        std::optional<float> length_px) {
    if (token.size() < 6 || token.compare(0, 5, "calc(") != 0 || token.back() != ')')
        return std::nullopt;
    const std::string expr = token.substr(5, token.size() - 6);

    float percent = 0.0f, pixels = 0.0f;
    float sign = 1.0f;
    bool expect_term = true;
    std::size_t i = 0;
    while (i < expr.size()) {
        if (std::isspace(static_cast<unsigned char>(expr[i]))) { ++i; continue; }
        if (!expect_term) {
            if (expr[i] != '+' && expr[i] != '-') return std::nullopt;
            // A binary operator must be surrounded by whitespace; leading
            // space was already consumed above, so only check the trailing one.
            if (i + 1 >= expr.size() ||
                !std::isspace(static_cast<unsigned char>(expr[i + 1])))
                return std::nullopt;
            sign = (expr[i] == '-') ? -1.0f : 1.0f;
            ++i;
            expect_term = true;
            continue;
        }
        std::size_t consumed = 0;
        float value = 0.0f;
        try {
            value = std::stof(expr.substr(i), &consumed);
        } catch (...) { return std::nullopt; }
        if (consumed == 0) return std::nullopt;
        i += consumed;
        const std::size_t unit_begin = i;
        while (i < expr.size() &&
               (std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '%'))
            ++i;
        const std::string unit = expr.substr(unit_begin, i - unit_begin);
        if (unit == "%")       percent += sign * value;
        else if (unit == "px") pixels  += sign * value;
        else                   return std::nullopt;  // bare number or em/rem/deg
        expect_term = false;
    }
    if (expect_term) return std::nullopt;  // empty, or a trailing operator

    float position = percent / 100.0f;
    if (pixels != 0.0f) {
        if (!length_px || !(*length_px > 0.0f)) return std::nullopt;
        position += pixels / *length_px;
    }
    return position;
}

// A stop position resolved to the gradient's own 0..1 parameter, or nullopt
// when the token is not a position at all.
//
// `angular` selects the units the gradient measures its stops in. A conic
// measures them as <angle-percentage>, and CSS conics are almost always
// authored in degrees — dropping the unit and keeping the number put `180deg`
// at 180.0, which is 18000% and clamps to the end of the ramp, so a two-colour
// sweep rendered as one flat colour. Linear and radial gradients measure
// lengths instead, where an angle is meaningless and is left alone.
std::optional<float> parse_stop_position(const std::string& tail, bool angular,
                                         std::optional<float> length_px) {
    if (tail.compare(0, 5, "calc(") == 0) return eval_calc_position(tail, length_px);
    const auto unit_at = [&](std::string_view unit) {
        return tail.size() > unit.size() &&
               tail.compare(tail.size() - unit.size(), unit.size(), unit) == 0;
    };
    std::string number = tail;
    float scale = 1.0f;  // bare number: already the 0..1 parameter
    if (!tail.empty() && tail.back() == '%') {
        number = tail.substr(0, tail.size() - 1);
        scale = 1.0f / 100.0f;
    } else if (unit_at("px")) {
        // A pixel offset needs the box to resolve against, which this parser
        // does not have. Kept as the historical raw-number reading rather than
        // silently becoming something else.
        number = tail.substr(0, tail.size() - 2);
    } else if (angular && unit_at("grad")) {
        number = tail.substr(0, tail.size() - 4);
        scale = 1.0f / 400.0f;
    } else if (angular && unit_at("turn")) {
        number = tail.substr(0, tail.size() - 4);
    } else if (angular && unit_at("deg")) {
        number = tail.substr(0, tail.size() - 3);
        scale = 1.0f / 360.0f;
    } else if (angular && unit_at("rad")) {
        number = tail.substr(0, tail.size() - 3);
        scale = 1.0f / 6.28318531f;
    }
    if (number.empty() ||
        !(std::isdigit(static_cast<unsigned char>(number[0])) ||
          number[0] == '.' || number[0] == '-'))
        return std::nullopt;
    // The number must be the WHOLE token. Without this, `calc(100% - 7px)`
    // reaches here as `7px)`, parses as 7, and lands the stop at 700%.
    try {
        std::size_t consumed = 0;
        const float value = std::stof(number, &consumed);
        if (consumed != number.size()) return std::nullopt;
        return value * scale;
    } catch (...) {
        return std::nullopt;
    }
}

// Paren-aware comma split (so rgba(...) stays intact) + trailing position peel.
// Fills colors/positions in parallel. `angular` selects the stop-position units
// (see parse_stop_position).
// `length_px` is the gradient line's length in pixels, used to resolve a `px`
// term inside a calc() stop position. Absent when the box is not yet laid out
// (the importer applies style before Yoga resolves bounds) or when the axis is
// angular, in which case a px-bearing position is refused rather than resolved
// against a zero-sized box.
// Returns false when a stop states a position this parser cannot evaluate. A
// stop with NO position of its own is spread evenly, which is what CSS asks
// for; a stop that names `calc(...)` and cannot be resolved is a different
// thing entirely, and spreading it evenly would put it somewhere the author
// did not ask for while looking like an ordinary render. Refusing the gradient
// is the loud answer, and it is what a browser does with the same declaration.
bool parse_stops(const std::string& colorStr,
                 const CssColorParser& parseColor,
                 std::vector<canvas::Color>& colors,
                 std::vector<float>& positions,
                 bool angular = false,
                 std::optional<float> length_px = std::nullopt) {
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
    // Peel positions off the RIGHT, up to two of them. One stop may carry two —
    // `#fff 0deg 2deg` is the shorthand for a hard band running from the first
    // to the second — and peeling only one left the other glued to the colour,
    // which took the colour down with it and made the whole stop unreadable.
    // Where the last whitespace-separated token starts, treating a parenthesised
    // group as part of the token it belongs to. `#fff calc(100% - 7px)` is a
    // colour and ONE position: splitting on the last space lands inside the
    // calc, peels `7px)`, and leaves `#fff calc(100% -` to be read as a colour —
    // which is how a hairline mask became a full spoke and why the render
    // differed with the spacing.
    const auto last_token_start = [](const std::string& s) -> std::size_t {
        std::size_t i = s.size();
        while (i > 0 && s[i - 1] == ' ') --i;
        int paren = 0;
        while (i > 0) {
            const char c = s[i - 1];
            if (c == ')') ++paren;
            else if (c == '(') { if (paren > 0) --paren; }
            else if (c == ' ' && paren == 0) return i;
            --i;
        }
        return std::string::npos;  // no separator: the whole token is the colour
    };
    // A stop that names calc() and cannot be evaluated makes the whole gradient
    // unreadable rather than merely position-less.
    bool refused = false;
    const auto peel = [&](std::string& tok) -> std::optional<float> {
        const auto start = last_token_start(tok);
        if (start == std::string::npos) return std::nullopt;
        const auto tail = tok.substr(start);
        const auto value = parse_stop_position(tail, angular, length_px);
        if (!value) {
            if (tail.compare(0, 5, "calc(") == 0) refused = true;
            return std::nullopt;
        }
        tok = tok.substr(0, start);
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        return value;
    };

    // A stop with no position of its own is spread evenly, which needs the
    // final stop COUNT — and a two-position stop contributes two. Resolve the
    // tokens first, then fill the gaps.
    struct Stop {
        canvas::Color color;
        std::optional<float> position;
    };
    std::vector<Stop> stops;
    stops.reserve(tokens.size());
    for (auto& token : tokens) {
        std::string tok = token;
        const auto second = peel(tok);
        const auto first = second ? peel(tok) : std::nullopt;
        const auto color = parseColor(tok);
        if (first) {
            stops.push_back({color, first});
            stops.push_back({color, second});
        } else {
            stops.push_back({color, second});
        }
    }

    if (refused) return false;

    for (size_t i = 0; i < stops.size(); ++i) {
        colors.push_back(stops[i].color);
        positions.push_back(stops[i].position.value_or(
            stops.size() > 1 ? static_cast<float>(i) / (stops.size() - 1) : 0));
    }
    return true;
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

    // The box a `px` term inside a calc() stop position resolves against.
    // Absent when the view has not been laid out yet — the importer applies
    // style before Yoga resolves bounds — so such a position is refused rather
    // than divided by a zero-sized box, which would place every stop at
    // infinity while looking like an ordinary parse.
    const auto box = v.local_bounds();
    const bool box_known = box.width > 0.0f && box.height > 0.0f;
    const auto line_length = [&](float x0, float y0, float x1, float y1)
        -> std::optional<float> {
        if (!box_known) return std::nullopt;
        const float dx = (x1 - x0) * box.width;
        const float dy = (y1 - y0) * box.height;
        return std::sqrt(dx * dx + dy * dy);
    };

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
        if (!parse_stops(inner.substr(color_start), color_of, colors, positions,
                         /*angular=*/false, line_length(x0, y0, x1, y1)))
            return false;
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
        // A radial stop position measures along the RADIUS, which the renderer
        // takes as radius_frac of the box's larger side.
        const std::optional<float> radius_px =
            box_known ? std::optional<float>(radius_frac *
                                             std::max(box.width, box.height))
                      : std::nullopt;
        if (!parse_stops(inner.substr(color_start), color_of, colors, positions,
                         /*angular=*/false, radius_px))
            return false;
        if (!colors.empty()) {
            v.set_background_gradient_radial(cx, cy, radius_frac, colors, positions);
            return true;
        }
        return false;
    }

    // conic-gradient([from <angle>] [at <pos>],] stop, stop, ...). CSS measures
    // `from` clockwise from the top (12 o'clock); the canvas sweep starts at +x
    // (3 o'clock), so we offset by -90deg to keep `from 0deg` pointing up.
    // `repeating-conic-gradient` differs from `conic-gradient` only in what
    // happens past the last stop: the band tiles instead of clamping. Same
    // prefix grammar, same stops, so the two share this branch and part ways
    // at the sweep span handed to the View.
    const bool conic_repeats =
        gradient.compare(0, 25, "repeating-conic-gradient(") == 0;
    if (conic_repeats || gradient.substr(0, 15) == "conic-gradient(") {
        const std::size_t open = conic_repeats ? 25 : 15;
        std::string inner = gradient.substr(open, gradient.size() - open - 1);
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
        // Conic stops are angles, not lengths, so there is no length for a `px`
        // term inside a calc() position to resolve against.
        if (!parse_stops(inner.substr(color_start), color_of, colors, positions,
                         /*angular=*/true, /*length_px=*/std::nullopt))
            return false;
        if (colors.empty()) return false;
        // The band a repeating conic tiles runs from 0 to its LAST stop, and
        // the shader wants that band expressed as 0..1. `repeating-conic-
        // gradient(#fff 0deg 2deg, #000 2deg 12deg)` therefore spans 12/360 of  docs-noise-lint: skip #000 is a CSS colour in an example
        // a turn with its stops rescaled onto it -- which is also why a band
        // that already covers a full turn simply degrades to a plain conic.
        float sweep_turns = 1.0f;
        if (conic_repeats) {
            const float span = *std::max_element(positions.begin(), positions.end());
            if (span > 0.0f && span < 1.0f) {
                sweep_turns = span;
                for (auto& p : positions) p /= span;
            }
        }
        v.set_background_gradient_conic(cx, cy, from_rad - 1.57079633f, colors,
                                        positions, sweep_turns);
        return true;
    }

    return false;
}

}  // namespace pulp::view
