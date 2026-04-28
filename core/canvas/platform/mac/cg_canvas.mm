#include <pulp/canvas/cg_canvas.hpp>

#ifdef __APPLE__

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace pulp::canvas {

CoreGraphicsCanvas::CoreGraphicsCanvas(CGContextRef ctx, float width, float height)
    : ctx_(ctx), width_(width), height_(height) {
    // Flip coordinate system (CG is bottom-up, we use top-down)
    CGContextTranslateCTM(ctx_, 0, height);
    CGContextScaleCTM(ctx_, 1.0, -1.0);
}

CoreGraphicsCanvas::~CoreGraphicsCanvas() = default;

void CoreGraphicsCanvas::save() { CGContextSaveGState(ctx_); }
void CoreGraphicsCanvas::restore() {
    if (in_transparency_layer_ > 0) {
        CGContextEndTransparencyLayer(ctx_);
        --in_transparency_layer_;
    }
    CGContextRestoreGState(ctx_);
}

void CoreGraphicsCanvas::translate(float x, float y) {
    CGContextTranslateCTM(ctx_, x, y);
}

void CoreGraphicsCanvas::scale(float sx, float sy) {
    CGContextScaleCTM(ctx_, sx, sy);
}

void CoreGraphicsCanvas::rotate(float radians) {
    CGContextRotateCTM(ctx_, radians);
}

void CoreGraphicsCanvas::clip_rect(float x, float y, float w, float h) {
    CGContextClipToRect(ctx_, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvas::apply_fill_color() {
    CGContextSetRGBFillColor(ctx_,
        fill_color_.r, fill_color_.g,
        fill_color_.b, fill_color_.a);
}

void CoreGraphicsCanvas::apply_stroke_color() {
    CGContextSetRGBStrokeColor(ctx_,
        stroke_color_.r, stroke_color_.g,
        stroke_color_.b, stroke_color_.a);
}

void CoreGraphicsCanvas::set_fill_color(Color c) { fill_color_ = c; }
void CoreGraphicsCanvas::set_stroke_color(Color c) { stroke_color_ = c; }

void CoreGraphicsCanvas::set_line_width(float w) {
    CGContextSetLineWidth(ctx_, w);
}

void CoreGraphicsCanvas::set_line_cap(LineCap cap) {
    CGLineCap cg_cap = kCGLineCapButt;
    switch (cap) {
        case LineCap::butt: cg_cap = kCGLineCapButt; break;
        case LineCap::round: cg_cap = kCGLineCapRound; break;
        case LineCap::square: cg_cap = kCGLineCapSquare; break;
    }
    CGContextSetLineCap(ctx_, cg_cap);
}

void CoreGraphicsCanvas::set_line_join(LineJoin join) {
    CGLineJoin cg_join = kCGLineJoinMiter;
    switch (join) {
        case LineJoin::miter: cg_join = kCGLineJoinMiter; break;
        case LineJoin::round: cg_join = kCGLineJoinRound; break;
        case LineJoin::bevel: cg_join = kCGLineJoinBevel; break;
    }
    CGContextSetLineJoin(ctx_, cg_join);
}

void CoreGraphicsCanvas::fill_rect(float x, float y, float w, float h) {
    apply_fill_color();
    CGContextFillRect(ctx_, CGRectMake(x, y, w, h));
}

// pulp #929 — clearRect must replace destination pixels with transparent
// black, not SrcOver-blend a transparent fill (which CoreGraphics would
// treat as a no-op the same way Skia does). CGContextClearRect zeroes
// the alpha channel of the destination region directly, mirroring the
// CanvasRenderingContext2D.clearRect spec for the CoreGraphics-backed
// macOS / iOS render paths.
void CoreGraphicsCanvas::clear_rect(float x, float y, float w, float h) {
    CGContextClearRect(ctx_, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvas::stroke_rect(float x, float y, float w, float h) {
    apply_stroke_color();
    CGContextStrokeRect(ctx_, CGRectMake(x, y, w, h));
}

void CoreGraphicsCanvas::add_rounded_rect_path(float x, float y, float w, float h, float r) {
    r = std::min(r, std::min(w, h) * 0.5f);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, x + r, y);
    CGContextAddLineToPoint(ctx_, x + w - r, y);
    CGContextAddArc(ctx_, x + w - r, y + r, r, -M_PI_2, 0, 0);
    CGContextAddLineToPoint(ctx_, x + w, y + h - r);
    CGContextAddArc(ctx_, x + w - r, y + h - r, r, 0, M_PI_2, 0);
    CGContextAddLineToPoint(ctx_, x + r, y + h);
    CGContextAddArc(ctx_, x + r, y + h - r, r, M_PI_2, M_PI, 0);
    CGContextAddLineToPoint(ctx_, x, y + r);
    CGContextAddArc(ctx_, x + r, y + r, r, M_PI, 3 * M_PI_2, 0);
    CGContextClosePath(ctx_);
}

void CoreGraphicsCanvas::fill_rounded_rect(float x, float y, float w, float h, float radius) {
    add_rounded_rect_path(x, y, w, h, radius);
    apply_fill_color();
    CGContextFillPath(ctx_);
}

void CoreGraphicsCanvas::stroke_rounded_rect(float x, float y, float w, float h, float radius) {
    add_rounded_rect_path(x, y, w, h, radius);
    apply_stroke_color();
    CGContextStrokePath(ctx_);
}

void CoreGraphicsCanvas::fill_circle(float cx, float cy, float radius) {
    apply_fill_color();
    CGContextFillEllipseInRect(ctx_, CGRectMake(cx - radius, cy - radius, radius * 2, radius * 2));
}

void CoreGraphicsCanvas::stroke_circle(float cx, float cy, float radius) {
    apply_stroke_color();
    CGContextStrokeEllipseInRect(ctx_, CGRectMake(cx - radius, cy - radius, radius * 2, radius * 2));
}

void CoreGraphicsCanvas::stroke_arc(float cx, float cy, float radius,
                                   float start_angle, float end_angle) {
    apply_stroke_color();
    CGContextBeginPath(ctx_);
    // CG uses clockwise=0 for counterclockwise, but our coords are flipped
    CGContextAddArc(ctx_, cx, cy, radius, start_angle, end_angle, 0);
    CGContextStrokePath(ctx_);
}

void CoreGraphicsCanvas::stroke_line(float x0, float y0, float x1, float y1) {
    apply_stroke_color();
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, x0, y0);
    CGContextAddLineToPoint(ctx_, x1, y1);
    CGContextStrokePath(ctx_);
}

void CoreGraphicsCanvas::stroke_path(const Point2D* points, size_t count) {
    if (count < 2) return;
    apply_stroke_color();
    CGContextSetShouldAntialias(ctx_, true);
    CGContextSetAllowsAntialiasing(ctx_, true);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i)
        CGContextAddLineToPoint(ctx_, points[i].x, points[i].y);
    CGContextStrokePath(ctx_);
}

void CoreGraphicsCanvas::fill_path(const Point2D* points, size_t count) {
    if (count < 3) return;
    apply_fill_color();
    CGContextSetShouldAntialias(ctx_, true);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i)
        CGContextAddLineToPoint(ctx_, points[i].x, points[i].y);
    CGContextClosePath(ctx_);
    CGContextFillPath(ctx_);
}

void CoreGraphicsCanvas::set_opacity(float alpha) {
    CGContextSetAlpha(ctx_, alpha);
}

void CoreGraphicsCanvas::save_layer(float x, float y, float w, float h,
                                     float opacity, float blur_radius) {
    // CoreGraphics transparency layers composite the subtree as a single unit.
    // CGContextBeginTransparencyLayer must be paired with CGContextEndTransparencyLayer.
    // We use save/restore to scope the alpha, and the transparency layer handles
    // proper group compositing.
    (void)x; (void)y; (void)w; (void)h;  // CG transparency layer uses clip, not explicit rect
    (void)blur_radius;  // CG blur would need CIFilter, not implemented here
    CGContextSaveGState(ctx_);
    CGContextSetAlpha(ctx_, opacity);
    CGContextBeginTransparencyLayer(ctx_, nullptr);
    ++in_transparency_layer_;
}

void CoreGraphicsCanvas::set_font(const std::string& family, float size) {
    font_family_ = family;
    font_size_ = size;
}

void CoreGraphicsCanvas::set_text_align(TextAlign align) {
    text_align_ = align;
}

static CTFontRef create_font_with_fallback(const std::string& family, float size) {
    NSString* ns_font = [NSString stringWithUTF8String:family.c_str()];
    CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)ns_font, size, NULL);
    if (!font) {
        // Requested family not available — fall back to system font
        font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, NULL);
    }
    return font;
}

void CoreGraphicsCanvas::fill_text(const std::string& text, float x, float y) {
    @autoreleasepool {
        NSString* ns_text = [NSString stringWithUTF8String:text.c_str()];

        CTFontRef font = create_font_with_fallback(font_family_, font_size_);
        if (!font) return;

        NSDictionary* attrs = @{
            (__bridge id)kCTFontAttributeName: (__bridge id)font,
            (__bridge id)kCTForegroundColorFromContextAttributeName: @YES
        };

        NSAttributedString* attr_str = [[NSAttributedString alloc]
            initWithString:ns_text attributes:attrs];

        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attr_str);

        // Measure for alignment
        CGFloat text_width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        float draw_x = x;
        switch (text_align_) {
            case TextAlign::center: draw_x = x - text_width * 0.5f; break;
            case TextAlign::right: draw_x = x - text_width; break;
            case TextAlign::left: break;
        }

        // CoreText draws bottom-up, but we've flipped the context.
        // Need to flip text back to render correctly.
        CGContextSaveGState(ctx_);
        apply_fill_color();
        CGContextTranslateCTM(ctx_, draw_x, y);
        CGContextScaleCTM(ctx_, 1.0, -1.0);
        CGContextSetTextPosition(ctx_, 0, 0);
        CTLineDraw(line, ctx_);
        CGContextRestoreGState(ctx_);

        CFRelease(line);
        CFRelease(font);
    }
}

float CoreGraphicsCanvas::measure_text(const std::string& text) {
    @autoreleasepool {
        NSString* ns_text = [NSString stringWithUTF8String:text.c_str()];

        CTFontRef font = create_font_with_fallback(font_family_, font_size_);
        if (!font) return 0;

        NSDictionary* attrs = @{
            (__bridge id)kCTFontAttributeName: (__bridge id)font
        };

        NSAttributedString* attr_str = [[NSAttributedString alloc]
            initWithString:ns_text attributes:attrs];

        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attr_str);

        CGFloat width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        CFRelease(line);
        CFRelease(font);
        return static_cast<float>(width);
    }
}

Canvas::TextMetrics CoreGraphicsCanvas::measure_text_full(const std::string& text) {
    @autoreleasepool {
        NSString* ns_text = [NSString stringWithUTF8String:text.c_str()];

        CTFontRef font = create_font_with_fallback(font_family_, font_size_);
        if (!font) return {};

        NSDictionary* attrs = @{
            (__bridge id)kCTFontAttributeName: (__bridge id)font
        };

        NSAttributedString* attr_str = [[NSAttributedString alloc]
            initWithString:ns_text attributes:attrs];

        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attr_str);

        CGFloat ascent, descent, leading;
        CGFloat width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

        TextMetrics m;
        m.width = static_cast<float>(width);
        m.ascent = static_cast<float>(ascent);
        m.descent = static_cast<float>(descent);
        m.line_height = static_cast<float>(ascent + descent + leading);

        CFRelease(line);
        CFRelease(font);
        return m;
    }
}

} // namespace pulp::canvas

#endif // __APPLE__
