#pragma once

#ifdef PULP_HAS_SKIA

#include "include/core/SkColorSpace.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"

#include <pulp/canvas/gradient_interpolation.hpp>

#if __has_include("include/effects/SkGradient.h")
#include "include/core/SkSpan.h"
#include "include/effects/SkGradient.h"
#define PULP_SKIA_HAS_GRADIENT_OBJECT 1
#else
#include "include/effects/SkGradientShader.h"
#define PULP_SKIA_HAS_GRADIENT_OBJECT 0
#endif

namespace pulp::canvas::skia_gradient {

// CSS interpolates gradient colours in PREMULTIPLIED space (css-images-3
// §3.4.2). Skia defaults to unpremultiplied, which drags the colour toward a
// transparent stop's RGB as the alpha falls — and `transparent` serializes to
// `rgba(0, 0, 0, 0)`, so a fade-out greys out instead of just fading. Opaque
// gradients are unaffected, which is why the default went unnoticed: only a
// stop with alpha < 1 can tell the two apart.
//
// The colour space is the CSS `<color-interpolation-method>`. Every space Pulp
// accepts has a Skia counterpart, so nothing is approximated here. sRGB stays
// kDestination, Skia's default, so a gradient that names no space paints the
// same pixels it always has. OKLab and OKLCH map to the plain spaces rather
// than Skia's experimental gamut-mapped variants: Chrome clips an out-of-gamut
// blend, and `in oklch longer hue` from red to blue reads (0, 147, 0) there.
#if PULP_SKIA_HAS_GRADIENT_OBJECT
using SkInterpolation = SkGradient::Interpolation;
#else
using SkInterpolation = SkGradientShader::Interpolation;
#endif

inline SkInterpolation css_interpolation(canvas::GradientInterpolation method = {}) {
    using Space = SkInterpolation::ColorSpace;
    using Hue = SkInterpolation::HueMethod;
    SkInterpolation interp;
    interp.fInPremul = SkInterpolation::InPremul::kYes;
    switch (method.space) {
    case canvas::GradientColorSpace::srgb:
        interp.fColorSpace = Space::kDestination;
        break;
    case canvas::GradientColorSpace::srgb_linear:
        interp.fColorSpace = Space::kSRGBLinear;
        break;
    case canvas::GradientColorSpace::display_p3:
        interp.fColorSpace = Space::kDisplayP3;
        break;
    case canvas::GradientColorSpace::a98_rgb:
        interp.fColorSpace = Space::kA98RGB;
        break;
    case canvas::GradientColorSpace::prophoto_rgb:
        interp.fColorSpace = Space::kProphotoRGB;
        break;
    case canvas::GradientColorSpace::rec2020:
        interp.fColorSpace = Space::kRec2020;
        break;
    case canvas::GradientColorSpace::lab:
        interp.fColorSpace = Space::kLab;
        break;
    case canvas::GradientColorSpace::oklab:
        interp.fColorSpace = Space::kOKLab;
        break;
    case canvas::GradientColorSpace::hsl:
        interp.fColorSpace = Space::kHSL;
        break;
    case canvas::GradientColorSpace::hwb:
        interp.fColorSpace = Space::kHWB;
        break;
    case canvas::GradientColorSpace::lch:
        interp.fColorSpace = Space::kLCH;
        break;
    case canvas::GradientColorSpace::oklch:
        interp.fColorSpace = Space::kOKLCH;
        break;
    }
    switch (method.hue) {
    case canvas::GradientHueMethod::shorter:
        interp.fHueMethod = Hue::kShorter;
        break;
    case canvas::GradientHueMethod::longer:
        interp.fHueMethod = Hue::kLonger;
        break;
    case canvas::GradientHueMethod::increasing:
        interp.fHueMethod = Hue::kIncreasing;
        break;
    case canvas::GradientHueMethod::decreasing:
        interp.fHueMethod = Hue::kDecreasing;
        break;
    }
    return interp;
}

/// `tile` decides what happens OUTSIDE the span pts[0]..pts[1]. A span that
/// already covers the box leaves nothing outside, so kClamp is right for a
/// plain linear; a one-band span plus kRepeat is what makes a repeating-linear
/// tile its band along the line, and Skia does the repetition itself.
inline sk_sp<SkShader> make_linear(const SkPoint pts[2], const SkColor4f* colors,
                                   const float* positions, int count,
                                   SkTileMode tile = SkTileMode::kClamp,
                                   canvas::GradientInterpolation method = {}) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             tile,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation(method));
    return SkShaders::LinearGradient(pts, grad);
#else
    return SkGradientShader::MakeLinear(pts, colors, SkColorSpace::MakeSRGB(), positions, count,
                                        tile, css_interpolation(method), nullptr);
#endif
}

inline sk_sp<SkShader> make_radial(SkPoint center, float radius, const SkColor4f* colors,
                                   const float* positions, int count,
                                   canvas::GradientInterpolation method = {}) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             SkTileMode::kClamp,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation(method));
    return SkShaders::RadialGradient(center, radius, grad);
#else
    return SkGradientShader::MakeRadial(center, radius, colors, SkColorSpace::MakeSRGB(), positions,
                                        count, SkTileMode::kClamp, css_interpolation(method),
                                        nullptr);
#endif
}

/// `tile` decides what happens OUTSIDE [start_degrees, end_degrees]. A full-turn
/// window never leaves anything outside, so kClamp is right for a plain conic;
/// a sub-turn window plus kRepeat is what makes a repeating-conic tile its band
/// around the circle, and Skia does the repetition itself.
inline sk_sp<SkShader> make_sweep(SkPoint center, float start_degrees, float end_degrees,
                                  const SkColor4f* colors, const float* positions, int count,
                                  SkTileMode tile = SkTileMode::kClamp,
                                  canvas::GradientInterpolation method = {}) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             tile,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation(method));
    return SkShaders::SweepGradient(center, start_degrees, end_degrees, grad);
#else
    return SkGradientShader::MakeSweep(center.x(), center.y(), colors, SkColorSpace::MakeSRGB(),
                                       positions, count, tile, start_degrees, end_degrees,
                                       css_interpolation(method), nullptr);
#endif
}

inline sk_sp<SkShader> make_two_point_conical(SkPoint start, float start_radius, SkPoint end,
                                              float end_radius, const SkColor4f* colors,
                                              const float* positions, int count,
                                              canvas::GradientInterpolation method = {}) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             SkTileMode::kClamp,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation(method));
    return SkShaders::TwoPointConicalGradient(start, start_radius, end, end_radius, grad);
#else
    return SkGradientShader::MakeTwoPointConical(
        start, start_radius, end, end_radius, colors, SkColorSpace::MakeSRGB(), positions, count,
        SkTileMode::kClamp, css_interpolation(method), nullptr);
#endif
}

/// Install a fill or stroke shader, dithering it when it is a gradient.
///
/// Chromium dithers CSS gradients, so a shallow gradient (a few code values
/// across hundreds of pixels) reaches the screen as fine row noise rather than
/// flat bands with hard steps. Undithered, the same gradient bands visibly and
/// disagrees with every browser capture of it. Image patterns share the same
/// shader slots and are left undithered, as Chromium leaves images.
inline void set_paint_shader(SkPaint& paint, sk_sp<SkShader> shader) {
    paint.setDither(shader != nullptr && !shader->isAImage());
    paint.setShader(std::move(shader));
}

} // namespace pulp::canvas::skia_gradient

#undef PULP_SKIA_HAS_GRADIENT_OBJECT

#endif // PULP_HAS_SKIA
