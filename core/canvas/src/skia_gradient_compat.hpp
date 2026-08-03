#pragma once

#ifdef PULP_HAS_SKIA

#include "include/core/SkColorSpace.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkShader.h"
#include "include/core/SkTileMode.h"

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
#if PULP_SKIA_HAS_GRADIENT_OBJECT
inline SkGradient::Interpolation css_interpolation() {
    SkGradient::Interpolation interp;
    interp.fInPremul = SkGradient::Interpolation::InPremul::kYes;
    return interp;
}
#else
inline SkGradientShader::Interpolation css_interpolation() {
    SkGradientShader::Interpolation interp;
    interp.fInPremul = SkGradientShader::Interpolation::InPremul::kYes;
    return interp;
}
#endif

inline sk_sp<SkShader> make_linear(const SkPoint pts[2],
                                   const SkColor4f* colors,
                                   const float* positions,
                                   int count) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             SkTileMode::kClamp,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation());
    return SkShaders::LinearGradient(pts, grad);
#else
    return SkGradientShader::MakeLinear(pts, colors, SkColorSpace::MakeSRGB(),
                                        positions, count, SkTileMode::kClamp,
                                        css_interpolation(), nullptr);
#endif
}

inline sk_sp<SkShader> make_radial(SkPoint center,
                                   float radius,
                                   const SkColor4f* colors,
                                   const float* positions,
                                   int count) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             SkTileMode::kClamp,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation());
    return SkShaders::RadialGradient(center, radius, grad);
#else
    return SkGradientShader::MakeRadial(center, radius, colors, SkColorSpace::MakeSRGB(),
                                        positions, count, SkTileMode::kClamp,
                                        css_interpolation(), nullptr);
#endif
}

/// `tile` decides what happens OUTSIDE [start_degrees, end_degrees]. A full-turn
/// window never leaves anything outside, so kClamp is right for a plain conic;
/// a sub-turn window plus kRepeat is what makes a repeating-conic tile its band
/// around the circle, and Skia does the repetition itself.
inline sk_sp<SkShader> make_sweep(SkPoint center,
                                  float start_degrees,
                                  float end_degrees,
                                  const SkColor4f* colors,
                                  const float* positions,
                                  int count,
                                  SkTileMode tile = SkTileMode::kClamp) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             tile,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation());
    return SkShaders::SweepGradient(center, start_degrees, end_degrees, grad);
#else
    return SkGradientShader::MakeSweep(center.x(), center.y(), colors,
                                       SkColorSpace::MakeSRGB(), positions, count,
                                       tile, start_degrees, end_degrees,
                                       css_interpolation(), nullptr);
#endif
}

inline sk_sp<SkShader> make_two_point_conical(SkPoint start,
                                              float start_radius,
                                              SkPoint end,
                                              float end_radius,
                                              const SkColor4f* colors,
                                              const float* positions,
                                              int count) {
#if PULP_SKIA_HAS_GRADIENT_OBJECT
    SkGradient::Colors stops(SkSpan<const SkColor4f>(colors, static_cast<size_t>(count)),
                             positions ? SkSpan<const float>(positions, static_cast<size_t>(count))
                                       : SkSpan<const float>(),
                             SkTileMode::kClamp,
                             SkColorSpace::MakeSRGB());
    SkGradient grad(stops, css_interpolation());
    return SkShaders::TwoPointConicalGradient(start, start_radius, end, end_radius, grad);
#else
    return SkGradientShader::MakeTwoPointConical(start, start_radius, end, end_radius,
                                                 colors, SkColorSpace::MakeSRGB(),
                                                 positions, count, SkTileMode::kClamp,
                                                 css_interpolation(), nullptr);
#endif
}

} // namespace pulp::canvas::skia_gradient

#undef PULP_SKIA_HAS_GRADIENT_OBJECT

#endif // PULP_HAS_SKIA
