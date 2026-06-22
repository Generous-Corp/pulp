// skia_canvas_box_shadow.cpp — Canvas2D / CSS box-shadow paint slice.
//
// Implements `draw_box_shadow(x, y, w, h, dx, dy, blur, spread, color,
// inset, corner_radius)` for CSS-compatible shadows: outset shadows paint
// outside the rect via mask-filter blur; inset shadows paint inside via
// clip + reverse-difference.
//
// Skia headers MUST be included BEFORE pulp/canvas/skia_canvas.hpp. See
// skia_canvas.cpp's head-of-file comment for the C++ name-lookup rule that
// forces this ordering.

#include <algorithm>
#include <cmath>

#ifdef PULP_HAS_SKIA

#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
// Box-shadow uses SkImageFilters::DropShadowOnly + SkImageFilters::Blur.
#include "include/effects/SkImageFilters.h"

#endif  // PULP_HAS_SKIA

#include <pulp/canvas/box_shadow_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#ifdef PULP_HAS_SKIA
#include "skia_canvas_internal.hpp"
#endif

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

// ── Box shadow ──────────────────────────────────────────────────────────────

void SkiaCanvas::draw_box_shadow(float x, float y, float w, float h,
                                  float dx, float dy, float blur, float spread,
                                  Color color, bool inset, float corner_radius) {
    if (!canvas_) return;
    if (color.a <= 0.0f || (w <= 0.0f && h <= 0.0f)) return;

    // Skia's drop-shadow blur sigma is roughly half the CSS blur radius;
    // matching the WebView 1:1 within ~5px is the acceptance criterion, so
    // we use blur/2.0 the way Chromium's compositor does.
    const float sigma = std::max(0.0f, blur * 0.5f);

    if (!inset) {
        // Cached path: the blurred coverage shape is origin-agnostic (depends on
        // size/blur/spread/corner/offset/scale, not on screen position or
        // color). Cache the white coverage image and re-blit it translated +
        // tinted, so a moving or color-animating shadow never re-blurs. Only
        // valid when the transform is a pure translate + uniform positive scale;
        // otherwise fall through to the direct path so output is unchanged.
        SkMatrix m = canvas_->getTotalMatrix();
        const bool axis_aligned =
            !m.hasPerspective() && m.getSkewX() == 0.0f && m.getSkewY() == 0.0f &&
            m.getScaleX() > 0.0f && m.getScaleX() == m.getScaleY();
        BoxShadowCache& cache = BoxShadowCache::instance();
        if (axis_aligned && cache.enabled()) {
            const float scale = m.getScaleX();
            const SkRect occluder_local = SkRect::MakeXYWH(
                -spread, -spread, w + spread * 2.0f, h + spread * 2.0f);
            if (occluder_local.width() > 0.0f && occluder_local.height() > 0.0f) {
                const float pad = std::ceil(3.0f * sigma) + 2.0f;
                SkRect shadow_rect = occluder_local.makeOffset(dx, dy);
                shadow_rect.outset(pad, pad);

                BoxShadowKey key;
                key.w = BoxShadowKey::q(w);
                key.h = BoxShadowKey::q(h);
                key.dx = BoxShadowKey::q(dx);
                key.dy = BoxShadowKey::q(dy);
                key.blur = BoxShadowKey::q(blur);
                key.spread = BoxShadowKey::q(spread);
                key.corner_radius = BoxShadowKey::q(corner_radius);
                key.scale = BoxShadowKey::q(scale);

                sk_sp<SkImage> coverage =
                    cache.get_or_render(key, [&]() -> sk_sp<SkImage> {
                        // Bail (→ direct path) rather than clamp the device size:
                        // clamping width*scale to the max would allocate a
                        // truncated coverage image that drawImageRect then
                        // stretches across the full dst, squishing the shadow.
                        // Returning nullptr makes the caller fall through to the
                        // direct (uncached) path so output stays correct. The
                        // comparison also rejects inf/NaN (an extreme scale),
                        // which would otherwise be UB in the int cast below.
                        constexpr float kMaxCoveragePx = 16384.0f;
                        const float want_w = std::ceil(shadow_rect.width() * scale);
                        const float want_h = std::ceil(shadow_rect.height() * scale);
                        if (!(want_w > 0.0f) || !(want_h > 0.0f) ||
                            want_w > kMaxCoveragePx || want_h > kMaxCoveragePx)
                            return nullptr;
                        const int pw = static_cast<int>(want_w);
                        const int ph = static_cast<int>(want_h);
                        SkImageInfo info =
                            SkImageInfo::MakeN32Premul(pw, ph, SkColorSpace::MakeSRGB());
                        sk_sp<SkSurface> surf = SkSurfaces::Raster(info);
                        if (!surf) return nullptr;
                        SkCanvas* c = surf->getCanvas();
                        c->clear(SK_ColorTRANSPARENT);
                        c->scale(scale, scale);
                        c->translate(-shadow_rect.left(), -shadow_rect.top());
                        SkPaint sp;
                        sp.setAntiAlias(true);
                        const SkColor4f white = to_sk_color4f(Color::rgba(1, 1, 1, 1));
                        sp.setColor4f(white);
                        sp.setImageFilter(SkImageFilters::DropShadowOnly(
                            dx, dy, sigma, sigma, white, nullptr, nullptr));
                        if (corner_radius > 0.0f) {
                            float r = corner_radius + spread * 0.5f;
                            c->drawRRect(SkRRect::MakeRectXY(occluder_local, r, r), sp);
                        } else {
                            c->drawRect(occluder_local, sp);
                        }
                        return surf->makeImageSnapshot();
                    });

                if (coverage) {
                    SkPaint blit;
                    blit.setAntiAlias(true);
                    blit.setColorFilter(SkColorFilters::Blend(
                        to_sk_color4f(color), nullptr, SkBlendMode::kSrcIn));
                    SkRect dst = SkRect::MakeXYWH(x + shadow_rect.left(),
                                                  y + shadow_rect.top(),
                                                  shadow_rect.width(),
                                                  shadow_rect.height());
                    canvas_->drawImageRect(
                        coverage.get(),
                        SkRect::MakeWH(static_cast<float>(coverage->width()),
                                       static_cast<float>(coverage->height())),
                        dst, SkSamplingOptions(SkFilterMode::kLinear), &blit,
                        SkCanvas::kStrict_SrcRectConstraint);
                    return;
                }
            }
        }

        // Outset drop shadow (direct path): render the inflated rounded-rect
        // silhouette through SkImageFilters::DropShadowOnly so only the shadow
        // remains (no source pixels). This is exactly what Chromium uses
        // for `box-shadow` and matches the WebView reference within ~5px.
        SkRect occluder = SkRect::MakeXYWH(x - spread,
                                           y - spread,
                                           w + spread * 2.0f,
                                           h + spread * 2.0f);
        if (occluder.width() <= 0.0f || occluder.height() <= 0.0f) return;

        SkPaint shadow_paint;
        shadow_paint.setAntiAlias(true);
        // The fill color underneath doesn't matter — DropShadowOnly drops
        // the source — but Skia still respects the paint's alpha.
        shadow_paint.setColor4f(to_sk_color4f(Color::rgba(0, 0, 0, 1)));
        shadow_paint.setImageFilter(
            SkImageFilters::DropShadowOnly(dx, dy, sigma, sigma,
                                            to_sk_color4f(color), nullptr,
                                            nullptr));

        if (corner_radius > 0.0f) {
            float r = corner_radius + spread * 0.5f;
            canvas_->drawRRect(SkRRect::MakeRectXY(occluder, r, r),
                                shadow_paint);
        } else {
            canvas_->drawRect(occluder, shadow_paint);
        }
        return;
    }

    // Inset shadow:
    //   1. Clip to the box silhouette.
    //   2. Use SkBlendMode::kSrcOut against an inflated occluder so the
    //      blurred mask only shows along the inside edges of the box.
    SkRect box = SkRect::MakeXYWH(x, y, w, h);
    canvas_->save();
    if (corner_radius > 0.0f) {
        canvas_->clipRRect(SkRRect::MakeRectXY(box, corner_radius, corner_radius),
                            true);
    } else {
        canvas_->clipRect(box, true);
    }

    // Paint a translucent rect of the shadow color that covers the box,
    // then punch out the inflated, offset, blurred occluder so what
    // remains is the inset shadow ring.
    SkPaint full_paint;
    full_paint.setAntiAlias(true);
    full_paint.setColor4f(to_sk_color4f(color));
    canvas_->saveLayer(&box, nullptr);
    canvas_->drawRect(box, full_paint);

    SkPaint hole_paint;
    hole_paint.setAntiAlias(true);
    hole_paint.setBlendMode(SkBlendMode::kDstOut);
    hole_paint.setColor4f(to_sk_color4f(Color::rgba(0, 0, 0, 1)));
    if (sigma > 0.0f) {
        hole_paint.setImageFilter(SkImageFilters::Blur(sigma, sigma,
                                                        SkTileMode::kDecal,
                                                        nullptr));
    }
    SkRect hole = SkRect::MakeXYWH(x + dx + spread,
                                    y + dy + spread,
                                    w - spread * 2.0f,
                                    h - spread * 2.0f);
    if (hole.width() > 0.0f && hole.height() > 0.0f) {
        if (corner_radius > 0.0f) {
            float r = std::max(0.0f, corner_radius - spread * 0.5f);
            canvas_->drawRRect(SkRRect::MakeRectXY(hole, r, r), hole_paint);
        } else {
            canvas_->drawRect(hole, hole_paint);
        }
    }
    canvas_->restore();
    canvas_->restore();
}


}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
