// skia_canvas_gradients.cpp — Canvas2D gradient + pattern fillStyle/strokeStyle slice.
//
// Extracted from skia_canvas.cpp in the 2026-05 Phase 4 (R2-3 follow-up)
// refactor. Covers linear / radial / conic / two-circles gradient
// builders for fill + stroke, plus image-pattern repeat-mode plumbing.
//
// pulp #1737 (Codex P2 sweep on #1791) — Skia headers MUST be included
// BEFORE pulp/canvas/skia_canvas.hpp. See skia_canvas.cpp head-of-file
// comment for the C++ name-lookup rule that forces this ordering.
//
// Skia's gradient API moved during the m149 window. This TU routes through
// skia_gradient_compat.hpp so Pulp uses the public packaged header surface
// available in the current Skia build instead of depending on one layout.

#include <algorithm>
#include <cstring>
#include <vector>

#ifdef PULP_HAS_SKIA

#include "include/core/SkBitmap.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkShader.h"
#include "skia_gradient_compat.hpp"

#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

// ── Gradients ────────────────────────────────────────────────────────────────

static void colors_to_skia4f(const Color* colors, const float* positions, int count,
                             std::vector<SkColor4f>& sk_colors,
                             std::vector<float>& sk_pos) {
    sk_colors.resize(static_cast<size_t>(count));
    sk_pos.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        sk_colors[static_cast<size_t>(i)] = SkColor4f::FromColor(colors[i].to_argb32());
        sk_pos[static_cast<size_t>(i)] = positions[i];
    }
}

void SkiaCanvas::set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                           const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    SkPoint pts[2] = {{x0, y0}, {x1, y1}};
    gradient_shader_ = skia_gradient::make_linear(pts, sk_colors.data(), sk_pos.data(), count);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_fill_gradient_radial(float cx, float cy, float radius,
                                           const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    gradient_shader_ = skia_gradient::make_radial({cx, cy}, radius,
                                                  sk_colors.data(), sk_pos.data(), count);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_fill_gradient_conic(float cx, float cy, float start_angle,
                                          const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    float start_deg = start_angle * 180.0f / 3.14159265f;
    gradient_shader_ = skia_gradient::make_sweep({cx, cy}, start_deg, start_deg + 360.0f,
                                                 sk_colors.data(), sk_pos.data(), count);
    has_gradient_ = gradient_shader_ != nullptr;
}

// pulp #1524 — Canvas2D `ctx.createRadialGradient(x0,y0,r0,x1,y1,r1)` two-circle
// form. Skia renders the real two-point-conical gradient via
// SkShaders::TwoPointConicalGradient, honouring an offset / sized inner
// circle (the existing single-circle path silently dropped (x0,y0,r0)).
void SkiaCanvas::set_fill_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    gradient_shader_ = skia_gradient::make_two_point_conical({x0, y0}, r0, {x1, y1}, r1,
                                                             sk_colors.data(), sk_pos.data(), count);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::clear_fill_gradient() {
    gradient_shader_ = nullptr;
    has_gradient_ = false;
}

// ── Stroke gradients (pulp Wave 3 c2d.7) ────────────────────────────────────
//
// Mirror of the fill-gradient setters above, targeting `stroke_shader_`.
// `apply_stroke_state` already attaches `stroke_shader_` to the active
// stroke paint, so every stroke path (stroke_rect, stroke_current_path,
// stroke_text, stroke_circle, stroke_arc, ...) inherits the gradient
// without per-call wiring. Sharing the field with the existing
// `set_stroke_pattern` is intentional: the spec assigns one stroke
// shader at a time — assigning a gradient replaces a prior pattern and
// vice versa, which matches Blink / WebKit semantics.

void SkiaCanvas::set_stroke_gradient_linear(float x0, float y0, float x1, float y1,
                                             const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    SkPoint pts[2] = {{x0, y0}, {x1, y1}};
    stroke_shader_ = skia_gradient::make_linear(pts, sk_colors.data(), sk_pos.data(), count);
}

void SkiaCanvas::set_stroke_gradient_radial(float cx, float cy, float radius,
                                             const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    stroke_shader_ = skia_gradient::make_radial({cx, cy}, radius,
                                                sk_colors.data(), sk_pos.data(), count);
}

void SkiaCanvas::set_stroke_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    stroke_shader_ = skia_gradient::make_two_point_conical({x0, y0}, r0, {x1, y1}, r1,
                                                           sk_colors.data(), sk_pos.data(), count);
}

void SkiaCanvas::set_stroke_gradient_conic(float cx, float cy, float start_angle,
                                            const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    float start_deg = start_angle * 180.0f / 3.14159265f;
    stroke_shader_ = skia_gradient::make_sweep({cx, cy}, start_deg, start_deg + 360.0f,
                                               sk_colors.data(), sk_pos.data(), count);
}

void SkiaCanvas::clear_stroke_gradient() {
    stroke_shader_ = nullptr;
}

// ── Patterns (pulp #1434 bridge-thin gap-fill) ──────────────────────────────
//
// Canvas2D `ctx.createPattern(image, repetition)` returns a CanvasPattern
// the shim assigns to fillStyle / strokeStyle. The shim then invokes
// `canvasSetFillPattern` / `canvasSetStrokePattern` which lands here as
// `set_fill_pattern` / `set_stroke_pattern`. We decode the source via the
// same `SkData` paths `draw_image_from_*` use, build an `SkShader::MakeImage`
// with the requested tile mode per axis, and stash it on
// `gradient_shader_` (for fills — already wired into `current_fill_paint`)
// or `stroke_shader_` (for strokes — picked up by `apply_stroke_state`).
//
// Falling back: if the image fails to decode (missing file, malformed
// data URI), we clear the active fill so the canvas degrades to the
// previous solid colour rather than rendering garbage.

namespace {

SkTileMode to_sk_tile_mode(pulp::canvas::Canvas::PatternTileMode mode) {
    using Tile = pulp::canvas::Canvas::PatternTileMode;
    return mode == Tile::repeat ? SkTileMode::kRepeat : SkTileMode::kDecal;
}

// Decode a pattern image source (file path or "data:" URL). Returns
// nullptr on failure; callers fall back to clearing the pattern.
sk_sp<SkImage> decode_pattern_image(const std::string& src) {
    if (src.empty()) return nullptr;
    constexpr std::string_view kDataPrefix = "data:";
    if (src.rfind(kDataPrefix, 0) == 0) {
        // The bridge already validated and decoded data URIs before
        // recording, so we don't see them here in practice — but keep
        // a guard so we don't accidentally feed a base64 blob to
        // SkData::MakeFromFileName.
        return nullptr;
    }
    auto data = SkData::MakeFromFileName(src.c_str());
    if (!data) return nullptr;
    return SkImages::DeferredFromEncodedData(data);
}

} // namespace

void SkiaCanvas::set_fill_pattern(const std::string& image_src,
                                   PatternTileMode tile_x,
                                   PatternTileMode tile_y) {
    auto image = decode_pattern_image(image_src);
    if (!image) {
        clear_fill_gradient();
        return;
    }
    gradient_shader_ = image->makeShader(to_sk_tile_mode(tile_x),
                                          to_sk_tile_mode(tile_y),
                                          sampling_options_for_image_smoothing(),
                                          nullptr);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_stroke_pattern(const std::string& image_src,
                                     PatternTileMode tile_x,
                                     PatternTileMode tile_y) {
    auto image = decode_pattern_image(image_src);
    if (!image) {
        stroke_shader_ = nullptr;
        return;
    }
    stroke_shader_ = image->makeShader(to_sk_tile_mode(tile_x),
                                        to_sk_tile_mode(tile_y),
                                        sampling_options_for_image_smoothing(),
                                        nullptr);
}


}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
