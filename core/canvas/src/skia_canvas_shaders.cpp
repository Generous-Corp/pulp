// skia_canvas_shaders.cpp — GPU shader-driven paint slices.
//
// Extracted from skia_canvas.cpp in the 2026-05 Phase 4 (R2-3 follow-up)
// refactor. Bundles four GPU/SkSL-driven concerns into one TU:
//
//   - GPU SDF Shape Primitives — kSDFShapeSkSL runtime effect + the
//     draw_sdf_shape / draw_sdf_ring / draw_sdf_rect family.
//   - Custom SkSL shader rendering — draw_with_shader() compiles a
//     user-supplied SkSL program with caller-bound uniforms.
//   - Blur backdrop — draw_blurred_backdrop() composites an
//     SkImageFilters::Blur saveLayer with an optional tint.
//   - GPU Waveform — kWaveformSkSL + draw_waveform() / draw_spectrum_bars()
//     for shader-driven 1D-texture waveform rendering.
//
// pulp #1737 (Codex P2 sweep on #1791) — Skia headers MUST be included
// BEFORE pulp/canvas/skia_canvas.hpp.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef PULP_HAS_SKIA

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkShader.h"
#include "include/core/SkSize.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

// ── GPU SDF Shape Primitives ─────────────────────────────────────────────────

static const char* kSDFShapeSkSL = R"(
    uniform float2 resolution;
    uniform float shapeType;  // 0=rect, 1=circle, 2=rounded_rect, 3=arc, 4=diamond
    uniform float cornerRadius;
    uniform float strokeWidth;
    uniform float arcStart;
    uniform float arcSweep;
    uniform float squirclePower;
    uniform float innerRadius;
    uniform float armWidth;
    uniform float bezierCX;
    uniform float bezierCY;
    uniform half4 fillColor;
    uniform half4 strokeColor;

    // SDF for a box centered at origin
    float sdBox(float2 p, float2 b) {
        float2 d = abs(p) - b;
        return length(max(d, float2(0.0))) + min(max(d.x, d.y), 0.0);
    }

    // SDF for a circle centered at origin
    float sdCircle(float2 p, float r) {
        return length(p) - r;
    }

    // SDF for rounded box
    float sdRoundBox(float2 p, float2 b, float r) {
        float2 q = abs(p) - b + float2(r);
        return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }

    // SDF for diamond (rotated square)
    float sdDiamond(float2 p, float s) {
        float2 q = abs(p);
        return (q.x + q.y - s) * 0.7071;
    }

    // SDF for squircle (superellipse): |x/a|^n + |y/b|^n = 1
    float sdSquircle(float2 p, float2 b, float n) {
        float2 q = abs(p) / b;
        return (pow(pow(q.x, n) + pow(q.y, n), 1.0/n) - 1.0) * min(b.x, b.y);
    }

    // SDF for equilateral triangle
    float sdTriangle(float2 p, float r) {
        float k = 1.7321; // sqrt(3)
        p.x = abs(p.x) - r;
        p.y = p.y + r / k;
        if (p.x + k * p.y > 0.0) p = float2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
        p.x -= clamp(p.x, -2.0 * r, 0.0);
        return -length(p) * sign(p.y);
    }

    // SDF for ring (annulus)
    float sdRing(float2 p, float outer, float inner) {
        return abs(length(p) - (outer + inner) * 0.5) - (outer - inner) * 0.5;
    }

    // SDF for stadium (pill/capsule)
    float sdStadium(float2 p, float2 b) {
        float r = min(b.x, b.y);
        float2 q = abs(p) - float2(b.x - r, 0.0);
        return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }

    // SDF for cross (plus sign)
    float sdCross(float2 p, float2 b, float armW) {
        float2 q = abs(p);
        float d1 = sdBox(q, float2(b.x, b.y * armW));
        float d2 = sdBox(q, float2(b.x * armW, b.y));
        return min(d1, d2);
    }

    // SDF for line segment with flat ends
    float sdFlatSegment(float2 p, float2 halfSize) {
        return sdBox(p, float2(halfSize.x, strokeWidth * 0.5));
    }

    // SDF for line segment with rounded ends
    float sdRoundedSegment(float2 p, float halfLen, float thickness) {
        p.x -= clamp(p.x, -halfLen, halfLen);
        return length(p) - thickness * 0.5;
    }

    // SDF for arc with thickness (flat caps)
    float sdFlatArc(float2 p, float outerR, float innerR, float startAngle, float sweepAngle) {
        float angle = atan2(p.y, p.x);
        float halfSweep = sweepAngle * 0.5;
        float midAngle = startAngle + halfSweep;
        float angleDiff = angle - midAngle;
        angleDiff = angleDiff - 6.2832 * floor((angleDiff + 3.1416) / 6.2832);
        float arcMask = abs(angleDiff) - halfSweep;
        float ringDist = abs(length(p) - (outerR + innerR) * 0.5) - (outerR - innerR) * 0.5;
        return max(ringDist, arcMask * outerR * 0.3);
    }

    // SDF for quadratic bezier curve with thickness (approximation)
    // Uses distance to the closest point on the curve segment
    float sdQuadBezier(float2 p, float2 a, float2 b, float2 c, float thickness) {
        // Approximate by sampling the curve at several points
        float minDist = 1e10;
        for (float t = 0.0; t <= 1.0; t += 0.05) {
            float2 q = (1.0-t)*(1.0-t)*a + 2.0*(1.0-t)*t*b + t*t*c;
            float d = length(p - q);
            minDist = min(minDist, d);
        }
        return minDist - thickness * 0.5;
    }

    half4 main(float2 coord) {
        float2 center = resolution * 0.5;
        float2 p = coord - center;
        float2 halfSize = center - float2(2.0);  // 2px padding for AA
        float r = min(halfSize.x, halfSize.y);
        float d;

        if (shapeType < 0.5) {
            d = sdBox(p, halfSize);              // 0: rect
        } else if (shapeType < 1.5) {
            d = sdCircle(p, r);                  // 1: circle
        } else if (shapeType < 2.5) {
            d = sdRoundBox(p, halfSize, cornerRadius); // 2: rounded rect
        } else if (shapeType < 3.5) {
            // 3: arc
            float angle = atan2(p.y, p.x);
            float halfSweep = arcSweep * 0.5;
            float midAngle = arcStart + halfSweep;
            float angleDiff = angle - midAngle;
            angleDiff = angleDiff - 6.2832 * floor((angleDiff + 3.1416) / 6.2832);
            float arcDist = abs(angleDiff) - halfSweep;
            float ringDist = abs(length(p) - r * 0.8) - strokeWidth * 0.5;
            d = max(ringDist, arcDist * r * 0.5);
        } else if (shapeType < 4.5) {
            d = sdDiamond(p, r);                 // 4: diamond
        } else if (shapeType < 5.5) {
            d = sdSquircle(p, halfSize, squirclePower); // 5: squircle
        } else if (shapeType < 6.5) {
            d = sdTriangle(p, r);                // 6: triangle
        } else if (shapeType < 7.5) {
            float outer = r;
            float inner = r * innerRadius;
            d = sdRing(p, outer, inner);         // 7: ring
        } else if (shapeType < 8.5) {
            d = sdStadium(p, halfSize);          // 8: stadium
        } else if (shapeType < 9.5) {
            d = sdCross(p, halfSize, armWidth);  // 9: cross
        } else if (shapeType < 10.5) {
            d = sdFlatSegment(p, halfSize);      // 10: flat segment
        } else if (shapeType < 11.5) {
            d = sdRoundedSegment(p, halfSize.x, max(strokeWidth, 2.0)); // 11: rounded segment
        } else if (shapeType < 12.5) {
            float outerR = r;
            float innerR = r * innerRadius;
            d = sdFlatArc(p, outerR, innerR, arcStart, arcSweep);       // 12: flat arc
        } else {
            // 13: quadratic bezier — control point from uniform parameters
            float2 a = float2(-halfSize.x, halfSize.y);
            float2 b = float2(bezierCX * halfSize.x, bezierCY * halfSize.y);
            float2 c = float2(halfSize.x, halfSize.y);
            d = sdQuadBezier(p, a, b, c, max(strokeWidth, 2.0));        // 13: quadratic bezier
        }

        // Render: filled or stroked with AA
        float aa = 1.0;
        if (strokeWidth > 0.0 && shapeType < 2.5) {
            float sd = abs(d) - strokeWidth * 0.5;
            float alpha = 1.0 - smoothstep(-aa, aa, sd);
            return strokeColor * half(alpha);
        } else {
            float alpha = 1.0 - smoothstep(-aa, aa, d);
            return fillColor * half(alpha);
        }
    }
)";

void SkiaCanvas::draw_sdf_shape(SDFShape shape, float x, float y, float w, float h,
                                 const SDFStyle& style) {
    if (!canvas_) { Canvas::draw_sdf_shape(shape, x, y, w, h, style); return; }

    static auto effectResult = SkRuntimeEffect::MakeForShader(SkString(kSDFShapeSkSL));
    if (!effectResult.effect) {
        Canvas::draw_sdf_shape(shape, x, y, w, h, style);
        return;
    }

    auto effect = effectResult.effect;
    SkRuntimeShaderBuilder builder(effect);
    builder.uniform("resolution") = SkV2{w, h};
    builder.uniform("shapeType") = static_cast<float>(shape);
    builder.uniform("cornerRadius") = style.corner_radius;
    builder.uniform("strokeWidth") = style.stroke_width;
    builder.uniform("arcStart") = style.arc_start;
    builder.uniform("arcSweep") = style.arc_sweep;
    builder.uniform("squirclePower") = style.squircle_power;
    builder.uniform("innerRadius") = style.inner_radius;
    builder.uniform("armWidth") = style.arm_width;
    builder.uniform("bezierCX") = style.bezier_cx;
    builder.uniform("bezierCY") = style.bezier_cy;
    builder.uniform("fillColor") = SkV4{
        style.fill_color.r, style.fill_color.g,
        style.fill_color.b, style.fill_color.a};
    builder.uniform("strokeColor") = SkV4{
        style.stroke_color.r, style.stroke_color.g,
        style.stroke_color.b, style.stroke_color.a};

    auto shader = builder.makeShader();
    if (!shader) { Canvas::draw_sdf_shape(shape, x, y, w, h, style); return; }

    SkPaint paint;
    paint.setShader(std::move(shader));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

// ── Custom SkSL shader rendering ─────────────────────────────────────────────

bool SkiaCanvas::draw_with_sksl(const std::string& sksl,
                                 float x, float y, float w, float h,
                                 const ShaderUniforms& uniforms) {
    if (!canvas_ || sksl.empty()) return false;

    // Compile and cache the shader effect (process-lifetime cache)
    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(sksl);
    if (!effect) return false;

    // Build shader with standard uniforms
    SkRuntimeShaderBuilder builder(effect);

    // Set all uniforms that exist in the shader (skip gracefully if not present)
    if (effect->findUniform("resolution"))
        builder.uniform("resolution") = SkV2{w, h};
    if (effect->findUniform("value"))
        builder.uniform("value") = uniforms.value;
    if (effect->findUniform("time"))
        builder.uniform("time") = uniforms.time;

    auto toSkV4 = [](Color c) -> SkV4 {
        return {c.r, c.g, c.b, c.a};
    };

    if (effect->findUniform("accentColor"))
        builder.uniform("accentColor") = toSkV4(uniforms.accent_color);
    if (effect->findUniform("bgColor"))
        builder.uniform("bgColor") = toSkV4(uniforms.bg_color);
    if (effect->findUniform("trackColor"))
        builder.uniform("trackColor") = toSkV4(uniforms.track_color);
    if (effect->findUniform("fillColor"))
        builder.uniform("fillColor") = toSkV4(uniforms.fill_color);
    if (effect->findUniform("thumbColor"))
        builder.uniform("thumbColor") = toSkV4(uniforms.thumb_color);

    auto shader = builder.makeShader();
    if (!shader) return false;

    SkPaint paint;
    paint.setShader(std::move(shader));
    canvas_->save();
    canvas_->translate(x, y);
    canvas_->drawRect(SkRect::MakeXYWH(0, 0, w, h), paint);
    canvas_->restore();
    return true;
}

bool SkiaCanvas::draw_native_dawn_texture(void* texture_handle,
                                          uint32_t width,
                                          uint32_t height,
                                          const std::string& format,
                                          float x,
                                          float y,
                                          float w,
                                          float h) {
    if (!canvas_ || !recorder_ || texture_handle == nullptr || width == 0 || height == 0) {
        return false;
    }

    auto* texture_ptr = static_cast<wgpu::Texture*>(texture_handle);
    if (texture_ptr == nullptr || !(*texture_ptr)) {
        return false;
    }

    auto backend_texture = skgpu::graphite::BackendTextures::MakeDawn(texture_ptr->Get());
    if (!backend_texture.isValid()) {
        return false;
    }

    auto image = SkImages::WrapTexture(recorder_,
                                       backend_texture,
                                       sk_color_type_from_webgpu_format(format),
                                       kPremul_SkAlphaType,
                                       sk_color_space_from_webgpu_format(format));
    if (!image) {
        return false;
    }

    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
    return true;
}

// ── Blur backdrop ────────────────────────────────────────────────────────────

void SkiaCanvas::draw_blurred_backdrop(float x, float y, float w, float h,
                                        float blur_radius, float corner_radius,
                                        Color tint) {
    if (!canvas_) return;

    SkRect rect = SkRect::MakeXYWH(x, y, w, h);

    // Backdrop blur using saveLayer with SkImageFilter
    auto blur = SkImageFilters::Blur(blur_radius, blur_radius, SkTileMode::kClamp, nullptr);

    canvas_->save();
    if (corner_radius > 0) {
        canvas_->clipRRect(SkRRect::MakeRectXY(rect, corner_radius, corner_radius), true);
    } else {
        canvas_->clipRect(rect, true);
    }

    // saveLayer with backdrop blur filter
    SkPaint layerPaint;
    layerPaint.setImageFilter(std::move(blur));
    canvas_->saveLayer(&rect, &layerPaint);
    canvas_->restore();

    // Tint overlay
    SkPaint tintPaint;
    tintPaint.setColor4f(to_sk_color4f(tint));
    if (corner_radius > 0) {
        canvas_->drawRRect(SkRRect::MakeRectXY(rect, corner_radius, corner_radius), tintPaint);
    } else {
        canvas_->drawRect(rect, tintPaint);
    }

    canvas_->restore();
}

// ── GPU Waveform (SkRuntimeEffect shader-driven) ────────────────────────────

// SkSL shader: samples waveform from a 1D texture, computes SDF distance
// to the curve for anti-aliased line + fill rendering.
static const char* kWaveformSkSL = R"(
    uniform shader waveformData;
    uniform float2 resolution;
    uniform float thickness;
    uniform float fillCenter;
    uniform half4 lineColor;
    uniform half4 fillColor;

    // Sample the waveform value at normalized x (0..1), returns -1..1
    float sampleWave(float x) {
        float texX = clamp(x, 0.0, 1.0) * resolution.x;
        // Sample red channel from the data texture
        return waveformData.eval(float2(texX + 0.5, 0.5)).r * 2.0 - 1.0;
    }

    // Minimum distance from point p to line segment a->b
    float segmentDist(float2 p, float2 a, float2 b) {
        float2 ab = b - a;
        float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
        float2 closest = a + t * ab;
        return length(p - closest);
    }

    half4 main(float2 coord) {
        float2 uv = coord / resolution;
        float cy = fillCenter;
        float halfH = resolution.y * 0.5;

        // Sample nearby waveform points for local line segments
        float pixelWidth = 1.0 / resolution.x;
        float minDist = 1e6;

        // Check 4 segments around current x for smooth coverage
        for (int i = -2; i <= 2; i++) {
            float x0 = uv.x + float(i) * pixelWidth;
            float x1 = x0 + pixelWidth;
            float y0 = cy - sampleWave(x0) * 0.5;
            float y1 = cy - sampleWave(x1) * 0.5;
            float2 a = float2(x0 * resolution.x, y0 * resolution.y);
            float2 b = float2(x1 * resolution.x, y1 * resolution.y);
            float d = segmentDist(coord, a, b);
            minDist = min(minDist, d);
        }

        // Line: SDF anti-aliased edge
        float lineAlpha = 1.0 - smoothstep(thickness * 0.5 - 0.5, thickness * 0.5 + 0.5, minDist);

        // Fill: area between waveform and center line
        float waveY = cy - sampleWave(uv.x) * 0.5;
        float centerY = cy;
        float fillAlpha = 0.0;
        if ((uv.y >= min(waveY, centerY) - pixelWidth) &&
            (uv.y <= max(waveY, centerY) + pixelWidth)) {
            // Slope-aware edge softening
            float edge = min(abs(uv.y - waveY), abs(uv.y - centerY));
            fillAlpha = 1.0 - smoothstep(0.0, pixelWidth * 2.0, edge);
            // Full fill in interior
            if (uv.y > min(waveY, centerY) + pixelWidth &&
                uv.y < max(waveY, centerY) - pixelWidth) {
                fillAlpha = 1.0;
            }
        }

        half4 result = fillColor * half(fillAlpha);
        result = result + lineColor * half(lineAlpha) * (1.0 - result.a);
        return result;
    }
)";

void SkiaCanvas::draw_waveform(const float* samples, size_t count,
                                float x, float y, float width, float height,
                                const WaveformStyle& style) {
    if (!canvas_ || count < 2) return;

    // Try SkRuntimeEffect shader path
    static auto effectResult = SkRuntimeEffect::MakeForShader(SkString(kWaveformSkSL));
    if (!effectResult.effect) {
        // Fallback to base class CPU implementation
        Canvas::draw_waveform(samples, count, x, y, width, height, style);
        return;
    }

    auto effect = effectResult.effect;

    // Pack sample data into an RGBA8 texture (store normalized 0..1 in R channel)
    // Each sample maps from [-1,1] to [0,1] for storage
    std::vector<uint8_t> texData(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint8_t val = static_cast<uint8_t>(std::clamp((samples[i] + 1.0f) * 0.5f, 0.0f, 1.0f) * 255.0f);
        texData[i * 4 + 0] = val;  // R
        texData[i * 4 + 1] = 0;
        texData[i * 4 + 2] = 0;
        texData[i * 4 + 3] = 255;
    }

    SkImageInfo texInfo = SkImageInfo::Make(static_cast<int>(count), 1,
                                            kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto texImage = SkImages::RasterFromPixmapCopy(
        SkPixmap(texInfo, texData.data(), count * 4));

    if (!texImage) {
        Canvas::draw_waveform(samples, count, x, y, width, height, style);
        return;
    }

    // Create child shader from the texture
    auto texShader = texImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                          SkSamplingOptions(SkFilterMode::kLinear));

    // Set uniforms
    SkRuntimeShaderBuilder builder(effect);
    builder.child("waveformData") = texShader;
    builder.uniform("resolution") = SkV2{width, height};
    builder.uniform("thickness") = style.line_thickness;
    builder.uniform("fillCenter") = style.fill_center;
    builder.uniform("lineColor") = SkV4{
        style.line_color.r, style.line_color.g,
        style.line_color.b, style.line_color.a};
    builder.uniform("fillColor") = SkV4{
        style.fill_color.r, style.fill_color.g,
        style.fill_color.b, style.fill_color.a};

    auto shader = builder.makeShader();
    if (!shader) {
        Canvas::draw_waveform(samples, count, x, y, width, height, style);
        return;
    }

    SkPaint paint;
    paint.setShader(std::move(shader));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, width, height), paint);
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
