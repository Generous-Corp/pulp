// CSS border line styles: keyword parsing and the per-side band geometry and
// shades the double and 3D styles are painted with.
#include <pulp/view/border_style.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

std::optional<View::BorderStyle> border_style_from_css(std::string_view keyword) {
    using BorderStyle = View::BorderStyle;
    if (keyword == "solid")  return BorderStyle::solid;
    if (keyword == "dashed") return BorderStyle::dashed;
    if (keyword == "dotted") return BorderStyle::dotted;
    if (keyword == "double") return BorderStyle::double_;
    if (keyword == "groove") return BorderStyle::groove;
    if (keyword == "ridge")  return BorderStyle::ridge;
    if (keyword == "inset")  return BorderStyle::inset;
    if (keyword == "outset") return BorderStyle::outset;
    if (keyword == "none")   return BorderStyle::none;
    if (keyword == "hidden") return BorderStyle::hidden;
    return std::nullopt;
}

namespace {

// CSS groove, ridge, inset and outset borders are drawn in shades derived
// from the author's border colour. Nothing here is a theme colour: the
// constants below define that derivation, and every colour it builds is the
// author's colour scaled channel by channel.

// How far a shade moves the colour's brightest channel, on a 0..1 scale.
constexpr float kShadeStep = 0.33f;
// The lighter shade of pure black, which has no channel to scale.
constexpr float kBlackLightShade = 84.0f / 255.0f;
// Grey levels bounding the band in which a colour gets one lighter and one
// darker shade; darker colours get two lighter shades, lighter ones keep
// their own colour as the light shade.
constexpr float kDarkLimitGrey = 32.0f / 255.0f;
constexpr float kLightLimitGrey = 235.0f / 255.0f;

// One channel of a shade, quantized the way the reference quantizes it: the
// scaled value is floored onto the 8-bit grid. Rounding instead moves about a
// third of all shades by one level.
float shade_channel(float channel, float multiplier) {
    constexpr float kScale = 255.99998f;
    const float level = std::floor(channel * multiplier * kScale);
    return std::clamp(level, 0.0f, 255.0f) / 255.0f;
}

// `c` with every channel scaled by `multiplier` onto the 8-bit grid.
Color scaled_shade(Color c, float multiplier) {
    return Color::rgba(shade_channel(c.r, multiplier),  // token-lint:allow derived from the author's colour
                       shade_channel(c.g, multiplier),
                       shade_channel(c.b, multiplier), c.a);
}

// A neutral grey at `level` (0..1), keeping `alpha`.
Color grey(float level, float alpha) {
    return Color::rgba(level, level, level, alpha);  // token-lint:allow shade derivation constant
}

Color lighten_border_color(Color c) {
    const float v = std::max({c.r, c.g, c.b});
    if (v <= 0.0f) return grey(kBlackLightShade, c.a);
    return scaled_shade(c, std::min(1.0f, v + kShadeStep) / v);
}

Color darken_border_color(Color c) {
    const float v = std::max({c.r, c.g, c.b});
    if (v <= 0.0f) return grey(0.0f, c.a);
    return scaled_shade(c, std::max(0.0f, v - kShadeStep) / v);
}

float relative_luminance(Color c) {
    const auto linear = [](float channel) {
        return channel <= 0.04045f
                   ? channel / 12.92f
                   : std::pow((channel + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * linear(c.r) + 0.7152f * linear(c.g) +
           0.0722f * linear(c.b);
}

bool is_near_side(BorderEdge side) {
    return side == BorderEdge::top || side == BorderEdge::left;
}

}  // namespace

BorderShades border_shades(Color side_color) {
    static const float kDarkLimit =
        relative_luminance(grey(kDarkLimitGrey, 1.0f));
    static const float kLightLimit =
        relative_luminance(grey(kLightLimitGrey, 1.0f));
    const float luminance = relative_luminance(side_color);
    if (luminance <= kDarkLimit) {
        const Color base = lighten_border_color(side_color);
        return {lighten_border_color(base), base};
    }
    return {luminance > kLightLimit ? side_color
                                    : lighten_border_color(side_color),
            darken_border_color(side_color)};
}

BorderBands border_bands(View::BorderStyle style, BorderEdge side,
                         float width) {
    using BorderStyle = View::BorderStyle;
    BorderBands bands;
    const float w = std::max(0.0f, width);
    bands.outer = w;
    // A whole-pixel width splits on whole pixels, as the reference does; a
    // fractional one splits exactly.
    const bool whole = std::abs(w - std::round(w)) < 1e-3f;
    if ((style == BorderStyle::groove || style == BorderStyle::ridge) &&
        w >= 2.0f) {
        const float half = whole ? (is_near_side(side) ? std::ceil(w * 0.5f)
                                                       : std::floor(w * 0.5f))
                                 : w * 0.5f;
        bands.outer = half;
        bands.inner = w - half;
    } else if (style == BorderStyle::double_ && w >= 3.0f) {
        const float line = whole ? std::round(w / 3.0f) : w / 3.0f;
        bands.outer = line;
        bands.inner = line;
        bands.gap = w - 2.0f * line;
    }
    return bands;
}

Color border_band_color(View::BorderStyle style, BorderEdge side,
                        bool outer_band, Color side_color, float width) {
    using BorderStyle = View::BorderStyle;
    const auto shades = [&](bool lit) {
        const auto s = border_shades(side_color);
        return lit ? s.light : s.dark;
    };
    const bool near = is_near_side(side);
    switch (style) {
        case BorderStyle::outset: return shades(near);
        case BorderStyle::inset:  return shades(!near);
        case BorderStyle::groove:
        case BorderStyle::ridge: {
            if (width < 2.0f) return side_color;
            // Groove: the outer half is shaded as inset, the inner as outset.
            const bool outset_band =
                (style == BorderStyle::ridge) == outer_band;
            return shades(outset_band ? near : !near);
        }
        default: return side_color;
    }
}

}  // namespace pulp::view
