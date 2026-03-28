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
#include "include/effects/SkRuntimeEffect.h"

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
    return SkColorSetARGB(c.a, c.r, c.g, c.b);
}

static SkPaint make_fill_paint(Color c) {
    SkPaint paint;
    paint.setColor(to_sk_color(c));
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    return paint;
}

static SkPaint make_stroke_paint(Color c, float width) {
    SkPaint paint;
    paint.setColor(to_sk_color(c));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setAntiAlias(true);
    return paint;
}

SkiaCanvas::SkiaCanvas(SkCanvas* canvas) : canvas_(canvas) {}
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
    SkFont font;
    font.setSize(font_size_);

    // Use platform font manager to find the requested typeface
    auto mgr = get_font_manager();
    if (mgr) {
        auto typeface = mgr->matchFamilyStyle(font_family_.c_str(), SkFontStyle::Normal());
        if (typeface) font.setTypeface(std::move(typeface));
    }

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
    SkFont font;
    font.setSize(font_size_);

    SkRect bounds;
    font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
    return bounds.width();
}

Canvas::TextMetrics SkiaCanvas::measure_text_full(const std::string& text) {
    SkFont font;
    font.setSize(font_size_);

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
        style.line_color.r / 255.0f, style.line_color.g / 255.0f,
        style.line_color.b / 255.0f, style.line_color.a / 255.0f};
    builder.uniform("fillColor") = SkV4{
        style.fill_color.r / 255.0f, style.fill_color.g / 255.0f,
        style.fill_color.b / 255.0f, style.fill_color.a / 255.0f};

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
