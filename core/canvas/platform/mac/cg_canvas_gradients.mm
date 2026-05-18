// cg_canvas_gradients.mm — CoreGraphics gradient + pattern paint slice.
//
// Extracted from cg_canvas.mm in the 2026-05 Phase 4 (R2-3 mirror)
// refactor. Pairs with core/canvas/src/skia_canvas_gradients.cpp on
// the Skia side. Covers fill + stroke gradient builders (linear,
// radial, conic, two-circles) for the CoreGraphics CPU path.
//
// `stroke_with_active_paint` and `fill_with_active_paint` remain in
// cg_canvas.mm because they are the central dispatch points shared
// with the non-gradient paint paths.

#include <pulp/canvas/cg_canvas.hpp>
#include <algorithm>
#include <utility>

#ifdef __APPLE__

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

namespace pulp::canvas {

// SkiaCanvas stores a gradient shader and applies it as the fill paint on
// fill_current_path / fill_rect. CoreGraphics doesn't have a "set fill
// shader" call — gradient draws go through CGContextDrawLinearGradient /
// CGContextDrawRadialGradient, which paint the gradient inside the current
// clip region. We mirror Skia's behavior by clipping to the rect/path being
// filled and then issuing the appropriate Draw*Gradient call.

void CoreGraphicsCanvas::set_fill_gradient_linear(float x0, float y0,
                                                   float x1, float y1,
                                                   const Color* colors,
                                                   const float* positions,
                                                   int count) {
    if (count <= 0) {
        clear_fill_gradient();
        return;
    }
    has_gradient_ = true;
    gradient_kind_ = GradientKind::linear;
    gradient_is_radial_ = false;
    grad_x0_ = x0; grad_y0_ = y0; grad_x1_ = x1; grad_y1_ = y1;
    grad_colors_.assign(colors, colors + count);
    grad_positions_.assign(positions, positions + count);
}

void CoreGraphicsCanvas::set_fill_gradient_radial(float cx, float cy,
                                                   float radius,
                                                   const Color* colors,
                                                   const float* positions,
                                                   int count) {
    if (count <= 0) {
        clear_fill_gradient();
        return;
    }
    has_gradient_ = true;
    gradient_kind_ = GradientKind::radial;
    gradient_is_radial_ = true;
    // Single-circle form: inner circle collapses to the centre with radius 0,
    // outer circle is (cx, cy, radius). Matches the JS shim's pre-#1524
    // contract where createRadialGradient routed only the outer circle.
    grad_x0_ = cx; grad_y0_ = cy; grad_x1_ = cx; grad_y1_ = cy;
    grad_radius_inner_ = 0.0f;
    grad_radius_ = radius;
    grad_colors_.assign(colors, colors + count);
    grad_positions_.assign(positions, positions + count);
}

// pulp #1524 — true two-circle radial gradient. CGContextDrawRadialGradient
// accepts (start_centre, start_radius, end_centre, end_radius), so we wire
// both circles through unmodified. The previous bridge dropped (x0,y0,r0)
// and used only the outer circle, which silently degraded centre-bloom-
// only fills (Skia rendered the real two-point conical via MakeTwoPointConical;
// CG produced a visibly different shape).
void CoreGraphicsCanvas::set_fill_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) {
    if (count <= 0) {
        clear_fill_gradient();
        return;
    }
    has_gradient_ = true;
    gradient_kind_ = GradientKind::radial_two_circles;
    gradient_is_radial_ = true;
    grad_x0_ = x0; grad_y0_ = y0;
    grad_x1_ = x1; grad_y1_ = y1;
    grad_radius_inner_ = r0;
    grad_radius_ = r1;
    grad_colors_.assign(colors, colors + count);
    grad_positions_.assign(positions, positions + count);
}

// pulp #1524 — Canvas2D ctx.createConicGradient on the CG backend.
// CoreGraphics has no native conic / sweep shader, so we record the conic
// parameters here and software-rasterise a CGImage at fill time
// (paint_conic_into_clip), interpolating colour stops by atan2 angle from
// (cx, cy). The Skia backend uses SkGradientShader::MakeSweep — same
// visual result, real two-stop+ sweep gradient.
void CoreGraphicsCanvas::set_fill_gradient_conic(float cx, float cy,
                                                  float start_angle,
                                                  const Color* colors,
                                                  const float* positions,
                                                  int count) {
    if (count <= 0) {
        clear_fill_gradient();
        return;
    }
    has_gradient_ = true;
    gradient_kind_ = GradientKind::conic_image;
    gradient_is_radial_ = false;
    // Repurpose linear x0/y0 as conic centre, x1 as start_angle (radians).
    grad_x0_ = cx; grad_y0_ = cy;
    grad_x1_ = start_angle; grad_y1_ = 0;
    grad_colors_.assign(colors, colors + count);
    grad_positions_.assign(positions, positions + count);
    // The bitmap is generated lazily inside fill_with_active_paint() once
    // we know the destination clip rect. Drop any cached image from a
    // previous conic that may not match the upcoming clip.
    release_conic_image();
}

void CoreGraphicsCanvas::clear_fill_gradient() {
    has_gradient_ = false;
    gradient_kind_ = GradientKind::none;
    gradient_is_radial_ = false;
    grad_colors_.clear();
    grad_positions_.clear();
    release_conic_image();
    release_pattern_image();
}

// pulp #1666 — stroke gradient setters mirror the fill gradient ones
// 1:1, populating the parallel `stroke_*` slots. The gradient itself is
// painted at stroke time by stroke_with_active_paint() which clips to
// the path and routes through the same CGGradientRef draw calls.
void CoreGraphicsCanvas::set_stroke_gradient_linear(float x0, float y0,
                                                     float x1, float y1,
                                                     const Color* colors,
                                                     const float* positions,
                                                     int count) {
    if (count <= 0) { clear_stroke_gradient(); return; }
    has_stroke_gradient_ = true;
    stroke_gradient_kind_ = GradientKind::linear;
    stroke_grad_x0_ = x0; stroke_grad_y0_ = y0;
    stroke_grad_x1_ = x1; stroke_grad_y1_ = y1;
    stroke_grad_colors_.assign(colors, colors + count);
    stroke_grad_positions_.assign(positions, positions + count);
}

void CoreGraphicsCanvas::set_stroke_gradient_radial(float cx, float cy,
                                                     float radius,
                                                     const Color* colors,
                                                     const float* positions,
                                                     int count) {
    if (count <= 0) { clear_stroke_gradient(); return; }
    has_stroke_gradient_ = true;
    stroke_gradient_kind_ = GradientKind::radial;
    stroke_grad_x0_ = cx; stroke_grad_y0_ = cy;
    stroke_grad_x1_ = cx; stroke_grad_y1_ = cy;
    stroke_grad_radius_inner_ = 0.0f;
    stroke_grad_radius_ = radius;
    stroke_grad_colors_.assign(colors, colors + count);
    stroke_grad_positions_.assign(positions, positions + count);
}

void CoreGraphicsCanvas::set_stroke_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) {
    if (count <= 0) { clear_stroke_gradient(); return; }
    has_stroke_gradient_ = true;
    stroke_gradient_kind_ = GradientKind::radial_two_circles;
    stroke_grad_x0_ = x0; stroke_grad_y0_ = y0;
    stroke_grad_x1_ = x1; stroke_grad_y1_ = y1;
    stroke_grad_radius_inner_ = r0;
    stroke_grad_radius_ = r1;
    stroke_grad_colors_.assign(colors, colors + count);
    stroke_grad_positions_.assign(positions, positions + count);
}

void CoreGraphicsCanvas::set_stroke_gradient_conic(float cx, float cy,
                                                    float start_angle,
                                                    const Color* colors,
                                                    const float* positions,
                                                    int count) {
    if (count <= 0) { clear_stroke_gradient(); return; }
    has_stroke_gradient_ = true;
    stroke_gradient_kind_ = GradientKind::conic_image;
    stroke_grad_x0_ = cx; stroke_grad_y0_ = cy;
    stroke_grad_x1_ = start_angle; stroke_grad_y1_ = 0;
    stroke_grad_colors_.assign(colors, colors + count);
    stroke_grad_positions_.assign(positions, positions + count);
}

void CoreGraphicsCanvas::clear_stroke_gradient() {
    has_stroke_gradient_ = false;
    stroke_gradient_kind_ = GradientKind::none;
    stroke_grad_colors_.clear();
    stroke_grad_positions_.clear();
}

// pulp #1666 — paint a stroke through the active stroke gradient. Uses
// the same approach as fill: create CGGradientRef from stroke_grad_colors_,
// clip the context to the stroked path's outline (replace path mode), then
// call CGContextDrawLinearGradient / CGContextDrawRadialGradient over the
// clip's bounding rect. Conic falls back to apply_stroke_color (CG has no
// native sweep; the rasterised conic image isn't worth the complexity for
// stroked outlines which would need the conic image clipped to the stroke
// silhouette).

}  // namespace pulp::canvas

#endif  // __APPLE__
