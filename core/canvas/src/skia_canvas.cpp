#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkRRect.h"

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

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
