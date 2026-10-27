#pragma once

// The colour space a gradient blends its stops in — CSS Color 4's
// `<color-interpolation-method>` (`linear-gradient(in oklab, ...)`).
//
// Kept apart from canvas.hpp so the enum can travel through the view and
// importer layers without pulling the whole Canvas surface along, and so the
// reference interpolation below has one home that backends and tests share.

#include <cstdint>
#include <vector>

namespace pulp::canvas {

struct Color;

/// Interpolation colour spaces a gradient can be painted in.
///
/// `xyz`, `xyz-d50`, `xyz-d65` and `display-p3-linear` have no member of their
/// own: each is a fixed linear transform of linear-light sRGB, and linear
/// interpolation commutes with a linear transform, so blending in any of them
/// gives exactly the colours `srgb_linear` gives. Chrome renders all four
/// identically to `in srgb-linear`.
enum class GradientColorSpace : std::uint8_t {
    srgb,        ///< gamma-encoded sRGB; the CSS default for legacy colours
    srgb_linear, ///< linear-light sRGB (and every space listed above)
    display_p3,
    a98_rgb,
    prophoto_rgb,
    rec2020,
    lab,   ///< CIE Lab, D50
    oklab, ///< the CSS default when a stop is a non-legacy colour
    hsl,
    hwb,
    lch, ///< CIE LCh, D50
    oklch,
};

/// Which way round the colour wheel a polar space travels between two hues.
/// Ignored for the rectangular spaces, where CSS forbids naming one.
enum class GradientHueMethod : std::uint8_t { shorter, longer, increasing, decreasing };

struct GradientInterpolation {
    GradientColorSpace space = GradientColorSpace::srgb;
    GradientHueMethod hue = GradientHueMethod::shorter;

    bool operator==(const GradientInterpolation&) const = default;
};

/// True for the cylindrical spaces, which interpolate a hue angle.
constexpr bool is_polar(GradientColorSpace space) {
    return space == GradientColorSpace::hsl || space == GradientColorSpace::hwb ||
           space == GradientColorSpace::lch || space == GradientColorSpace::oklch;
}

/// The sRGB colour `t` of the way from `a` to `b` when the two are blended in
/// `method`'s space, per CSS Color 4 §12: premultiplied by alpha, a powerless
/// hue (an achromatic stop) takes the other stop's hue, and the result is
/// clipped to the sRGB gamut, as Chrome clips it.
///
/// This is the reference the backends are held to. Skia interpolates natively;
/// a backend that blends only in sRGB approximates a curve by sampling this.
Color interpolate_gradient_color(const Color& a, const Color& b, float t,
                                 GradientInterpolation method);

/// Expand a stop list into one whose plain sRGB blend follows `method`'s curve:
/// every segment between two stops gains `samples_per_segment - 1` interior
/// stops taken from interpolate_gradient_color(). The default method returns
/// the stops unchanged. Positions must be non-decreasing, as a gradient's are.
void resample_gradient_stops(const Color* colors, const float* positions, int count,
                             GradientInterpolation method, int samples_per_segment,
                             std::vector<Color>& out_colors, std::vector<float>& out_positions);

} // namespace pulp::canvas
