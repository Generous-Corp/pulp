// CSS border line styles: keyword parsing and the per-side band geometry and
// shades the double and 3D styles are painted with.
#include <pulp/view/border_style.hpp>
#include <pulp/view/theme_contrast.hpp>

#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <array>
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

bool is_banded_border_style(View::BorderStyle style) {
    using BorderStyle = View::BorderStyle;
    return style == BorderStyle::double_ || style == BorderStyle::groove ||
           style == BorderStyle::ridge || style == BorderStyle::inset ||
           style == BorderStyle::outset;
}

CornerRadii fit_corner_radii(float width, float height, CornerRadii r) {
    for (float* v : {&r.tl_x, &r.tl_y, &r.tr_x, &r.tr_y, &r.br_x, &r.br_y,
                     &r.bl_x, &r.bl_y})
        *v = std::max(0.0f, *v);
    // Double precision, so a uniform overflowing radius lands exactly on half
    // the side instead of an ulp either side of it.
    double f = 1.0;
    const auto limit = [&f](float side, float a, float b) {
        const double sum = static_cast<double>(a) + static_cast<double>(b);
        if (sum > static_cast<double>(side))
            f = std::min(f, std::max(0.0, static_cast<double>(side)) / sum);
    };
    limit(width, r.tl_x, r.tr_x);
    limit(width, r.bl_x, r.br_x);
    limit(height, r.tl_y, r.bl_y);
    limit(height, r.tr_y, r.br_y);
    if (f < 1.0) {
        for (float* v : {&r.tl_x, &r.tl_y, &r.tr_x, &r.tr_y, &r.br_x, &r.br_y,
                         &r.bl_x, &r.bl_y})
            *v = static_cast<float>(static_cast<double>(*v) * f);
    }
    return r;
}

namespace {

/// A rectangle given by its insets from the border box.
struct Insets {
    float top = 0.0f, right = 0.0f, bottom = 0.0f, left = 0.0f;
};

/// Add a closed rounded-rectangle subpath for the border box inset by `in`,
/// clockwise. Each corner radius is the fitted outer radius `outer` shrunk by
/// the inset on its own axis, so unequal side widths give the elliptical inner
/// corner CSS does, then fitted again to the inset box. Built from cubics,
/// which every backend draws.
void add_inset_rounded_rect(canvas::Canvas& canvas, const BandedBorder& b,
                            const CornerRadii& outer, const Insets& in) {
    const float x0 = in.left, y0 = in.top;
    const float x1 = b.width - in.right, y1 = b.height - in.bottom;
    if (x1 <= x0 || y1 <= y0) return;
    const auto shrink = [](float r, float inset) {
        return std::max(0.0f, r - inset);
    };
    const CornerRadii inner = fit_corner_radii(
        x1 - x0, y1 - y0,
        {shrink(outer.tl_x, in.left), shrink(outer.tl_y, in.top),
         shrink(outer.tr_x, in.right), shrink(outer.tr_y, in.top),
         shrink(outer.br_x, in.right), shrink(outer.br_y, in.bottom),
         shrink(outer.bl_x, in.left), shrink(outer.bl_y, in.bottom)});
    const float tlx = inner.tl_x, tly = inner.tl_y;
    const float trx = inner.tr_x, try_ = inner.tr_y;
    const float brx = inner.br_x, bry = inner.br_y;
    const float blx = inner.bl_x, bly = inner.bl_y;
    constexpr float k = 0.5522847498f;  // cubic quarter-ellipse handle
    canvas.move_to(x0 + tlx, y0);
    canvas.line_to(x1 - trx, y0);
    if (trx > 0.0f && try_ > 0.0f)
        canvas.cubic_to(x1 - trx + trx * k, y0, x1, y0 + try_ - try_ * k, x1,
                        y0 + try_);
    canvas.line_to(x1, y1 - bry);
    if (brx > 0.0f && bry > 0.0f)
        canvas.cubic_to(x1, y1 - bry + bry * k, x1 - brx + brx * k, y1,
                        x1 - brx, y1);
    canvas.line_to(x0 + blx, y1);
    if (blx > 0.0f && bly > 0.0f)
        canvas.cubic_to(x0 + blx - blx * k, y1, x0, y1 - bly + bly * k, x0,
                        y1 - bly);
    canvas.line_to(x0, y0 + tly);
    if (tlx > 0.0f && tly > 0.0f)
        canvas.cubic_to(x0, y0 + tly - tly * k, x0 + tlx - tlx * k, y0,
                        x0 + tlx, y0);
    canvas.close_path();
}

/// Where the mitre line from an outer box corner ends: past the padding-box
/// corner by the corner radius, so the wedge it bounds covers the curved
/// pocket a rounded inner edge leaves inside the padding-box corner. It stops
/// at the box's centre lines, so the four wedges cannot cross, unless a side
/// is wider than half the box and its padding-box corner is already past them.
struct MitrePoint { float x = 0.0f, y = 0.0f; };

MitrePoint mitre_end(float cx, float cy, float dx, float dy, float side_x,
                float side_y, float radius, float w, float h) {
    float ux = side_x, uy = side_y;
    if (ux <= 0.0f && uy <= 0.0f) ux = uy = 1.0f;
    float reach = 1.0f + std::max(0.0f, radius) / std::max(ux, uy);
    if (ux > 0.0f) reach = std::min(reach, std::max(1.0f, 0.5f * w / ux));
    if (uy > 0.0f) reach = std::min(reach, std::max(1.0f, 0.5f * h / uy));
    return {std::clamp(cx + dx * ux * reach, 0.0f, w),
            std::clamp(cy + dy * uy * reach, 0.0f, h)};
}

}  // namespace

void paint_banded_border(canvas::Canvas& canvas, const BandedBorder& b) {
    const float w = b.width, h = b.height;
    if (w <= 0.0f || h <= 0.0f) return;
    const BorderBands bands[4] = {
        border_bands(b.style, BorderEdge::top, b.top),
        border_bands(b.style, BorderEdge::right, b.right),
        border_bands(b.style, BorderEdge::bottom, b.bottom),
        border_bands(b.style, BorderEdge::left, b.left),
    };
    const float widths[4] = {b.top, b.right, b.bottom, b.left};
    const Color colors[4] = {b.top_color, b.right_color, b.bottom_color,
                             b.left_color};
    const BorderEdge edges[4] = {BorderEdge::top, BorderEdge::right,
                                 BorderEdge::bottom, BorderEdge::left};

    // The outer corners, fitted to the border box as the background is.
    const CornerRadii radii = fit_corner_radii(
        w, h, CornerRadii::circular(b.radius_tl, b.radius_tr, b.radius_br,
                                    b.radius_bl));

    // The four mitre lines, outer corner → beyond the padding-box corner.
    const float max_r = std::max({radii.tl_x, radii.tl_y, radii.tr_x,
                                  radii.tr_y, radii.br_x, radii.br_y,
                                  radii.bl_x, radii.bl_y});
    const MitrePoint tl = mitre_end(0, 0, 1, 1, b.left, b.top, max_r, w, h);
    const MitrePoint tr = mitre_end(w, 0, -1, 1, b.right, b.top, max_r, w, h);
    const MitrePoint br = mitre_end(w, h, -1, -1, b.right, b.bottom, max_r, w, h);
    const MitrePoint bl = mitre_end(0, h, 1, -1, b.left, b.bottom, max_r, w, h);
    const std::array<std::array<MitrePoint, 4>, 4> wedges = {{
        {{{0, 0}, {w, 0}, tr, tl}},
        {{{w, 0}, {w, h}, br, tr}},
        {{{w, h}, {0, h}, bl, br}},
        {{{0, h}, {0, 0}, tl, bl}},
    }};

    const auto paint_band = [&](const Insets& outer, const Insets& inner,
                                bool outer_band) {
        Color band_color[4];
        bool present[4];
        for (int i = 0; i < 4; ++i) {
            band_color[i] = border_band_color(b.style, edges[i], outer_band,
                                              colors[i], widths[i]);
            present[i] = widths[i] > 0.0f;
        }
        const auto ring = [&] {
            canvas.begin_path();
            add_inset_rounded_rect(canvas, b, radii, outer);
            add_inset_rounded_rect(canvas, b, radii, inner);
            canvas.fill_current_path(canvas::FillRule::evenodd);
        };
        bool done[4] = {false, false, false, false};
        for (int i = 0; i < 4; ++i) {
            if (done[i] || !present[i]) continue;
            // Every side painting this colour, filled through one clip so the
            // mitres between them are interior to the shape.
            bool group[4] = {false, false, false, false};
            int count = 0, present_count = 0;
            for (int j = 0; j < 4; ++j) {
                if (!present[j]) continue;
                ++present_count;
                if (band_color[j] == band_color[i]) {
                    group[j] = done[j] = true;
                    ++count;
                }
            }
            canvas.set_fill_color(band_color[i]);
            if (count == present_count) {
                ring();
                continue;
            }
            canvas.save();
            canvas.begin_path();
            for (int j = 0; j < 4; ++j) {
                if (!group[j]) continue;
                const auto& q = wedges[static_cast<size_t>(j)];
                canvas.move_to(q[0].x, q[0].y);
                for (int k = 1; k < 4; ++k) canvas.line_to(q[k].x, q[k].y);
                canvas.close_path();
            }
            canvas.clip();
            ring();
            canvas.restore();
        }
    };

    const Insets outer_edge{};
    const Insets outer_band{bands[0].outer, bands[1].outer, bands[2].outer,
                            bands[3].outer};
    paint_band(outer_edge, outer_band, true);
    const Insets inner_band_start{b.top - bands[0].inner,
                                  b.right - bands[1].inner,
                                  b.bottom - bands[2].inner,
                                  b.left - bands[3].inner};
    const Insets padding_edge{b.top, b.right, b.bottom, b.left};
    if (bands[0].inner > 0.0f || bands[1].inner > 0.0f ||
        bands[2].inner > 0.0f || bands[3].inner > 0.0f)
        paint_band(inner_band_start, padding_edge, false);
}

}  // namespace pulp::view
