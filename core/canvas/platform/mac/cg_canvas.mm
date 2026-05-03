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

CoreGraphicsCanvas::~CoreGraphicsCanvas() {
    release_path();
}

void CoreGraphicsCanvas::release_path() {
    if (path_) {
        CGPathRelease(path_);
        path_ = nullptr;
    }
}

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

// pulp #943 (#933 P1) — concat the supplied affine onto the current CTM, do
// NOT replace it. View::paint_all() routes JS-supplied setTransform(id, ...)
// through Canvas::concat_transform so the View's transform composes with the
// parent View's transform (the bounds translate, outer transforms, etc.)
// rather than wiping it. Mirrors SkCanvas::concat / SkiaCanvas::concat_transform.
//
// CanvasRenderingContext2D affine matrix layout:
//   [ a c e ]
//   [ b d f ]
//   [ 0 0 1 ]
// CGAffineTransformMake takes (a, b, c, d, tx, ty) in the same column-major
// convention used by CanvasRenderingContext2D.setTransform / DOMMatrix, so the
// six floats map 1:1 with no transposition needed.
// CGContextConcatCTM right-multiplies the supplied transform onto the CTM,
// matching SkCanvas::concat semantics.
void CoreGraphicsCanvas::concat_transform(float a, float b, float c,
                                          float d, float e, float f) {
    CGContextConcatCTM(ctx_, CGAffineTransformMake(a, b, c, d, e, f));
}

// pulp #1322 — JS-driven setTransform must compose onto the paint baseline
// captured at CanvasWidget::paint() entry, mirroring SkiaCanvas's behavior.
// Without this override, the base-class no-op silently drops every JS
// setTransform call on the CPU paint path (which is what standalone uses
// when use_gpu=false), so any subsequent fill/stroke draws at the wrong
// place — or, far more often, at (0,0) and gets invisibly clipped.
void CoreGraphicsCanvas::set_transform(float a, float b, float c,
                                       float d, float e, float f) {
    // Compose user matrix onto the captured baseline:
    //   final = baseline * user
    // CGAffineTransformConcat(t1, t2) returns t1 * t2 (left-multiplication
    // of t1 by t2). The baseline is the CTM at paint() entry; user is the
    // matrix the JS code is asking for in canvas-local space.
    CGAffineTransform user = CGAffineTransformMake(a, b, c, d, e, f);
    if (has_baseline_) {
        CGAffineTransform baseline = CGAffineTransformMake(
            static_cast<CGFloat>(baseline_xform_[0]),
            static_cast<CGFloat>(baseline_xform_[1]),
            static_cast<CGFloat>(baseline_xform_[2]),
            static_cast<CGFloat>(baseline_xform_[3]),
            static_cast<CGFloat>(baseline_xform_[4]),
            static_cast<CGFloat>(baseline_xform_[5]));
        CGAffineTransform composed = CGAffineTransformConcat(user, baseline);
        // Replace CTM: invert current and concat the desired final matrix.
        CGAffineTransform inv = CGAffineTransformInvert(CGContextGetCTM(ctx_));
        CGContextConcatCTM(ctx_, inv);
        CGContextConcatCTM(ctx_, composed);
    } else {
        // No baseline captured — replace CTM with user matrix outright. This
        // matches the default Canvas2D semantic that setTransform replaces.
        CGAffineTransform inv = CGAffineTransformInvert(CGContextGetCTM(ctx_));
        CGContextConcatCTM(ctx_, inv);
        CGContextConcatCTM(ctx_, user);
    }
}

void CoreGraphicsCanvas::capture_paint_baseline_transform() {
    CGAffineTransform t = CGContextGetCTM(ctx_);
    baseline_xform_[0] = static_cast<double>(t.a);
    baseline_xform_[1] = static_cast<double>(t.b);
    baseline_xform_[2] = static_cast<double>(t.c);
    baseline_xform_[3] = static_cast<double>(t.d);
    baseline_xform_[4] = static_cast<double>(t.tx);
    baseline_xform_[5] = static_cast<double>(t.ty);
    has_baseline_ = true;
}

void CoreGraphicsCanvas::clip_rect(float x, float y, float w, float h) {
    CGContextClipToRect(ctx_, CGRectMake(x, y, w, h));
}

// pulp #1322 — clip() intersects the current clip region with the path
// being built via begin_path/move_to/line_to/etc. Mirrors
// CanvasRenderingContext2D.clip() and SkiaCanvas::clip(). Without the
// override, the base-class no-op silently leaves the clip region wide open
// so subsequent draws spill over their intended bounds.
void CoreGraphicsCanvas::clip() {
    if (!path_) return;
    CGContextAddPath(ctx_, path_);
    // Use even-odd? Canvas2D defaults to non-zero winding for clip().
    CGContextClip(ctx_);
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
    if (has_gradient_) {
        CGContextSaveGState(ctx_);
        CGContextClipToRect(ctx_, CGRectMake(x, y, w, h));
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
        return;
    }
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

// ── Canvas2D path builder (pulp #1322) ───────────────────────────────────────
//
// Background. CanvasWidget JS code drives draw via the HTML5 Canvas2D-style
// path API: beginPath, moveTo, lineTo, quadTo, cubicTo, closePath, then
// fill() / stroke(). The base Canvas class default-implements every one of
// these as a no-op so backends that don't have a real path builder still
// compile, but it means the CoreGraphics CPU paint path used by Pulp's
// standalone host (when use_gpu=false) silently dropped the entire JS draw
// program. Spectr's FilterBank canvas issues 1800+ such commands per frame
// and the result was a fully-white window — see the issue thread for the
// full repro.
//
// Implementation: mirror SkiaCanvas's approach but with CGMutablePath.
// We hold a CGMutablePathRef per begin_path() call, append segments as
// the JS bridge calls into us, and on fill_current_path / stroke_current_path
// we hand the path to CGContextAddPath + CGContextFillPath / CGContextStrokePath.
// The path is released when the canvas is destroyed.
//
// Note: CGContext maintains its own internal path that the CG-shape draws
// (fill_rect, fill_circle, fill_rounded_rect) build and consume. We keep
// that internal path separate from path_ — the JS-driven path lives in
// path_ and is only flushed to CG when fill_current_path / stroke_current_path
// fire. fill_rect etc. continue to use the CG implicit path.

void CoreGraphicsCanvas::begin_path() {
    release_path();
    path_ = CGPathCreateMutable();
}

void CoreGraphicsCanvas::move_to(float x, float y) {
    if (!path_) path_ = CGPathCreateMutable();
    CGPathMoveToPoint(path_, NULL, x, y);
}

void CoreGraphicsCanvas::line_to(float x, float y) {
    if (!path_) path_ = CGPathCreateMutable();
    CGPathAddLineToPoint(path_, NULL, x, y);
}

void CoreGraphicsCanvas::quad_to(float cpx, float cpy, float x, float y) {
    if (!path_) path_ = CGPathCreateMutable();
    CGPathAddQuadCurveToPoint(path_, NULL, cpx, cpy, x, y);
}

void CoreGraphicsCanvas::cubic_to(float cp1x, float cp1y,
                                   float cp2x, float cp2y,
                                   float x, float y) {
    if (!path_) path_ = CGPathCreateMutable();
    CGPathAddCurveToPoint(path_, NULL, cp1x, cp1y, cp2x, cp2y, x, y);
}

void CoreGraphicsCanvas::close_path() {
    if (path_) CGPathCloseSubpath(path_);
}

void CoreGraphicsCanvas::fill_current_path() {
    if (!path_) return;
    if (has_gradient_) {
        // Clip to the path, then draw the gradient; restore the clip.
        CGContextSaveGState(ctx_);
        CGContextAddPath(ctx_, path_);
        CGContextClip(ctx_);
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
    } else {
        apply_fill_color();
        CGContextAddPath(ctx_, path_);
        CGContextFillPath(ctx_);
    }
    // Mirrors SkiaCanvas::fill_current_path which detaches the path on use —
    // the next draw must begin_path() again, matching Canvas2D semantics.
    release_path();
}

void CoreGraphicsCanvas::stroke_current_path() {
    if (!path_) return;
    apply_stroke_color();
    CGContextAddPath(ctx_, path_);
    CGContextStrokePath(ctx_);
    release_path();
}

// ── Gradient fills (pulp #1322) ──────────────────────────────────────────────
//
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
    gradient_is_radial_ = true;
    grad_x0_ = cx; grad_y0_ = cy; grad_radius_ = radius;
    grad_colors_.assign(colors, colors + count);
    grad_positions_.assign(positions, positions + count);
}

void CoreGraphicsCanvas::clear_fill_gradient() {
    has_gradient_ = false;
    grad_colors_.clear();
    grad_positions_.clear();
}

void CoreGraphicsCanvas::fill_with_active_paint() {
    if (!has_gradient_ || grad_colors_.empty()) {
        apply_fill_color();
        return;
    }
    const size_t n = grad_colors_.size();
    std::vector<CGFloat> components(n * 4);
    std::vector<CGFloat> locations(n);
    for (size_t i = 0; i < n; ++i) {
        components[i * 4 + 0] = grad_colors_[i].r;
        components[i * 4 + 1] = grad_colors_[i].g;
        components[i * 4 + 2] = grad_colors_[i].b;
        components[i * 4 + 3] = grad_colors_[i].a;
        locations[i] = grad_positions_[i];
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return;
    CGGradientRef gradient = CGGradientCreateWithColorComponents(
        cs, components.data(), locations.data(), n);
    CGColorSpaceRelease(cs);
    if (!gradient) return;

    const CGGradientDrawingOptions opts =
        kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation;
    if (gradient_is_radial_) {
        CGContextDrawRadialGradient(ctx_,
            gradient,
            CGPointMake(grad_x0_, grad_y0_), 0.0,
            CGPointMake(grad_x0_, grad_y0_), grad_radius_,
            opts);
    } else {
        CGContextDrawLinearGradient(ctx_,
            gradient,
            CGPointMake(grad_x0_, grad_y0_),
            CGPointMake(grad_x1_, grad_y1_),
            opts);
    }
    CGGradientRelease(gradient);
}

} // namespace pulp::canvas

#endif // __APPLE__
