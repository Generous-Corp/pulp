// skia_canvas_gradients.cpp — Canvas2D gradient + pattern fillStyle/strokeStyle slice.
//
// Owns the Skia implementations for Canvas2D fill/stroke gradients
// (linear, radial, conic, and two-circles) plus image-pattern repeat-mode
// plumbing.
//
// Skia headers MUST be included BEFORE pulp/canvas/skia_canvas.hpp. See
// skia_canvas.cpp's head-of-file comment for the C++ name-lookup rule that
// forces this ordering.
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

// Turn a sweep shader by `start_angle` radians about its own centre. Applied as
// a local matrix so the gradient's own angle window stays a full turn; see
// set_fill_gradient_conic for why the rotation cannot live in that window.
static sk_sp<SkShader> rotate_sweep(sk_sp<SkShader> shader, float cx, float cy,
                                    float start_angle) {
    if (shader == nullptr || start_angle == 0.0f) return shader;
    const float degrees = start_angle * 180.0f / 3.14159265f;
    return shader->makeWithLocalMatrix(SkMatrix::RotateDeg(degrees, {cx, cy}));
}

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

// An ellipse is a circle of radius rx with the y axis squashed by ry/rx about
// the centre. Skia has no elliptical gradient, but every shader carries a local
// matrix, so the squash goes on the SHADER rather than on the canvas — which
// matters because the canvas transform would also distort the shape being
// filled, and a background gradient is routinely filled through a rounded-rect
// or per-corner path whose corners must not stretch with it.
void SkiaCanvas::set_fill_gradient_radial_elliptical(
        float cx, float cy, float rx, float ry,
        const Color* colors, const float* positions, int count) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) {
        set_fill_gradient_radial(cx, cy, rx, colors, positions, count);
        return;
    }
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    auto shader = skia_gradient::make_radial({cx, cy}, rx,
                                             sk_colors.data(), sk_pos.data(), count);
    if (shader) {
        SkMatrix m = SkMatrix::Translate(cx, cy);
        m.preScale(1.0f, ry / rx);
        m.preTranslate(-cx, -cy);
        shader = shader->makeWithLocalMatrix(m);
    }
    gradient_shader_ = std::move(shader);
    has_gradient_ = gradient_shader_ != nullptr;
}

// A sweep's start angle is ROTATION, not a window into the turn. Skia clamps
// angles outside [start, end] rather than wrapping them, so handing it
// `start .. start + 360` left every angle before `start` outside the window and
// painted flat in the end stop's colour — a wedge the size of the rotation, and
// the CSS default `from 0deg` carries a -90° correction, so the common case lost
// a quarter of the circle. The window stays a full turn and the SHADER is
// rotated instead, which no angle can fall outside of.
void SkiaCanvas::set_fill_gradient_conic(float cx, float cy, float start_angle,
                                          const Color* colors, const float* positions, int count) {
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    gradient_shader_ = skia_gradient::make_sweep({cx, cy}, 0.0f, 360.0f,
                                                 sk_colors.data(), sk_pos.data(), count);
    gradient_shader_ = rotate_sweep(std::move(gradient_shader_), cx, cy, start_angle);
    has_gradient_ = gradient_shader_ != nullptr;
}

// The band spans `sweep_turns` of a turn and Skia tiles it the rest of the way:
// a sub-turn window plus a repeating tile mode IS the repetition, so nothing
// here expands the stop list or touches the paint path. A band of a full turn
// or more has nothing to repeat and degrades to the plain sweep.
void SkiaCanvas::set_fill_gradient_conic_repeating(float cx, float cy, float start_angle,
                                                    float sweep_turns,
                                                    const Color* colors,
                                                    const float* positions, int count) {
    if (!(sweep_turns > 0.0f) || sweep_turns >= 1.0f) {
        set_fill_gradient_conic(cx, cy, start_angle, colors, positions, count);
        return;
    }
    std::vector<SkColor4f> sk_colors;
    std::vector<float> sk_pos;
    colors_to_skia4f(colors, positions, count, sk_colors, sk_pos);
    gradient_shader_ = skia_gradient::make_sweep({cx, cy}, 0.0f, sweep_turns * 360.0f,
                                                 sk_colors.data(), sk_pos.data(), count,
                                                 SkTileMode::kRepeat);
    gradient_shader_ = rotate_sweep(std::move(gradient_shader_), cx, cy, start_angle);
    has_gradient_ = gradient_shader_ != nullptr;
}

// Canvas2D `ctx.createRadialGradient(x0,y0,r0,x1,y1,r1)` two-circle form.
// Skia renders the real two-point-conical gradient via
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

// ── Stroke gradients ────────────────────────────────────────────────────────
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
    // Same full-turn window + rotation as the fill side; a stroked sweep loses
    // the identical wedge otherwise.
    stroke_shader_ = skia_gradient::make_sweep({cx, cy}, 0.0f, 360.0f,
                                               sk_colors.data(), sk_pos.data(), count);
    stroke_shader_ = rotate_sweep(std::move(stroke_shader_), cx, cy, start_angle);
}

void SkiaCanvas::clear_stroke_gradient() {
    stroke_shader_ = nullptr;
}

// ── Patterns ────────────────────────────────────────────────────────────────
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
// previous solid color rather than rendering garbage.

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
    // Graphite (live GPU) cannot build a shader from a raster-backed SkImage —
    // it drops the draw with "Couldn't convert SkImage to a Graphite-backed
    // representation". Upload to a GPU texture first (no-op on raster canvases).
    image = ensure_gpu_image(image);
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
    image = ensure_gpu_image(image);  // GPU-upload — see set_fill_pattern.
    stroke_shader_ = image->makeShader(to_sk_tile_mode(tile_x),
                                        to_sk_tile_mode(tile_y),
                                        sampling_options_for_image_smoothing(),
                                        nullptr);
}


}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
