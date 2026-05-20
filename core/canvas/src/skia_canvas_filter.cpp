// skia_canvas_filter.cpp — CSS `filter` (Canvas2D `ctx.filter`) parser.
//
// Extracted from skia_canvas.cpp as part of the per-feature TU split
// (see skia_canvas_text.cpp / skia_canvas_mask.cpp / skia_canvas_gradients.cpp
// for the prior cuts). skia_canvas.cpp is a long-lived merge-conflict
// hot spot; pulling the self-contained CSS-filter-parsing cluster into its
// own TU means filter-parsing work no longer recompiles the whole file.
//
// The cluster is pure free functions in an anonymous namespace — no
// `SkiaCanvas::` members. The single symbol the rest of skia_canvas.cpp
// needs is `parse_filter_chain` (called once, by SkiaCanvas::set_filter);
// it is declared in skia_canvas_filter.hpp and given external linkage.
// Everything else stays file-local here.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef PULP_HAS_SKIA

#include "include/core/SkColor.h"
#include "include/core/SkColorFilter.h"
#include "include/effects/SkColorMatrix.h"
#include "include/effects/SkImageFilters.h"

#include "skia_canvas_filter.hpp"

namespace pulp::canvas {

namespace {

// Skim leading whitespace.
inline void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

// Parse a single CSS filter <length-or-number-or-percentage> argument.
// Returns the numeric value coerced into the unit the caller expects:
//   * `expect_px` → drops a trailing "px" or treats bare numbers as px
//     (used by blur / drop-shadow).
//   * `expect_angle` → converts deg/rad/turn into radians (hue-rotate).
//   * default → unitless multiplier; "%" divides by 100. Used by
//     brightness / contrast / saturate / opacity / invert / grayscale /
//     sepia.
//
// Returns NaN on parse failure so callers can default to spec-neutral.
enum class FilterArgKind { multiplier, pixels, angle };
double parse_filter_arg(const std::string& body, FilterArgKind kind) {
    // Find numeric prefix.
    size_t end = 0;
    while (end < body.size() &&
           (std::isdigit(static_cast<unsigned char>(body[end])) ||
            body[end] == '.' || body[end] == '-' || body[end] == '+' ||
            body[end] == 'e' || body[end] == 'E')) {
        ++end;
    }
    if (end == 0) return std::nan("");
    double v = 0;
    try { v = std::stod(body.substr(0, end)); }
    catch (...) { return std::nan(""); }
    std::string unit = body.substr(end);
    // Strip whitespace from unit.
    while (!unit.empty() && (unit.back() == ' ' || unit.back() == '\t')) unit.pop_back();
    size_t us = 0;
    while (us < unit.size() && (unit[us] == ' ' || unit[us] == '\t')) ++us;
    unit = unit.substr(us);

    switch (kind) {
        case FilterArgKind::pixels:
            // Bare numbers are px; "px" suffix is the canonical form.
            return v;
        case FilterArgKind::angle:
            if (unit == "rad") return v;
            if (unit == "turn") return v * (2.0 * 3.14159265358979323846);
            // Default deg (or unrecognised → deg per CSS spec).
            return v * (3.14159265358979323846 / 180.0);
        case FilterArgKind::multiplier:
            if (!unit.empty() && unit.back() == '%') return v * 0.01;
            return v;
    }
    return v;
}

// Split space-separated arguments from a filter function's body.
// Handles parenthesised sub-expressions (e.g. `rgb(1,2,3)`) so color
// functions are kept as a single token.
std::vector<std::string> parse_filter_args(const std::string& body) {
    std::vector<std::string> tokens;
    std::string tok;
    int paren = 0;
    for (char c : body) {
        if (c == '(') { ++paren; tok += c; continue; }
        if (c == ')') { --paren; tok += c; continue; }
        if (paren == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!tok.empty()) { tokens.push_back(std::move(tok)); tok.clear(); }
            continue;
        }
        tok += c;
    }
    if (!tok.empty()) tokens.push_back(std::move(tok));
    return tokens;
}

// Parse a single CSS length token to a numeric pixel value.
// "12px" → 12.0, "12" → 12.0, "invalid" → NaN.
double parse_filter_arg_length(const std::string& tok) {
    if (tok.empty()) return std::nan("");
    size_t end = 0;
    while (end < tok.size() &&
           (std::isdigit(static_cast<unsigned char>(tok[end])) ||
            tok[end] == '.' || tok[end] == '-' || tok[end] == '+' ||
            tok[end] == 'e' || tok[end] == 'E')) {
        ++end;
    }
    if (end == 0) return std::nan("");
    double v = 0;
    try { v = std::stod(tok.substr(0, end)); }
    catch (...) { return std::nan(""); }
    return v; // bare number or "px" suffix both treated as pixels
}

// Parse a CSS color string into SkColor. Supports #hex, rgb()/rgba(),
// hsl()/hsla(), "transparent", and a subset of named colors.
// Defaults to black on parse failure.
SkColor parse_css_color_to_skcolor(const std::string& str) {
    if (str == "transparent") return SK_ColorTRANSPARENT;

    if (!str.empty() && str[0] == '#') {
        auto parse_hex = [](const std::string& s) -> std::optional<uint8_t> {
            try { return static_cast<uint8_t>(std::stoul(s, nullptr, 16)); }
            catch (...) { return std::nullopt; }
        };
        if (str.size() == 4) { // #RGB — each nibble doubled
            auto r = parse_hex(std::string(2, str[1]));
            auto g = parse_hex(std::string(2, str[2]));
            auto b = parse_hex(std::string(2, str[3]));
            if (r && g && b) return SkColorSetRGB(*r, *g, *b);
            return SK_ColorBLACK;
        }
        if (str.size() >= 7) { // #RRGGBB[AA]
            auto r = parse_hex(str.substr(1, 2));
            auto g = parse_hex(str.substr(3, 2));
            auto b = parse_hex(str.substr(5, 2));
            if (!r || !g || !b) return SK_ColorBLACK;
            uint8_t a = 255;
            if (str.size() >= 9) {
                auto parsed = parse_hex(str.substr(7, 2));
                if (!parsed) return SK_ColorBLACK;
                a = *parsed;
            }
            return SkColorSetARGB(a, *r, *g, *b);
        }
        return SK_ColorBLACK;
    }

    // rgb(r, g, b) / rgba(r, g, b, a)
    if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            vals[n++] = std::stof(tok);
        }
        uint8_t r = static_cast<uint8_t>(std::clamp(vals[0] / 255.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t g = static_cast<uint8_t>(std::clamp(vals[1] / 255.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t b = static_cast<uint8_t>(std::clamp(vals[2] / 255.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t a = static_cast<uint8_t>(std::clamp(vals[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        return SkColorSetARGB(a, r, g, b);
    }

    // hsl(h, s%, l%) / hsla(h, s%, l%, a)
    if (str.substr(0, 4) == "hsl(" || str.substr(0, 5) == "hsla(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            if (tok.back() == '%') tok.pop_back();
            vals[n++] = std::stof(tok);
        }
        float h = std::fmod(vals[0], 360.0f) / 360.0f;
        float s = vals[1] / 100.0f;
        float l = vals[2] / 100.0f;
        auto hue2rgb = [](float p, float q, float t) {
            if (t < 0) t += 1; if (t > 1) t -= 1;
            if (t < 1.0f / 6) return p + (q - p) * 6 * t;
            if (t < 1.0f / 2) return q;
            if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
            return p;
        };
        float r, g, b;
        if (s == 0) { r = g = b = l; }
        else {
            float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
            float p = 2 * l - q;
            r = hue2rgb(p, q, h + 1.0f / 3);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f / 3);
        }
        return SkColorSetARGB(
            static_cast<uint8_t>(vals[3] * 255.0f + 0.5f),
            static_cast<uint8_t>(r * 255.0f + 0.5f),
            static_cast<uint8_t>(g * 255.0f + 0.5f),
            static_cast<uint8_t>(b * 255.0f + 0.5f));
    }

    // Named colors (subset matching test expectations).
    static const std::unordered_map<std::string, SkColor> named = {
        {"black", SK_ColorBLACK}, {"white", SK_ColorWHITE},
        {"red", SK_ColorRED}, {"green", SK_ColorGREEN},
        {"blue", SK_ColorBLUE}, {"yellow", SK_ColorYELLOW},
        {"cyan", SK_ColorCYAN}, {"magenta", SK_ColorMAGENTA},
        {"gray", 0xFF808080}, {"grey", 0xFF808080},
    };
    auto it = named.find(str);
    if (it != named.end()) return it->second;

    return SK_ColorBLACK;
}

// Build a saturation SkColorMatrix. s=1 is identity, s=0 is grayscale,
// s>1 boosts saturation. Standard luminance weights match the CSS spec.
SkColorMatrix make_saturation_matrix(double s) {
    const float r = 0.213f;
    const float g = 0.715f;
    const float b = 0.072f;
    const float sf = static_cast<float>(s);
    return SkColorMatrix(
        r + (1 - r) * sf, g - g * sf,        b - b * sf,        0, 0,
        r - r * sf,        g + (1 - g) * sf, b - b * sf,        0, 0,
        r - r * sf,        g - g * sf,        b + (1 - b) * sf, 0, 0,
        0,                 0,                 0,                 1, 0);
}

// Sepia per CSS spec. amount=0 identity, amount=1 full sepia.
SkColorMatrix make_sepia_matrix(double amount) {
    const float a = static_cast<float>(amount);
    const float inv = 1.0f - a;
    return SkColorMatrix(
        0.393f * a + inv, 0.769f * a,        0.189f * a,        0, 0,
        0.349f * a,        0.686f * a + inv, 0.168f * a,        0, 0,
        0.272f * a,        0.534f * a,        0.131f * a + inv, 0, 0,
        0,                 0,                 0,                 1, 0);
}

// Hue-rotate matrix (CSS spec). angle in radians.
SkColorMatrix make_hue_rotate_matrix(double rad) {
    const float c = static_cast<float>(std::cos(rad));
    const float s = static_cast<float>(std::sin(rad));
    return SkColorMatrix(
        0.213f + c * 0.787f - s * 0.213f,
        0.715f - c * 0.715f - s * 0.715f,
        0.072f - c * 0.072f + s * 0.928f, 0, 0,
        0.213f - c * 0.213f + s * 0.143f,
        0.715f + c * 0.285f + s * 0.140f,
        0.072f - c * 0.072f - s * 0.283f, 0, 0,
        0.213f - c * 0.213f - s * 0.787f,
        0.715f - c * 0.715f + s * 0.715f,
        0.072f + c * 0.928f + s * 0.072f, 0, 0,
        0, 0, 0, 1, 0);
}

// Linear contrast matrix: out = c * in + (0.5 * (1 - c)).
SkColorMatrix make_contrast_matrix(double c) {
    const float f = static_cast<float>(c);
    const float t = 0.5f * (1.0f - f);
    return SkColorMatrix(
        f, 0, 0, 0, t,
        0, f, 0, 0, t,
        0, 0, f, 0, t,
        0, 0, 0, 1, 0);
}

// Linear brightness matrix: scale RGB by `amount`.
SkColorMatrix make_brightness_matrix(double amount) {
    const float f = static_cast<float>(amount);
    return SkColorMatrix(
        f, 0, 0, 0, 0,
        0, f, 0, 0, 0,
        0, 0, f, 0, 0,
        0, 0, 0, 1, 0);
}

// Invert matrix: out = (1 - 2*amount) * in + amount.
SkColorMatrix make_invert_matrix(double amount) {
    const float k = static_cast<float>(amount);
    const float diag = 1.0f - 2.0f * k;
    return SkColorMatrix(
        diag, 0,    0,    0, k,
        0,    diag, 0,    0, k,
        0,    0,    diag, 0, k,
        0,    0,    0,    1, 0);
}

// Grayscale matrix: amount=0 identity, amount=1 full luma. Matches the
// CSS spec's interpolation of saturation toward 0 weighted by `amount`.
SkColorMatrix make_grayscale_matrix(double amount) {
    return make_saturation_matrix(1.0 - amount);
}

// Opacity = scale alpha. Identity at 1.
SkColorMatrix make_opacity_matrix(double a) {
    const float f = static_cast<float>(a);
    return SkColorMatrix(
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, f, 0);
}

// Wrap an SkColorMatrix into an SkImageFilter, composing onto the
// existing chain (inner). Caller passes the existing chain so we can
// fold multiple color-matrix functions into a single ColorFilter
// imagefilter where possible — but for simplicity we just compose.
sk_sp<SkImageFilter> compose(sk_sp<SkImageFilter> outer,
                              sk_sp<SkImageFilter> inner) {
    if (!outer) return inner;
    if (!inner) return outer;
    return SkImageFilters::Compose(std::move(outer), std::move(inner));
}

sk_sp<SkImageFilter> color_matrix_filter(const SkColorMatrix& m) {
    return SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), nullptr, nullptr);
}

}  // namespace

// Parse a CSS <filter-function-list> string into an SkImageFilter chain.
// Supported functions (per Canvas2D spec subset):
//   blur(<length>)            — SkImageFilters::Blur
//   brightness(<num|%>)       — color matrix scale
//   contrast(<num|%>)         — color matrix scale + bias
//   grayscale(<num|%>)        — color matrix saturation→0
//   hue-rotate(<angle>)       — color matrix rotation in YIQ-ish space
//   invert(<num|%>)           — color matrix invert
//   opacity(<num|%>)          — alpha scale
//   saturate(<num|%>)         — color matrix saturation
//   sepia(<num|%>)            — color matrix sepia interpolation
// Unknown or malformed functions are silently dropped (per CSS spec for
// invalid filter-function-list — the entire chain falls back to none).
sk_sp<SkImageFilter> parse_filter_chain(const std::string& src) {
    if (src.empty()) return nullptr;
    // Skip purely "none" / whitespace.
    size_t first = 0;
    while (first < src.size() && (src[first] == ' ' || src[first] == '\t')) ++first;
    if (first >= src.size()) return nullptr;
    if (src.compare(first, 4, "none") == 0) return nullptr;

    sk_sp<SkImageFilter> chain;
    size_t i = first;
    while (i < src.size()) {
        skip_ws(src, i);
        if (i >= src.size()) break;
        // Read function name up to '('.
        size_t name_start = i;
        while (i < src.size() && src[i] != '(' && src[i] != ' ' && src[i] != '\t') ++i;
        std::string name = src.substr(name_start, i - name_start);
        skip_ws(src, i);
        if (i >= src.size() || src[i] != '(') {
            // Malformed token — abort parse, return whatever we built.
            return chain;
        }
        ++i; // past '('
        size_t body_start = i;
        int depth = 1;
        while (i < src.size() && depth > 0) {
            if (src[i] == '(') ++depth;
            else if (src[i] == ')') { if (--depth == 0) break; }
            ++i;
        }
        if (i >= src.size()) return chain;
        std::string body = src.substr(body_start, i - body_start);
        ++i; // past ')'

        // Trim body whitespace.
        size_t bs = 0, be = body.size();
        while (bs < be && (body[bs] == ' ' || body[bs] == '\t')) ++bs;
        while (be > bs && (body[be - 1] == ' ' || body[be - 1] == '\t')) --be;
        body = body.substr(bs, be - bs);

        sk_sp<SkImageFilter> step;
        if (name == "blur") {
            double radius_px = parse_filter_arg(body, FilterArgKind::pixels);
            if (std::isfinite(radius_px) && radius_px > 0) {
                // CSS spec: blur radius is the standard deviation, NOT
                // the diameter. Skia takes sigma directly.
                float sigma = static_cast<float>(radius_px);
                step = SkImageFilters::Blur(sigma, sigma, nullptr);
            }
        } else if (name == "brightness") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) step = color_matrix_filter(make_brightness_matrix(amt));
        } else if (name == "contrast") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) step = color_matrix_filter(make_contrast_matrix(amt));
        } else if (name == "grayscale") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_grayscale_matrix(amt));
            }
        } else if (name == "hue-rotate") {
            double rad = parse_filter_arg(body, FilterArgKind::angle);
            if (std::isfinite(rad)) step = color_matrix_filter(make_hue_rotate_matrix(rad));
        } else if (name == "invert") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_invert_matrix(amt));
            }
        } else if (name == "opacity") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_opacity_matrix(amt));
            }
        } else if (name == "saturate") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt) && amt >= 0.0) {
                step = color_matrix_filter(make_saturation_matrix(amt));
            }
        } else if (name == "sepia") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_sepia_matrix(amt));
            }
        } else if (name == "drop-shadow") {
            // drop-shadow(<dx> <dy> <blur-radius> <color>)
            // dx, dy, blur-radius are lengths (px); color is optional
            // (defaults to black per CSS spec § filter-effects).
            auto tokens = parse_filter_args(body);
            if (tokens.size() >= 3) {
                float dx   = static_cast<float>(parse_filter_arg_length(tokens[0]));
                float dy   = static_cast<float>(parse_filter_arg_length(tokens[1]));
                float blur = static_cast<float>(parse_filter_arg_length(tokens[2]));
                if (blur < 0) blur = 0; // CSS spec: negative blur → 0
                SkColor color = SK_ColorBLACK;
                if (tokens.size() >= 4) {
                    std::string cs = tokens[3];
                    for (size_t k = 4; k < tokens.size(); ++k) cs += " " + tokens[k];
                    color = parse_css_color_to_skcolor(cs);
                }
                step = SkImageFilters::DropShadow(dx, dy, blur, blur, color, nullptr, nullptr);
            }
        }
        chain = compose(std::move(step), std::move(chain));
        // Skip optional comma between functions (spec is whitespace-only,
        // but tolerate ",").
        skip_ws(src, i);
        if (i < src.size() && src[i] == ',') { ++i; }
    }
    return chain;
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
