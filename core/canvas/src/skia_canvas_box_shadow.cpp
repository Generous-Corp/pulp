// skia_canvas_box_shadow.cpp — Canvas2D / CSS box-shadow paint slice.
//
// Extracted from skia_canvas.cpp in the 2026-05 Phase 4 (R2-3 follow-up)
// refactor. Implements `draw_box_shadow(x, y, w, h, dx, dy, blur, spread,
// color, inset, corner_radius)` matching the CSS box-shadow spec —
// outset (default) shadows paint outside the rect via mask-filter blur;
// inset shadows paint inside via clip + reverse-difference. Issue-925.
//
// pulp #1737 (Codex P2 sweep on #1791) — Skia headers MUST be included
// BEFORE pulp/canvas/skia_canvas.hpp. See skia_canvas.cpp head-of-file
// comment for the C++ name-lookup rule that forces this ordering.

#include <algorithm>
#include <cmath>

#ifdef PULP_HAS_SKIA

#include "include/core/SkCanvas.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

// ── Box shadow (issue-925) ──────────────────────────────────────────────────

void SkiaCanvas::draw_box_shadow(float x, float y, float w, float h,
                                  float dx, float dy, float blur, float spread,
                                  Color color, bool inset, float corner_radius) {
    if (!canvas_) return;
    if (color.a <= 0.0f || (w <= 0.0f && h <= 0.0f)) return;

    // Skia's drop-shadow blur sigma is roughly half the CSS blur radius;
    // matching the WebView 1:1 within ~5px is the acceptance criterion
    // (#925) so we use blur/2.0 the way Chromium's compositor does.
    const float sigma = std::max(0.0f, blur * 0.5f);

    if (!inset) {
        // Outset drop shadow: render the inflated rounded-rect silhouette
        // through SkImageFilters::DropShadowOnly so only the shadow
        // remains (no source pixels). This is exactly what Chromium uses
        // for `box-shadow` and matches the WebView reference within ~5px
        // (#925 acceptance criterion).
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
