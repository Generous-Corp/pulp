// skia_canvas_opacity.cpp — Canvas2D save_layer + filter composition slice.
//
// Owns the Skia offscreen-layer / CSS opacity / CSS filter composition
// surface:
//
//   - set_opacity(alpha) — per-draw alpha shim (full opacity uses
//     save_layer instead).
//   - save_layer(x, y, w, h, alpha) — push a layer with whole-subtree
//     alpha so child draws composite as a unit.
//   - save_layer_with_blend(x, y, w, h, alpha, blend_mode) — same plus a
//     non-source-over blend that applies to the layer-on-parent composite.
//   - save_layer_with_filters(x, y, w, h, filter_chain) — CSS filter
//     chain. Walks blur / brightness / contrast / drop-shadow / hue-rotate /
//     invert / opacity / saturate / sepia / grayscale steps, composes them
//     as SkImageFilter, and pushes the layer with that filter as the
//     image-filter argument to saveLayer.
//   - save_backdrop_filter(x, y, w, h, blur_radius) — CSS backdrop-filter
//     blur — pushes a layer whose source is the parent-canvas content
//     pre-filtered, so subsequent draws composite over the blurred backdrop.
//
// Skia headers MUST be included BEFORE pulp/canvas/skia_canvas.hpp. See
// skia_canvas.cpp's head-of-file comment for the C++ name-lookup rule that
// forces this ordering.

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef PULP_HAS_SKIA

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkColorMatrix.h"
#include "include/effects/SkColorMatrixFilter.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#ifdef PULP_HAS_SKIA
#include "skia_canvas_internal.hpp"  // skia_blend_mode_for, to_sk_color4f
#include "named_shader_effects.hpp"  // make_named_shader_effect (curated SkSL)
#endif

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

void SkiaCanvas::set_opacity(float alpha) {
    // Note: set_opacity alone doesn't composite correctly for subtrees.
    // For proper CSS opacity, use save_layer() which creates an offscreen
    // buffer. This method exists for simple single-draw opacity.
    // The SkPaint alpha is applied per-draw, not per-subtree.
    (void)alpha; // Handled via save_layer in paint_all
}

// Shared saveLayer for every compositing-layer entry point. See the header for
// the parameter contract. Consolidates what were four near-identical bodies
// (opacity, blend, bloom, filter chain) and the SkSL post-effect.
void SkiaCanvas::push_layer(float x, float y, float w, float h,
                            float opacity, float blur_radius,
                            Canvas::BlendMode mode,
                            sk_sp<SkImageFilter> image_filter,
                            bool force_non_opaque) {
    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint layer_paint;
    if (opacity < 1.0f) {
        layer_paint.setAlphaf(opacity);
    }
    // A caller-supplied image filter (blur chain, bloom, runtime shader) takes
    // precedence; otherwise a plain blur radius becomes a Gaussian blur filter.
    if (image_filter) {
        layer_paint.setImageFilter(std::move(image_filter));
    } else if (blur_radius > 0.0f) {
        layer_paint.setImageFilter(
            SkImageFilters::Blur(blur_radius, blur_radius, SkTileMode::kClamp, nullptr));
    }
    if (mode != Canvas::BlendMode::normal) {
        layer_paint.setBlendMode(skia_blend_mode_for(mode));
    }
    canvas_->saveLayer(&bounds, &layer_paint);
    // Track non-opaque destinations so text-paint paths inside pick greyscale AA
    // over LCD subpixel AA (browser parity). A filter chain can reduce coverage
    // below opaque even with opacity == 1 (force_non_opaque).
    if (opacity < 1.0f || force_non_opaque) {
        non_opaque_layer_stack_.push_back(canvas_->getSaveCount());
    }
}

void SkiaCanvas::save_layer(float x, float y, float w, float h,
                             float opacity, float blur_radius) {
    if (!canvas_) { save(); return; }
    push_layer(x, y, w, h, opacity, blur_radius, Canvas::BlendMode::normal, nullptr);
}

// saveLayer with explicit blend mode. The layer-paint's blend mode is the one
// Skia uses when compositing the layer back onto its parent at restore() time,
// which is exactly the CSS / RN `mix-blend-mode` semantic ("isolate the
// subtree, then blend it back").
void SkiaCanvas::save_layer_with_blend(float x, float y, float w, float h,
                                       float opacity, float blur_radius,
                                       Canvas::BlendMode mode) {
    if (!canvas_) { save(); return; }
    push_layer(x, y, w, h, opacity, blur_radius, mode, nullptr);
}


// Real bloom / glow post-effect.
//
// Threshold the layer content (keep only pixels brighter than `threshold`,
// gained by `intensity`), blur the result, and additively composite (kPlus)
// that glow back over the original layer content — so bright regions bleed a
// halo beyond their edges. Restored by the matching restore() like any layer.
//
//   out_rgb   = clamp((in_rgb   - threshold) * g)      // thresholded highlight
//   out_alpha = clamp((luma(in) - threshold) * g)      // luma-gated so that
//                                                        // below-threshold
//                                                        // regions glow nothing
//   layer     = source + blur(out)                     // additive bloom
//
// Gating alpha by luminance (rather than leaving it identity) matters: a plain
// identity-alpha threshold matrix would spread a faint alpha halo from
// below-threshold regions even though their color is zero. SkColorFilters::
// Matrix operates on UNPREMUL color, and — verified by pixel readback in
// test_canvas.cpp — the translation column is in NORMALIZED [0,1] space in this
// Skia build, so the bias is -threshold*g (no 255 scale).
void SkiaCanvas::save_layer_with_bloom(float x, float y, float w, float h,
                                       float intensity, float threshold,
                                       float radius) {
    if (!canvas_) { save(); return; }

    const float denom = std::max(1.0f - threshold, 0.05f);
    const float g = intensity / denom;
    const float bias = -threshold * g;
    // Rec.709 luma weights for the alpha (glow) gate.
    const float lr = 0.2126f * g, lg = 0.7152f * g, lb = 0.0722f * g;
    float m[20] = {
        g,  0,  0,  0, bias,
        0,  g,  0,  0, bias,
        0,  0,  g,  0, bias,
        lr, lg, lb, 0, bias,
    };
    sk_sp<SkImageFilter> thresholded =
        SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), nullptr);
    sk_sp<SkImageFilter> glow = SkImageFilters::Blur(
        radius, radius, SkTileMode::kClamp, std::move(thresholded));
    sk_sp<SkImageFilter> bloom = SkImageFilters::Blend(
        SkBlendMode::kPlus, /*background=*/nullptr, /*foreground=*/std::move(glow));

    push_layer(x, y, w, h, /*opacity=*/1.0f, /*blur=*/0.0f,
               Canvas::BlendMode::normal, std::move(bloom));
}

// Full CSS filter chain composition.
//
// Builds an SkImageFilter chain from the structured FilterChainEntry
// list. Color-matrix-based filters (brightness / contrast / grayscale
// / hue-rotate / invert / saturate / sepia) all reduce to an
// SkColorMatrix wrapped via SkImageFilters::ColorFilter, then composed
// in order via SkImageFilters::Compose. Blur and drop-shadow are
// independent SkImageFilter primitives composed into the same chain.
// The `opacity()` filter function affects the layer alpha rather than
// a color matrix (matches how CSS treats it — multiplicative on the
// already-composited layer).
void SkiaCanvas::save_layer_with_filters(float x, float y, float w, float h,
                                          float opacity,
                                          const FilterChainEntry* chain,
                                          int count) {
    if (!canvas_) { save(); return; }

    // Walk the chain. Build a single composed image filter per CSS
    // semantics: filters are applied in source order, so chain[0] is
    // the inner-most input to chain[1], etc.
    sk_sp<SkImageFilter> composed;
    auto compose = [&composed](sk_sp<SkImageFilter> next) {
        if (!next) return;
        composed = composed
            ? SkImageFilters::Compose(std::move(next), std::move(composed))
            : std::move(next);
    };

    // A filter chain can drive the layer's destination below full opacity even
    // when the `opacity` parameter is 1 — a `Kind::opacity` entry reduces alpha
    // via a color matrix INSIDE the composed filter, and drop-shadow adds
    // partially-transparent pixels. Text painted into such a layer needs
    // greyscale AA (not LCD subpixel), same as a plain opacity layer. Track it
    // so push_layer marks the layer non-opaque. Without this, glyphs inside
    // `filter: opacity(0.5)` render LCD-fringed while plain `opacity: 0.5` is
    // correct.
    bool reduces_coverage = false;

    for (int i = 0; i < count; ++i) {
        const FilterChainEntry& f = chain[i];
        switch (f.kind) {
            case FilterChainEntry::Kind::blur: {
                if (f.amount > 0.0f) {
                    compose(SkImageFilters::Blur(f.amount, f.amount,
                                                 SkTileMode::kClamp, nullptr));
                }
                break;
            }
            case FilterChainEntry::Kind::opacity: {
                // Per CSS — opacity(a) multiplies the alpha channel by
                // a (0..1). This MUST remain in the composed chain at its
                // original source-order position, because subsequent filters
                // (e.g. drop-shadow) depend on the reduced alpha as their
                // input. Folding it into the layer alpha would apply opacity
                // AFTER the shadow was generated, which produces a different
                // and incorrect result for `opacity(0.5) drop-shadow(...)`.
                const float a = std::min(std::max(f.amount, 0.0f), 1.0f);
                if (a < 1.0f) reduces_coverage = true;
                float m[20] = {
                    1, 0, 0, 0, 0,
                    0, 1, 0, 0, 0,
                    0, 0, 1, 0, 0,
                    0, 0, 0, a, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::brightness: {
                // Per CSS spec — RGB scaled, alpha untouched.
                const float k = f.amount;
                float m[20] = {
                    k, 0, 0, 0, 0,
                    0, k, 0, 0, 0,
                    0, 0, k, 0, 0,
                    0, 0, 0, 1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::contrast: {
                // Per CSS — c=amount, slope=c, intercept=0.5*(1-c).
                // SkColorFilters::Matrix's translation column is NORMALIZED
                // [0,1] (not 0..255 — verified by pixel readback here and in the
                // bloom path), so the bias is 0.5*(1-c). contrast(0) → 0.5 on
                // every channel = mid-gray. (A prior *255 scale blew this out to
                // white, masked by a [!mayfail] tag.)
                const float c = f.amount;
                const float t = 0.5f * (1.0f - c);
                float m[20] = {
                    c, 0, 0, 0, t,
                    0, c, 0, 0, t,
                    0, 0, c, 0, t,
                    0, 0, 0, 1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::grayscale: {
                // Per CSS spec table — blends towards luminance-weighted gray.
                // amount=1 is fully gray; amount=0 is identity.
                const float a = std::min(std::max(f.amount, 0.0f), 1.0f);
                const float r = 0.2126f, g = 0.7152f, b = 0.0722f;
                float m[20] = {
                    1 - a + a * r, a * g,         a * b,         0, 0,
                    a * r,         1 - a + a * g, a * b,         0, 0,
                    a * r,         a * g,         1 - a + a * b, 0, 0,
                    0,             0,             0,             1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::saturate: {
                // Per CSS spec — saturate(0) is fully gray, saturate(1) is identity.
                // Same matrix family as grayscale but with amount inverted.
                const float a = f.amount;
                const float r = 0.2126f, g = 0.7152f, b = 0.0722f;
                const float inv_a = 1.0f - a;
                float m[20] = {
                    a + inv_a * r, inv_a * g,    inv_a * b,    0, 0,
                    inv_a * r,    a + inv_a * g, inv_a * b,    0, 0,
                    inv_a * r,    inv_a * g,    a + inv_a * b, 0, 0,
                    0,            0,            0,             1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::invert: {
                // Per CSS spec — amount=1 fully inverts, amount=0 is identity.
                // SkColorFilters::Matrix's translation column is NORMALIZED
                // [0,1], so the bias is `a` (not a*255). invert(1): k=-1, t=1 →
                // black(0)→1=white, white(1)→-1+1=0=black. (A prior *255 scale
                // left white uninverted; black→white worked only by clamp
                // accident, which is why the [!mayfail] black-only test passed.)
                const float a = std::min(std::max(f.amount, 0.0f), 1.0f);
                const float k = 1.0f - 2.0f * a;
                const float t = a;
                float m[20] = {
                    k, 0, 0, 0, t,
                    0, k, 0, 0, t,
                    0, 0, k, 0, t,
                    0, 0, 0, 1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::sepia: {
                // Per CSS spec table — sepia(amount) blends with sepia tone.
                const float a = std::min(std::max(f.amount, 0.0f), 1.0f);
                // Identity matrix interpolated towards the sepia matrix.
                auto lerp = [a](float ident, float sepia_v) {
                    return ident + a * (sepia_v - ident);
                };
                float m[20] = {
                    lerp(1, 0.393f), lerp(0, 0.769f), lerp(0, 0.189f), 0, 0,
                    lerp(0, 0.349f), lerp(1, 0.686f), lerp(0, 0.168f), 0, 0,
                    lerp(0, 0.272f), lerp(0, 0.534f), lerp(1, 0.131f), 0, 0,
                    0,               0,               0,                1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::hue_rotate: {
                // Per CSS spec — rotation around the achromatic axis in YIQ.
                // Standard 3x3 hue-rotation matrix expressed as 4x5 RGB.
                const float deg = f.angle_deg;
                const float rad = deg * 3.14159265358979323846f / 180.0f;
                const float cos_h = std::cos(rad);
                const float sin_h = std::sin(rad);
                // Constants from the CSS Filter Effects spec, Appendix A.
                float m[20] = {
                    0.213f + cos_h * 0.787f - sin_h * 0.213f,
                    0.715f - cos_h * 0.715f - sin_h * 0.715f,
                    0.072f - cos_h * 0.072f + sin_h * 0.928f,
                    0, 0,

                    0.213f - cos_h * 0.213f + sin_h * 0.143f,
                    0.715f + cos_h * 0.285f + sin_h * 0.140f,
                    0.072f - cos_h * 0.072f - sin_h * 0.283f,
                    0, 0,

                    0.213f - cos_h * 0.213f - sin_h * 0.787f,
                    0.715f - cos_h * 0.715f + sin_h * 0.715f,
                    0.072f + cos_h * 0.928f + sin_h * 0.072f,
                    0, 0,

                    0, 0, 0, 1, 0,
                };
                compose(SkImageFilters::ColorFilter(
                    SkColorFilters::Matrix(m), nullptr));
                break;
            }
            case FilterChainEntry::Kind::drop_shadow: {
                // Per CSS spec — drop-shadow renders an offset blurred
                // copy of the layer alpha tinted to ds_color, composited
                // BELOW the original. SkImageFilters::DropShadow wraps
                // the input filter so we feed it the chain so far as
                // input — composes naturally with prior color matrices.
                SkColor color = SkColorSetARGB(
                    f.ds_color.a8(), f.ds_color.r8(),
                    f.ds_color.g8(), f.ds_color.b8());
                sk_sp<SkImageFilter> input = composed; // chain so far as input
                composed = SkImageFilters::DropShadow(
                    f.ds_offset_x, f.ds_offset_y,
                    f.ds_blur, f.ds_blur,
                    color,
                    std::move(input));
                break;
            }
        }
    }

    push_layer(x, y, w, h, opacity, /*blur=*/0.0f, Canvas::BlendMode::normal,
               std::move(composed), /*force_non_opaque=*/reduces_coverage);
}

// Curated named GPU post-effect (crt / grain / vignette / noise / brushed /
// bloom). Resolves the name to a vetted SkSL runtime shader and applies it as
// an SkImageFilters::RuntimeShader over the whole compositing layer. Safe by
// construction: generated UIs pick an effect by NAME with a single clamped
// intensity — never arbitrary shader source. An unknown name or an SkSL
// compile failure yields a null filter, so the layer opens plain and the
// effect is simply skipped (graceful no-op, never a hard error).
void SkiaCanvas::save_layer_with_shader_effect(float x, float y,
                                               float w, float h,
                                               const std::string& effect_name,
                                               float intensity) {
    if (!canvas_) { save(); return; }
    // The RuntimeShader image filter runs in the layer's DEVICE space, so its
    // main(float2 coord) sees device pixels. Feed the effect the device-space
    // resolution (logical size x CTM scale) so position-dependent effects
    // (vignette centering, CRT curvature, bloom kernel radius) stay correct
    // under HiDPI / any canvas scale. At DPR=1 this is a no-op.
    const SkMatrix m = canvas_->getTotalMatrix();
    const float sx = std::hypot(m.getScaleX(), m.getSkewY());
    const float sy = std::hypot(m.getSkewX(), m.getScaleY());
    const float dw = w * (sx > 0.0f ? sx : 1.0f);
    const float dh = h * (sy > 0.0f ? sy : 1.0f);
    sk_sp<SkImageFilter> fx =
        make_named_shader_effect(effect_name, intensity, dw, dh, /*time=*/0.0f);
    // Confine the effect to the layer rect. A RuntimeShader filter reports
    // UNBOUNDED output — it can write at any coordinate, whether or not the
    // child drew there — so saveLayer treats its bounds argument as a hint and
    // grows the layer to the device clip. An effect that writes opaque pixels
    // where the child is transparent then composites over everything already
    // painted behind this canvas, which reads as the whole window going black
    // behind one small screen.
    //
    // Today only `crt` actually does that (its scanlines and aperture mask
    // write regardless of child alpha); the other five happen to preserve the
    // child's transparency. That is the reason to crop rather than to fix one
    // shader: containment must not depend on every present and future curated
    // effect remembering to preserve alpha. Do not read this as redundant when
    // adding effect number seven.
    //
    // `bloom` is the case that looks like it wants the opposite, since its
    // purpose is to spread light past its source — but a layer cannot paint
    // outside itself in the first place (saveLayer already clips layer CONTENT
    // to `bounds`). Without the crop, a bright element at the widget edge threw
    // a HARD-EDGED band of uniform colour ~8px past the boundary that stopped
    // abruptly instead of falling off — the unbounded-output artifact again,
    // not a glow. Cropping removes that band and changes nothing inside. So do
    // not widen the crop for bloom; that would quietly restore the overspill.
    //
    // kDecal (Crop's default) is transparent outside the rect, so the
    // surrounding content shows through untouched. The rect is in the same
    // local space as the layer bounds.
    if (fx) fx = SkImageFilters::Crop(SkRect::MakeXYWH(x, y, w, h), std::move(fx));
    // A runtime-shader post-effect can drop coverage below opaque, so mark the
    // layer non-opaque when an effect is actually active (matches the
    // filter-chain path) for correct greyscale text AA inside the layer.
    const bool active = static_cast<bool>(fx);
    push_layer(x, y, w, h, /*opacity=*/1.0f, /*blur=*/0.0f,
               Canvas::BlendMode::normal, std::move(fx),
               /*force_non_opaque=*/active);
}

void SkiaCanvas::save_backdrop_filter(float x, float y, float w, float h,
                                       float blur_radius) {
    // CSS `backdrop-filter: blur(N)`. Push a layer whose initial contents are
    // the parent surface filtered through a Gaussian blur, so subsequent draws
    // into this layer composite over the blurred backdrop.
    if (!canvas_) { save(); return; }
    if (blur_radius <= 0.0f) {
        // Degenerate: behave like a plain save() so the matching restore()
        // stays balanced and the View::paint_all bookkeeping is unaffected.
        canvas_->save();
        return;
    }

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    auto backdrop = SkImageFilters::Blur(blur_radius, blur_radius,
                                         SkTileMode::kClamp, nullptr);

    SkCanvas::SaveLayerRec rec(&bounds, /*paint=*/nullptr, backdrop.get(), 0);
    canvas_->saveLayer(rec);
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
