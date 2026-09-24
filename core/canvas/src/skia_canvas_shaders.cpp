// skia_canvas_shaders.cpp — GPU shader-driven paint slices.
//
// Owns four GPU/SkSL-driven Canvas paint paths:
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
// Skia headers MUST be included BEFORE pulp/canvas/skia_canvas.hpp. See
// skia_canvas.cpp's head-of-file comment for the C++ name-lookup rule that
// forces this ordering.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdint>
#include <regex>

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
#include "skia_canvas_internal.hpp"  // to_sk_color4f, webgpu-format helpers
#include "runtime_effect_cache.hpp"  // RuntimeEffectCache (sibling header)
#if PULP_CANVAS_GRAPHITE
// WebGPU/Graphite native-texture wrapping needs these Graphite/Dawn includes.
// They do not exist in a Ganesh-only Skia (the Emscripten slice), which is why
// draw_native_dawn_texture has no meaning there.
#include "include/gpu/GpuTypes.h"                  // skgpu::Origin
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Image.h"            // SkImages::WrapTexture
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"  // BackendTextures::MakeDawn(WGPUTexture)
#include "webgpu/webgpu_cpp.h"
#endif
#endif

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

static const char* sdf_shape_name(Canvas::SDFShape shape) {
    switch (shape) {
        case Canvas::SDFShape::rect: return "rect";
        case Canvas::SDFShape::circle: return "circle";
        case Canvas::SDFShape::rounded_rect: return "rounded_rect";
        case Canvas::SDFShape::arc: return "arc";
        case Canvas::SDFShape::diamond: return "diamond";
        case Canvas::SDFShape::squircle: return "squircle";
        case Canvas::SDFShape::triangle: return "triangle";
        case Canvas::SDFShape::ring: return "ring";
        case Canvas::SDFShape::stadium: return "stadium";
        case Canvas::SDFShape::cross: return "cross";
        case Canvas::SDFShape::flat_segment: return "flat_segment";
        case Canvas::SDFShape::rounded_segment: return "rounded_segment";
        case Canvas::SDFShape::flat_arc: return "flat_arc";
        case Canvas::SDFShape::quadratic_bezier: return "quadratic_bezier";
    }
    return "unknown";
}

// The set of shapes that actually emit a usable chart, which is exactly the
// set main() marks `valid`. Advertising a shape here that main() then reports
// invalid would trade a precise install-time error for a silent zero at every
// fragment, which is the failure mode the chart's absence rule exists to
// prevent.
static bool sdf_shape_has_chart(Canvas::SDFShape shape) {
    return shape == Canvas::SDFShape::flat_arc;
}

namespace {
// IEEE-754 float32 -> binary16 conversion for the RGBA_F16 raster upload.
std::uint16_t shader_float_to_half(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xffu;
    const std::uint32_t mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu)
        return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa ? 0x0200u : 0));
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (e <= 0) {
        if (e < -10) return static_cast<std::uint16_t>(sign);
        const std::uint32_t m = (mantissa | 0x800000u) >> static_cast<unsigned>(1 - e + 13);
        return static_cast<std::uint16_t>(sign | m);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(e) << 10) |
                                       (mantissa >> 13));
}

sk_sp<SkShader> make_shader_data_texture(
    const std::shared_ptr<const Canvas::ShaderDataTexture>& data) {
    if (!data) return nullptr;
    const int width = std::max<int>(1, static_cast<int>(data->samples.size()));
    std::vector<std::uint16_t> texels(static_cast<std::size_t>(width) * 4u, 0);
    for (int i = 0; i < width; ++i) {
        const float sample = data->samples[static_cast<std::size_t>(i)];
        texels[static_cast<std::size_t>(i) * 4u + 0] = shader_float_to_half(sample);
        texels[static_cast<std::size_t>(i) * 4u + 3] = shader_float_to_half(1.0f);
    }
    const auto info = SkImageInfo::Make(width, 1, kRGBA_F16_SkColorType,
                                        kPremul_SkAlphaType);
    auto image = SkImages::RasterFromPixmapCopy(
        SkPixmap(info, texels.data(), static_cast<std::size_t>(width) * 8u));
    if (!image) return nullptr;
    return image->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                             SkSamplingOptions(SkFilterMode::kLinear));
}
} // namespace

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
        float angle = atan(p.y, p.x);
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

// Phase-B authoring entry point.  Reuse the production primitive functions
// above, but stop before their legacy main() and provide a structured geometry
// value to the author's shade() function.  Keeping this composer alongside
// the legacy source makes the two paths share exactly the same distances.
static std::string compose_sdf_geometry_shader(Canvas::SDFShape shape,
                                               const std::string& author_sksl,
                                               const std::string& sdf_expression = {},
                                               std::uint32_t leaf_count = 0) {
    (void)shape;
    const std::string source(kSDFShapeSkSL);
    const auto main_at = source.find("half4 main(float2 coord)");
    std::string primitive_prelude =
        main_at == std::string::npos ? source : source.substr(0, main_at);
    // SkSL exposes atan(y, x), whereas the legacy source used atan2 in a
    // path that was not previously composed through RuntimeEffect.
    for (std::size_t at = primitive_prelude.find("atan2("); at != std::string::npos;
         at = primitive_prelude.find("atan2(", at + 1))
        primitive_prelude.replace(at, 6, "atan(");
    std::string leaf_uniforms;
    for (std::uint32_t i = 0; i < leaf_count; ++i) {
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_x;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_y;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_w;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_h;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_cornerRadius;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_innerRadius;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_arcStart;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_arcSweep;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_squirclePower;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_armWidth;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_bezierCX;\n";
        leaf_uniforms += "uniform float pulp_leaf" + std::to_string(i) + "_bezierCY;\n";
    }
    auto composed = primitive_prelude + leaf_uniforms + R"(
struct PulpGeom { float sdf; float2 grad; float2 pos; float2 uv; float coverage; };
struct PulpFragment { half4 color; float strokeWidth; float sigma; };
float pulp_smooth_union(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / max(abs(k), 0.0001), 0.0, 1.0);
    return mix(b, a, h) - abs(k) * h * (1.0 - h);
}
float pulp_smooth_subtract(float a, float b, float k) {
    return -pulp_smooth_union(-a, b, k);
}

float pulp_shape_distance(float2 p) {
    float2 halfSize = resolution * 0.5 - float2(2.0);
    float r = min(halfSize.x, halfSize.y);
    if (shapeType < 0.5) return sdBox(p, halfSize);
    if (shapeType < 1.5) return sdCircle(p, r);
    if (shapeType < 2.5) return sdRoundBox(p, halfSize, cornerRadius);
    if (shapeType < 3.5) {
        float angle = atan(p.y, p.x);
        float halfSweep = arcSweep * 0.5;
        float diff = angle - (arcStart + halfSweep);
        diff -= 6.2832 * floor((diff + 3.1416) / 6.2832);
        float ring = abs(length(p) - r * 0.8) - strokeWidth * 0.5;
        return max(ring, (abs(diff) - halfSweep) * r * 0.5);
    }
    if (shapeType < 4.5) return sdDiamond(p, r);
    if (shapeType < 5.5) return sdSquircle(p, halfSize, squirclePower);
    if (shapeType < 6.5) return sdTriangle(p, r);
    if (shapeType < 7.5) return sdRing(p, r, r * innerRadius);
    if (shapeType < 8.5) return sdStadium(p, halfSize);
    if (shapeType < 9.5) return sdCross(p, halfSize, armWidth);
    if (shapeType < 10.5) return sdFlatSegment(p, halfSize);
    if (shapeType < 11.5) return sdRoundedSegment(p, halfSize.x, max(strokeWidth, 2.0));
    if (shapeType < 12.5) return sdFlatArc(p, r, r * innerRadius, arcStart, arcSweep);
    return sdQuadBezier(p, float2(-halfSize.x, halfSize.y),
                        float2(bezierCX * halfSize.x, bezierCY * halfSize.y),
                        float2(halfSize.x, halfSize.y), max(strokeWidth, 2.0));
}

PulpGeom pulp_geom(float2 coord) {
    float2 p = coord - resolution * 0.5;
    float d = pulp_shape_distance(p);
    // Central differences keep the gradient tied to the exact composed field
    // and remain stable for all primitive branches.
    float e = 0.5;
    float2 grad = float2(pulp_shape_distance(p + float2(e, 0.0)) -
                         pulp_shape_distance(p - float2(e, 0.0)),
                         pulp_shape_distance(p + float2(0.0, e)) -
                         pulp_shape_distance(p - float2(0.0, e))) / (2.0 * e);
    // At an exact extremum of the field the central differences cancel and
    // grad is exactly zero; normalize() would divide by zero and yield NaN for
    // that fragment. A zero direction is the honest answer there — the field
    // has no gradient — and it keeps every author expression finite.
    float gradLen = length(grad);
    float2 gradDir = gradLen > 0.0 ? grad / gradLen : float2(0.0);
    return PulpGeom(d, gradDir, p, coord / resolution,
                    1.0 - smoothstep(-1.0, 1.0, d));
}
)" + author_sksl + R"(
half4 main(float2 coord) {
    PulpGeom g = pulp_geom(coord);
    PulpFragment f = shade(g, coord);
    float sd = f.strokeWidth > 0.0 ? abs(g.sdf) - f.strokeWidth * 0.5 : g.sdf;
    float alpha = 1.0 - smoothstep(-1.0, 1.0, sd);
    return f.color * half(alpha);
}
)";
    if (!sdf_expression.empty()) {
        const std::string needle = "float d = pulp_shape_distance(p);";
        const auto at = composed.find(needle);
        if (at != std::string::npos)
            composed.replace(at, needle.size(), "float d = " + sdf_expression + ";");
    }
    return composed;
}

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

static std::string compose_sdf_chart_shader(const std::string& author_sksl) {
    return R"(
struct PulpChart { float t; float d; float side; float2 tan; float px; float valid; };
struct PulpSdf { float d; float id; };
uniform float2 resolution;
uniform float shapeType;
uniform float arcStart;
uniform float arcSweep;
uniform float innerRadius;
uniform float strokeWidth;
uniform float reach;
uniform float featherSigma;
uniform float featherCurve;
uniform float featherMode;
// Device pixels per local canvas unit, from Canvas::backing_scale().
uniform float pixelScale;
uniform float leaf0; uniform float leaf1; uniform float leaf2; uniform float leaf3;
uniform float leaf4; uniform float leaf5; uniform float leaf6; uniform float leaf7;
uniform float leaf8; uniform float leaf9; uniform float leaf10; uniform float leaf11;
uniform float leaf12; uniform float leaf13; uniform float leaf14; uniform float leaf15;

// Abramowitz–Stegun erfc approximation used by the analytic feather path.
float pulpErfc(float x) {
    float ax = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * ax);
    float p = (((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t
                 - 0.284496736) * t + 0.254829592) * t);
    float e = p * exp(-ax * ax);
    return x >= 0.0 ? e : 2.0 - e;
}

float pulpFeather(float sd, float sigma, int curve, int mode) {
    float s = max(abs(sigma), 0.0001);
    float coverage = curve == 0
        ? 0.5 * pulpErfc(sd / (s * 1.41421356237))
        : clamp(0.5 - sd / (2.0 * s), 0.0, 1.0);
    if (mode == 1) coverage *= sd > 0.0 ? 1.0 : 0.0; // glow
    else if (mode == 2) coverage *= sd < 0.0 ? 1.0 : 0.0; // inner
    else if (mode == 3) coverage = 1.0 - coverage; // outer
    else if (mode == 4) coverage = sd < 0.0 ? 1.0 - coverage : 0.0; // inset
    // Radial and sweep are chart-aware modes; the scalar form remains useful
    // as a stable fallback when no chart is available.
    else if (mode == 5) coverage *= sd < 0.0 ? 1.0 : 0.0;
    else if (mode == 6) coverage *= sd > 0.0 ? 1.0 : 0.0;
    return clamp(coverage, 0.0, 1.0);
}
float pulpFeather(PulpChart g, float sigma, int curve, int mode) {
    float scale = mode == 5 ? max(abs(g.d), 0.001)
                            : (mode == 6 ? max(abs(g.t - 0.5) * 2.0, 0.001) : 1.0);
    return pulpFeather(g.d, sigma * scale, curve, mode == 5 || mode == 6 ? 0 : mode);
}

// Bounded analytic feathering. mode 0 is a compact linear/smooth edge,
// mode 1 is Gaussian-like falloff, and mode 2 is an exponential glow. The
// caller supplies pixel width in the same units as d; invalid widths are
// clamped so a malformed author uniform cannot create NaNs.
PulpSdf pulpLeaf(float d, float id) { return PulpSdf(d, id); }
PulpSdf pulpUnion(PulpSdf a, PulpSdf b) { return a.d < b.d ? a : b; }
PulpSdf pulpIntersect(PulpSdf a, PulpSdf b) { return a.d > b.d ? a : b; }
PulpSdf pulpSubtract(PulpSdf a, PulpSdf b) { return PulpSdf(max(a.d, -b.d), a.id); }
PulpSdf pulpSmoothUnion(PulpSdf a, PulpSdf b, float k) {
    float h = clamp(0.5 + 0.5 * (b.d - a.d) / max(abs(k), 0.0001), 0.0, 1.0);
    return PulpSdf(mix(b.d, a.d, h) - abs(k) * h * (1.0 - h), h < 0.5 ? b.id : a.id);
}
)" + author_sksl + R"(
half4 main(float2 coord) {
    float2 center = resolution * 0.5;
    float2 p = coord - center;
    float radius = min(center.x, center.y) - 2.0;
    float angle = atan(p.y, p.x);
    float halfSweep = arcSweep * 0.5;
    float mid = arcStart + halfSweep;
    float diff = angle - mid;
    diff = diff - 6.2831853 * floor((diff + 3.1415926) / 6.2831853);
    float outer = radius;
    float inner = radius * innerRadius;
    float d = abs(length(p) - (outer + inner) * 0.5) - (outer - inner) * 0.5;
    float t = clamp((diff + halfSweep) / max(2.0 * halfSweep, 0.0001), 0.0, 1.0);
    float2 tan = float2(-sin(angle), cos(angle));
    // px is one device pixel measured in the same units as d, so an author
    // writing `2.0 * g.px` gets two device pixels at any backing scale.
    float px = 1.0 / max(pixelScale, 0.0001);
    PulpChart g = PulpChart(t, d, d < 0.0 ? -1.0 : 1.0, tan, px,
                            (shapeType > 11.5 && shapeType < 12.5 &&
                             arcSweep > 0.0 && outer > inner) ? 1.0 : 0.0);
    half4 shaded = shade(g);
    if (featherSigma > 0.0) {
        float feather = pulpFeather(g, featherSigma,
                                    int(featherCurve + 0.5),
                                    int(featherMode + 0.5));
        shaded.a *= half(feather);
    }
    return shaded;
}
)";
}

// RuntimeEffect reports diagnostics against the composed source. Keep author
// feedback stable by translating the two formats SkSL has used in practice
// ("error: N:" and "line N") back to the author's source line.
static std::string remap_sksl_author_lines(const std::string& error,
                                           const std::string& composed,
                                           const std::string& author) {
    const auto at = composed.find(author);
    if (at == std::string::npos || at == 0) return error;
    const auto prefix = composed.substr(0, at);
    const auto offset = static_cast<int>(std::count(prefix.begin(), prefix.end(), '\n'));
    if (offset <= 0) return error;

    auto remap = [offset](const std::string& input, const std::regex& pattern,
                          bool has_suffix) {
        std::string output;
        std::size_t cursor = 0;
        for (std::sregex_iterator it(input.begin(), input.end(), pattern), end;
             it != end; ++it) {
            const auto& match = *it;
            output.append(input, cursor, static_cast<std::size_t>(match.position()) - cursor);
            const int line = std::stoi(match[2].str());
            output += match[1].str();
            output += std::to_string(std::max(1, line - offset));
            if (has_suffix) output += match[3].str();
            cursor = static_cast<std::size_t>(match.position() + match.length());
        }
        output.append(input, cursor, std::string::npos);
        return output;
    };
    const std::regex error_line(R"((error:\s*)([0-9]+)(:))");
    auto mapped = remap(error, error_line, true);
    const std::regex line_number(R"((\bline\s+)([0-9]+))");
    return remap(mapped, line_number, false);
}

std::string Canvas::compile_sdf_chart_sksl(SDFShape shape, const std::string& sksl) {
    if (sksl.empty()) return "Empty shader code";
    if (!sdf_shape_has_chart(shape) &&
        (sksl.find("PulpChart") != std::string::npos ||
         sksl.find("pulp_chart") != std::string::npos))
        return std::string("Shape '") + sdf_shape_name(shape) +
               "' has no stroke chart (t/d/side); chart shaders require a band "
               "shape whose chart is implemented (flat_arc)";
    std::string error;
    const bool structured = sksl.find("PulpFragment shade") != std::string::npos;
    const auto source = structured
                            ? compose_sdf_geometry_shader(SDFShape::flat_arc, sksl)
                            : compose_sdf_chart_shader(sksl);
    auto effect = RuntimeEffectCache::instance().get_or_compile(source, error);
    return effect ? std::string() : remap_sksl_author_lines(error, source, sksl);
}

bool SkiaCanvas::draw_sdf_shape_with_shader(SDFShape shape, float x, float y,
                                              float w, float h,
                                              const SDFStyle& style,
                                              const std::string& author_sksl,
                                              const ShaderDrawOptions& options) {
    if (!canvas_ || author_sksl.empty()) return false;
    if (!sdf_shape_has_chart(shape) &&
        (author_sksl.find("PulpChart") != std::string::npos ||
         author_sksl.find("pulp_chart") != std::string::npos))
        return false;
    // Keep the chart prelude deliberately small and explicit. It is emitted
    // before the author function so the same source can be compiled at draw
    // time and by the bridge's normal SkSL compiler.
    const bool structured = author_sksl.find("PulpFragment shade") != std::string::npos;
    const std::string source = structured
                                   ? compose_sdf_geometry_shader(shape, author_sksl)
                                   : compose_sdf_chart_shader(author_sksl);
    std::string error;
    auto effect = RuntimeEffectCache::instance().get_or_compile(source, error);
    if (!effect) return false;
    SkRuntimeShaderBuilder builder(effect);
    builder.uniform("resolution") = SkV2{w, h};
    builder.uniform("shapeType") = static_cast<float>(shape);
    builder.uniform("arcStart") = style.arc_start;
    builder.uniform("arcSweep") = style.arc_sweep;
    builder.uniform("innerRadius") = style.inner_radius;
    builder.uniform("strokeWidth") = style.stroke_width;
    builder.uniform("reach") = options.reach;
    builder.uniform("featherSigma") = style.feather_sigma;
    builder.uniform("featherCurve") = static_cast<float>(style.feather_curve);
    builder.uniform("featherMode") = static_cast<float>(style.feather_mode);
    // Ignored by the geometry composer, which declares no pixelScale.
    builder.uniform("pixelScale") = backing_scale();
    for (const auto& named : options.named_uniforms) {
        if (!effect->findUniform(named.name.c_str()) || named.count < 1 || named.count > 4) continue;
        auto slot = builder.uniform(named.name.c_str());
        if (named.count == 1) slot = named.v[0];
        else if (named.count == 2) slot = SkV2{named.v[0], named.v[1]};
        else if (named.count == 3) slot = SkV3{named.v[0], named.v[1], named.v[2]};
        else slot = SkV4{named.v[0], named.v[1], named.v[2], named.v[3]};
    }
    if (options.data_texture) {
        if (auto data_shader = make_shader_data_texture(options.data_texture)) {
            // The bridge's scope binding names the child after the channel;
            // retain the generic aliases for hand-authored shaders.
            std::vector<std::string> candidates = {options.data_texture->name,
                                                   "scopeData", "scope", "valueChannel"};
            for (const auto& candidate : candidates)
                if (effect->findChild(candidate.c_str())) builder.child(candidate.c_str()) = data_shader;
        }
        const std::string suffixes[] = {"_count", "_live", "_neutral"};
        const std::string prefix = options.data_texture->name;
        if (effect->findUniform("_count")) builder.uniform("_count") = static_cast<float>(options.data_texture->count);
        if (effect->findUniform("_live")) builder.uniform("_live") = options.data_texture->live ? 1.0f : 0.0f;
        if (effect->findUniform("_neutral")) builder.uniform("_neutral") = options.data_texture->neutral;
        if (!prefix.empty()) {
            if (effect->findUniform((prefix + suffixes[0]).c_str())) builder.uniform((prefix + suffixes[0]).c_str()) = static_cast<float>(options.data_texture->count);
            if (effect->findUniform((prefix + suffixes[1]).c_str())) builder.uniform((prefix + suffixes[1]).c_str()) = options.data_texture->live ? 1.0f : 0.0f;
            if (effect->findUniform((prefix + suffixes[2]).c_str())) builder.uniform((prefix + suffixes[2]).c_str()) = options.data_texture->neutral;
        }
    }
    auto shader = builder.makeShader();
    if (!shader) return false;
    SkPaint paint; paint.setShader(std::move(shader));
    canvas_->save(); canvas_->translate(x, y);
    canvas_->drawRect(SkRect::MakeXYWH(0, 0, w, h), paint); canvas_->restore();
    return true;
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

bool SkiaCanvas::draw_with_sksl(const std::string& sksl,
                                 float x, float y, float w, float h,
                                 const ShaderDrawOptions& options) {
    if (!canvas_ || sksl.empty()) return false;
    std::string composed = sksl;
    if (options.geometry && sksl.find("PulpFragment shade") != std::string::npos)
        composed = compose_sdf_geometry_shader(options.geometry->shape, sksl,
                                               options.geometry->sdf_expression,
                                               options.geometry->leaf_count);
    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(composed);
    if (!effect) return false;
    SkRuntimeShaderBuilder builder(effect);
    if (effect->findUniform("resolution")) builder.uniform("resolution") = SkV2{w, h};
    if (effect->findUniform("reach")) builder.uniform("reach") = options.reach;
    const auto& u = options.uniforms;
    if (effect->findUniform("value")) builder.uniform("value") = u.value;
    if (effect->findUniform("time")) builder.uniform("time") = u.time;
    auto color = [](Color c) -> SkV4 { return {c.r, c.g, c.b, c.a}; };
    if (effect->findUniform("accentColor")) builder.uniform("accentColor") = color(u.accent_color);
    if (effect->findUniform("bgColor")) builder.uniform("bgColor") = color(u.bg_color);
    if (effect->findUniform("trackColor")) builder.uniform("trackColor") = color(u.track_color);
    if (effect->findUniform("fillColor")) builder.uniform("fillColor") = color(u.fill_color);
    if (effect->findUniform("thumbColor")) builder.uniform("thumbColor") = color(u.thumb_color);
    if (options.geometry) {
        const auto& style = options.geometry->style;
        if (effect->findUniform("shapeType")) builder.uniform("shapeType") = static_cast<float>(options.geometry->shape);
        if (effect->findUniform("cornerRadius")) builder.uniform("cornerRadius") = style.corner_radius;
        if (effect->findUniform("strokeWidth")) builder.uniform("strokeWidth") = style.stroke_width;
        if (effect->findUniform("arcStart")) builder.uniform("arcStart") = style.arc_start;
        if (effect->findUniform("arcSweep")) builder.uniform("arcSweep") = style.arc_sweep;
        if (effect->findUniform("squirclePower")) builder.uniform("squirclePower") = style.squircle_power;
        if (effect->findUniform("innerRadius")) builder.uniform("innerRadius") = style.inner_radius;
        if (effect->findUniform("armWidth")) builder.uniform("armWidth") = style.arm_width;
        if (effect->findUniform("bezierCX")) builder.uniform("bezierCX") = style.bezier_cx;
        if (effect->findUniform("bezierCY")) builder.uniform("bezierCY") = style.bezier_cy;
        for (const auto& uniform : options.geometry->leaf_uniforms) {
            if (!effect->findUniform(uniform.name.c_str())) continue;
            switch (uniform.count) {
                case 1: builder.uniform(uniform.name.c_str()) = uniform.v[0]; break;
                case 2: builder.uniform(uniform.name.c_str()) = SkV2{uniform.v[0], uniform.v[1]}; break;
                case 3: builder.uniform(uniform.name.c_str()) = SkV3{uniform.v[0], uniform.v[1], uniform.v[2]}; break;
                case 4: builder.uniform(uniform.name.c_str()) = SkV4{uniform.v[0], uniform.v[1], uniform.v[2], uniform.v[3]}; break;
                default: break;
            }
        }
    }
    if (options.data_texture) {
        if (auto data_shader = make_shader_data_texture(options.data_texture)) {
            std::vector<std::string> candidates = {options.data_texture->name,
                                                   "scopeData", "scope", "valueChannel"};
            for (const auto& candidate : candidates)
                if (effect->findChild(candidate.c_str())) builder.child(candidate.c_str()) = data_shader;
        }
        if (effect->findUniform("_count"))
            builder.uniform("_count") = static_cast<float>(options.data_texture->count);
        if (effect->findUniform("_live"))
            builder.uniform("_live") = options.data_texture->live ? 1.0f : 0.0f;
        if (effect->findUniform("_neutral"))
            builder.uniform("_neutral") = options.data_texture->neutral;
        const std::string prefix = options.data_texture->name;
        if (!prefix.empty()) {
            if (effect->findUniform((prefix + "_count").c_str())) builder.uniform((prefix + "_count").c_str()) = static_cast<float>(options.data_texture->count);
            if (effect->findUniform((prefix + "_live").c_str())) builder.uniform((prefix + "_live").c_str()) = options.data_texture->live ? 1.0f : 0.0f;
            if (effect->findUniform((prefix + "_neutral").c_str())) builder.uniform((prefix + "_neutral").c_str()) = options.data_texture->neutral;
        }
    }
    for (const auto& named : options.named_uniforms) {
        auto* info = effect->findUniform(named.name.c_str());
        if (!info || named.count < 1 || named.count > 4) continue;
        auto slot = builder.uniform(named.name.c_str());
        if (named.count == 1) slot = named.v[0];
        else if (named.count == 2) slot = SkV2{named.v[0], named.v[1]};
        else if (named.count == 3) slot = SkV3{named.v[0], named.v[1], named.v[2]};
        else slot = SkV4{named.v[0], named.v[1], named.v[2], named.v[3]};
    }
    auto shader = builder.makeShader();
    if (!shader) return false;
    SkPaint paint; paint.setShader(std::move(shader));
    canvas_->save(); canvas_->translate(x, y);
    canvas_->drawRect(SkRect::MakeXYWH(-options.reach, -options.reach,
                                       w + 2.0f * options.reach,
                                       h + 2.0f * options.reach), paint); canvas_->restore();
    return true;
}

bool SkiaCanvas::save_layer_with_sksl_post_effect(
        float x, float y, float w, float h,
        const std::string& sksl, const ShaderUniforms& uniforms,
        float sample_radius,
        const std::vector<NamedUniform>& extra_uniforms,
        Canvas::BlendMode blend_mode) {
    // On any failure (no canvas, empty/invalid SkSL, or a shader that does not
    // declare the `content` child) fall back to a plain unfiltered layer so the
    // subtree still renders, and return false so the caller can log once.
    auto plain_fallback = [&]() -> bool {
        save_layer(x, y, w, h, 1.0f, 0.0f);
        return false;
    };
    if (!canvas_ || sksl.empty()) return plain_fallback();

    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(sksl);
    if (!effect) return plain_fallback();
    // The compositor binds the layer content to a child shader named "content".
    // A generative shader without that child cannot post-process anything.
    if (!effect->findChild("content")) return plain_fallback();

    SkRuntimeShaderBuilder builder(effect);
    // Fixed widget vocabulary (bound when declared).
    if (effect->findUniform("resolution"))
        builder.uniform("resolution") = SkV2{w, h};
    if (effect->findUniform("value"))
        builder.uniform("value") = uniforms.value;
    if (effect->findUniform("time"))
        builder.uniform("time") = uniforms.time;
    auto toSkV4 = [](Color c) -> SkV4 { return {c.r, c.g, c.b, c.a}; };
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

    // Arbitrary caller-named uniforms — the real user-facing surface. Each is
    // bound only if the shader actually declares it (findUniform guard), so an
    // unused name is silently ignored rather than a compile/bind error.
    for (const auto& nu : extra_uniforms) {
        if (!effect->findUniform(nu.name.c_str())) continue;
        switch (nu.count) {
            case 1: builder.uniform(nu.name.c_str()) = nu.v[0]; break;
            case 2: builder.uniform(nu.name.c_str()) = SkV2{nu.v[0], nu.v[1]}; break;
            case 3: builder.uniform(nu.name.c_str()) = SkV3{nu.v[0], nu.v[1], nu.v[2]}; break;
            case 4: builder.uniform(nu.name.c_str()) = SkV4{nu.v[0], nu.v[1], nu.v[2], nu.v[3]}; break;
            default: break;
        }
    }

    // Bind the layer's rendered content as the "content" child (input=nullptr =
    // implicit source image) and hang the runtime shader off the layer paint via
    // push_layer, which also applies the composite blend mode (e.g.
    // BlendMode::lighter for an additive glow) when the layer restores.
    sk_sp<SkImageFilter> filter = SkImageFilters::RuntimeShader(
        builder, std::max(sample_radius, 0.0f), "content", nullptr);
    if (!filter) return plain_fallback();

    push_layer(x, y, w, h, /*opacity=*/1.0f, /*blur=*/0.0f, blend_mode,
               std::move(filter));
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
#if !PULP_CANVAS_GRAPHITE
    // Ganesh-only build (Emscripten / WebGL2): there is no Dawn texture to
    // wrap and no Graphite recorder to wrap it with. Report the draw as
    // unhandled so the widget paint path takes its non-GPU branch.
    (void)texture_handle; (void)width; (void)height; (void)format;
    (void)x; (void)y; (void)w; (void)h;
    return false;
#else
    // WebGPU canvas widgets provide a `wgpu::Texture*` that must be wrapped
    // as a Graphite backend texture, materialized as an SkImage, and drawn
    // into the current Skia canvas. Returning true tells the widget paint path
    // that the offscreen Dawn texture reached the presentable surface.
    if (!canvas_ || !recorder_ || !texture_handle || width == 0 || height == 0) {
        return false;
    }

    auto* texture = static_cast<wgpu::Texture*>(texture_handle);
    if (!texture || !(*texture)) {
        return false;
    }

    auto backend_tex = skgpu::graphite::BackendTextures::MakeDawn(texture->Get());
    if (!backend_tex.isValid()) {
        return false;
    }

    auto image = SkImages::WrapTexture(recorder_,
                                       backend_tex,
                                       sk_color_type_from_webgpu_format(format),
                                       kPremul_SkAlphaType,
                                       sk_color_space_from_webgpu_format(format),
                                       skgpu::Origin::kTopLeft,
                                       nullptr,
                                       nullptr,
                                       "Pulp native GPU canvas");
    if (!image) {
        return false;
    }

    canvas_->drawImageRect(image,
                           SkRect::MakeXYWH(x, y, w, h),
                           sampling_options_for_image_smoothing());
    return true;
#endif
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
