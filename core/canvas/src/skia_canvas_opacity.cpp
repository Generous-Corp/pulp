// skia_canvas_opacity.cpp — Canvas2D save_layer + filter composition slice.
//
// Extracted from skia_canvas.cpp in the 2026-05 Phase 4 (R2-3 follow-up)
// refactor. Covers the offscreen-layer / CSS opacity / CSS filter
// composition surface:
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
// pulp #1737 (Codex P2 sweep on #1791) — Skia headers MUST be included
// BEFORE pulp/canvas/skia_canvas.hpp.

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

namespace pulp::canvas {

void SkiaCanvas::set_opacity(float alpha) {
    // Note: set_opacity alone doesn't composite correctly for subtrees.
    // For proper CSS opacity, use save_layer() which creates an offscreen
    // buffer. This method exists for simple single-draw opacity.
    // The SkPaint alpha is applied per-draw, not per-subtree.
    (void)alpha; // Handled via save_layer in paint_all
}

void SkiaCanvas::save_layer(float x, float y, float w, float h,
                             float opacity, float blur_radius) {
    if (!canvas_) { save(); return; }

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint layer_paint;

    // Set layer opacity (composited when the layer is restored)
    if (opacity < 1.0f) {
        layer_paint.setAlphaf(opacity);
    }

    // Optionally apply blur as an image filter on the layer
    if (blur_radius > 0.0f) {
        layer_paint.setImageFilter(
            SkImageFilters::Blur(blur_radius, blur_radius, SkTileMode::kClamp, nullptr));
    }

    canvas_->saveLayer(&bounds, &layer_paint);

    // pulp #1899 (gap #3) — record that this layer's destination is
    // non-opaque so text-paint paths inside it use greyscale AA.
    if (opacity < 1.0f) {
        non_opaque_layer_stack_.push_back(canvas_->getSaveCount());
    }
}

// pulp #1549 — saveLayer with explicit blend mode. The layer-paint's blend
// mode is the one Skia uses when compositing the layer back onto its
// parent at restore() time, which is exactly the CSS / RN
// `mix-blend-mode` semantic ("isolate the subtree, then blend it back").
void SkiaCanvas::save_layer_with_blend(float x, float y, float w, float h,
                                       float opacity, float blur_radius,
                                       Canvas::BlendMode mode) {
    if (!canvas_) { save(); return; }

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint layer_paint;

    if (opacity < 1.0f) {
        layer_paint.setAlphaf(opacity);
    }
    if (blur_radius > 0.0f) {
        layer_paint.setImageFilter(
            SkImageFilters::Blur(blur_radius, blur_radius, SkTileMode::kClamp, nullptr));
    }
    if (mode != Canvas::BlendMode::normal) {
        layer_paint.setBlendMode(skia_blend_mode_for(mode));
    }

    canvas_->saveLayer(&bounds, &layer_paint);

    // pulp #1899 (gap #3) — mirror save_layer(): track non-opaque layer
    // so text inside it picks greyscale AA over LCD subpixel AA.
    if (opacity < 1.0f) {
        non_opaque_layer_stack_.push_back(canvas_->getSaveCount());
    }
}


// pulp #1434 Phase A2-4 — full CSS filter chain composition.
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
    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint layer_paint;

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
                // a (0..1). Codex P2 #3195880608: this MUST remain in
                // the composed chain at its original source-order
                // position, because subsequent filters (e.g. drop-shadow)
                // depend on the reduced alpha as their input. Folding
                // it into the layer alpha would apply opacity AFTER the
                // shadow was generated, which produces a different and
                // incorrect result for `opacity(0.5) drop-shadow(...)`.
                const float a = std::min(std::max(f.amount, 0.0f), 1.0f);
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
                // SkColorFilters::Matrix expects the translation column
                // in 0..255 space (Codex P1 #3195880597), so the bias
                // term is multiplied by 255 to land at mid-gray for
                // contrast(0). The slope multipliers stay normalized.
                const float c = f.amount;
                const float t = 0.5f * (1.0f - c) * 255.0f;
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
                // SkColorFilters::Matrix expects the translation column in
                // 0..255 space (Codex P1 #3195880597), so the bias term `a`
                // is multiplied by 255 to map black->white at invert(1).
                const float a = std::min(std::max(f.amount, 0.0f), 1.0f);
                const float k = 1.0f - 2.0f * a;
                const float t = a * 255.0f;
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

    if (composed) layer_paint.setImageFilter(composed);
    if (opacity < 1.0f) layer_paint.setAlphaf(opacity);

    canvas_->saveLayer(&bounds, &layer_paint);

    // pulp #1899 (gap #3) — track non-opaque destination for text-edging.
    if (opacity < 1.0f) {
        non_opaque_layer_stack_.push_back(canvas_->getSaveCount());
    }
}

void SkiaCanvas::save_backdrop_filter(float x, float y, float w, float h,
                                       float blur_radius) {
    // CSS `backdrop-filter: blur(N)` (issue-926). Push a layer whose initial
    // contents are the parent surface filtered through a Gaussian blur, so
    // subsequent draws into this layer composite over the blurred backdrop.
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
