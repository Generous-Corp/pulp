#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkRRect.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkData.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkGradientShader.h"
#include "runtime_effect_cache.hpp"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkBlendMode.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "webgpu/webgpu_cpp.h"

// Platform font manager
#ifdef __APPLE__
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/core/SkFontScanner.h"
#endif

namespace pulp::canvas {

// Lazily create a platform-appropriate font manager
// macOS: CoreText, Windows: DirectWrite, Linux: fontconfig
static sk_sp<SkFontMgr> get_font_manager() {
    static sk_sp<SkFontMgr> mgr;
    if (!mgr) {
#ifdef __APPLE__
        mgr = SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
        mgr = SkFontMgr_New_DirectWrite();
#elif defined(__linux__)
        mgr = SkFontMgr_New_FontConfig(nullptr, nullptr);
#else
        mgr = SkFontMgr::RefEmpty();
#endif
    }
    return mgr;
}

static SkColor to_sk_color(Color c) {
    return c.to_argb32();
}

static SkColor4f to_sk_color4f(Color c) {
    return {c.r, c.g, c.b, c.a};
}

static SkPaint make_fill_paint(Color c) {
    SkPaint paint;
    paint.setColor4f(to_sk_color4f(c));
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    return paint;
}

static SkPaint make_stroke_paint(Color c, float width) {
    SkPaint paint;
    paint.setColor4f(to_sk_color4f(c));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setAntiAlias(true);
    return paint;
}

static SkFont make_font(const std::string& family, float size) {
    SkFont font;
    font.setSize(size);

    auto mgr = get_font_manager();
    if (mgr) {
        auto typeface = mgr->matchFamilyStyle(family.c_str(), SkFontStyle::Normal());
        if (typeface) font.setTypeface(std::move(typeface));
    }

    return font;
}

static SkColorType sk_color_type_from_webgpu_format(const std::string& format) {
    if (format == "rgba16float") return kRGBA_F16_SkColorType;
    if (format == "rgba8unorm" || format == "rgba8unorm-srgb") return kRGBA_8888_SkColorType;
    return kBGRA_8888_SkColorType;
}

static sk_sp<SkColorSpace> sk_color_space_from_webgpu_format(const std::string& format) {
    if (format == "rgba16float") return SkColorSpace::MakeSRGBLinear();
    return SkColorSpace::MakeSRGB();
}

SkiaCanvas::SkiaCanvas(SkCanvas* canvas, skgpu::graphite::Recorder* recorder)
    : canvas_(canvas), recorder_(recorder) {}
SkiaCanvas::~SkiaCanvas() = default;

void SkiaCanvas::save() { canvas_->save(); }
void SkiaCanvas::restore() { canvas_->restore(); }

void SkiaCanvas::translate(float x, float y) { canvas_->translate(x, y); }
void SkiaCanvas::scale(float sx, float sy) { canvas_->scale(sx, sy); }
void SkiaCanvas::rotate(float radians) {
    canvas_->rotate(radians * 180.0f / 3.14159265f); // Skia uses degrees
}

void SkiaCanvas::clip_rect(float x, float y, float w, float h) {
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SkiaCanvas::set_fill_color(Color c) { fill_color_ = c; }
void SkiaCanvas::set_stroke_color(Color c) { stroke_color_ = c; }
void SkiaCanvas::set_line_width(float w) { line_width_ = w; }

void SkiaCanvas::set_line_cap(LineCap cap) {
    line_cap_ = cap;
}

void SkiaCanvas::set_line_join(LineJoin join) {
    line_join_ = join;
}

void SkiaCanvas::fill_rect(float x, float y, float w, float h) {
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), make_fill_paint(fill_color_));
}

void SkiaCanvas::stroke_rect(float x, float y, float w, float h) {
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void SkiaCanvas::fill_rounded_rect(float x, float y, float w, float h, float radius) {
    SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius);
    canvas_->drawRRect(rrect, make_fill_paint(fill_color_));
}

void SkiaCanvas::stroke_rounded_rect(float x, float y, float w, float h, float radius) {
    SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius);
    canvas_->drawRRect(rrect, make_stroke_paint(stroke_color_, line_width_));
}

void SkiaCanvas::fill_circle(float cx, float cy, float radius) {
    canvas_->drawCircle(cx, cy, radius, make_fill_paint(fill_color_));
}

void SkiaCanvas::stroke_circle(float cx, float cy, float radius) {
    canvas_->drawCircle(cx, cy, radius, make_stroke_paint(stroke_color_, line_width_));
}

void SkiaCanvas::stroke_arc(float cx, float cy, float radius,
                           float start_angle, float end_angle) {
    float start_deg = start_angle * 180.0f / 3.14159265f;
    float sweep_deg = (end_angle - start_angle) * 180.0f / 3.14159265f;
    SkRect oval = SkRect::MakeXYWH(cx - radius, cy - radius, radius * 2, radius * 2);
    SkPath path = SkPathBuilder().addArc(oval, start_deg, sweep_deg).detach();
    canvas_->drawPath(path, make_stroke_paint(stroke_color_, line_width_));
}

void SkiaCanvas::stroke_line(float x0, float y0, float x1, float y1) {
    canvas_->drawLine(x0, y0, x1, y1, make_stroke_paint(stroke_color_, line_width_));
}

void SkiaCanvas::set_font(const std::string& family, float size) {
    font_family_ = family;
    font_size_ = size;
}

void SkiaCanvas::set_text_align(TextAlign align) {
    text_align_ = align;
}

void SkiaCanvas::fill_text(const std::string& text, float x, float y) {
    SkFont font = make_font(font_family_, font_size_);

    auto paint = make_fill_paint(fill_color_);

    // Handle text alignment
    float draw_x = x;
    if (text_align_ != TextAlign::left) {
        SkRect bounds;
        font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
        if (text_align_ == TextAlign::center) draw_x -= bounds.width() * 0.5f;
        else if (text_align_ == TextAlign::right) draw_x -= bounds.width();
    }

    canvas_->drawSimpleText(text.c_str(), text.size(), SkTextEncoding::kUTF8,
                           draw_x, y, font, paint);
}

float SkiaCanvas::measure_text(const std::string& text) {
    SkFont font = make_font(font_family_, font_size_);

    SkRect bounds;
    font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
    return bounds.width();
}

Canvas::TextMetrics SkiaCanvas::measure_text_full(const std::string& text) {
    SkFont font = make_font(font_family_, font_size_);

    SkFontMetrics sk_metrics;
    font.getMetrics(&sk_metrics);

    SkRect bounds;
    font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);

    TextMetrics m;
    m.width = bounds.width();
    m.ascent = -sk_metrics.fAscent;   // Skia ascent is negative, we return positive
    m.descent = sk_metrics.fDescent;  // Skia descent is positive
    m.line_height = -sk_metrics.fAscent + sk_metrics.fDescent + sk_metrics.fLeading;
    return m;
}

// ── Images ──────────────────────────────────────────────────────────────────

bool SkiaCanvas::draw_image_from_data(const uint8_t* data, size_t size,
                                       float x, float y, float w, float h) {
    if (!canvas_ || !data || size == 0) return false;

    auto sk_data = SkData::MakeWithoutCopy(data, size);
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;

    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           SkSamplingOptions(SkFilterMode::kLinear));
    return true;
}

bool SkiaCanvas::draw_image_from_file(const std::string& path,
                                       float x, float y, float w, float h) {
    if (!canvas_ || path.empty()) return false;

    auto sk_data = SkData::MakeFromFileName(path.c_str());
    if (!sk_data) return false;

    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;

    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           SkSamplingOptions(SkFilterMode::kLinear));
    return true;
}

// ── Gradients ────────────────────────────────────────────────────────────────

static void colors_to_skia(const Color* colors, const float* positions, int count,
                            std::vector<SkColor>& sk_colors, std::vector<SkScalar>& sk_pos) {
    sk_colors.resize(static_cast<size_t>(count));
    sk_pos.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        sk_colors[static_cast<size_t>(i)] = colors[i].to_argb32();
        sk_pos[static_cast<size_t>(i)] = positions[i];
    }
}

void SkiaCanvas::set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                           const Color* colors, const float* positions, int count) {
    std::vector<SkColor> sk_colors;
    std::vector<SkScalar> sk_pos;
    colors_to_skia(colors, positions, count, sk_colors, sk_pos);
    SkPoint pts[2] = {{x0, y0}, {x1, y1}};
    gradient_shader_ = SkGradientShader::MakeLinear(pts, sk_colors.data(), sk_pos.data(), count,
                                                     SkTileMode::kClamp);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_fill_gradient_radial(float cx, float cy, float radius,
                                           const Color* colors, const float* positions, int count) {
    std::vector<SkColor> sk_colors;
    std::vector<SkScalar> sk_pos;
    colors_to_skia(colors, positions, count, sk_colors, sk_pos);
    gradient_shader_ = SkGradientShader::MakeRadial({cx, cy}, radius, sk_colors.data(),
                                                     sk_pos.data(), count, SkTileMode::kClamp);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_fill_gradient_conic(float cx, float cy, float start_angle,
                                          const Color* colors, const float* positions, int count) {
    std::vector<SkColor> sk_colors;
    std::vector<SkScalar> sk_pos;
    colors_to_skia(colors, positions, count, sk_colors, sk_pos);
    float start_deg = start_angle * 180.0f / 3.14159265f;
    gradient_shader_ = SkGradientShader::MakeSweep(cx, cy, sk_colors.data(), sk_pos.data(),
                                                     count, SkTileMode::kClamp,
                                                     start_deg, start_deg + 360.0f, 0, nullptr);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::clear_fill_gradient() {
    gradient_shader_ = nullptr;
    has_gradient_ = false;
}

// ── Blend modes ─────────────────────────────────────────────────────────────

void SkiaCanvas::set_blend_mode(BlendMode mode) {
    static const SkBlendMode map[] = {
        SkBlendMode::kSrcOver, SkBlendMode::kMultiply, SkBlendMode::kScreen,
        SkBlendMode::kOverlay, SkBlendMode::kDarken, SkBlendMode::kLighten,
        SkBlendMode::kColorDodge, SkBlendMode::kColorBurn, SkBlendMode::kHardLight,
        SkBlendMode::kSoftLight, SkBlendMode::kDifference, SkBlendMode::kExclusion,
        SkBlendMode::kHue, SkBlendMode::kSaturation, SkBlendMode::kColor,
        SkBlendMode::kLuminosity
    };
    blend_mode_ = map[static_cast<int>(mode)];
}

// ── Path building ───────────────────────────────────────────────────────────

void SkiaCanvas::begin_path() {
    path_builder_ = std::make_unique<SkPathBuilder>();
}

void SkiaCanvas::move_to(float x, float y) {
    if (path_builder_) path_builder_->moveTo(x, y);
}

void SkiaCanvas::line_to(float x, float y) {
    if (path_builder_) path_builder_->lineTo(x, y);
}

void SkiaCanvas::quad_to(float cpx, float cpy, float x, float y) {
    if (path_builder_) path_builder_->quadTo(cpx, cpy, x, y);
}

void SkiaCanvas::cubic_to(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    if (path_builder_) path_builder_->cubicTo(cp1x, cp1y, cp2x, cp2y, x, y);
}

void SkiaCanvas::close_path() {
    if (path_builder_) path_builder_->close();
}

void SkiaCanvas::fill_current_path() {
    if (!canvas_ || !path_builder_) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    if (has_gradient_ && gradient_shader_) {
        paint.setShader(gradient_shader_);
    } else {
        paint.setColor4f(to_sk_color4f(fill_color_));
    }
    paint.setBlendMode(blend_mode_);
    canvas_->drawPath(path_builder_->detach(), paint);
}

void SkiaCanvas::stroke_current_path() {
    if (!canvas_ || !path_builder_) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setColor4f(to_sk_color4f(stroke_color_));
    paint.setStrokeWidth(line_width_);
    paint.setBlendMode(blend_mode_);
    canvas_->drawPath(path_builder_->detach(), paint);
}

// ── Static SkSL compilation (accessible from bridge without Canvas instance) ──

std::string Canvas::compile_sksl(const std::string& sksl) {
    if (sksl.empty()) return "Empty shader code";
    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(sksl);
    return effect ? "" : cache.last_error();
}

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

// ── Opacity & Compositing Layers ────────────────────────────────────────────

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

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
