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
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

namespace pulp::canvas {

CoreGraphicsCanvas::CoreGraphicsCanvas(CGContextRef ctx, float width, float height)
    : ctx_(ctx), width_(width), height_(height) {
    // Flip coordinate system (CG is bottom-up, we use top-down)
    CGContextTranslateCTM(ctx_, 0, height);
    CGContextScaleCTM(ctx_, 1.0, -1.0);
}

CoreGraphicsCanvas::~CoreGraphicsCanvas() {
    release_path();
    release_conic_image();
    release_pattern_image();
}

void CoreGraphicsCanvas::release_conic_image() {
    if (conic_image_) {
        CGImageRelease(conic_image_);
        conic_image_ = nullptr;
    }
}

void CoreGraphicsCanvas::release_pattern_image() {
    if (pattern_image_) {
        CGImageRelease(pattern_image_);
        pattern_image_ = nullptr;
    }
    has_pattern_ = false;
}

void CoreGraphicsCanvas::release_path() {
    if (path_) {
        CGPathRelease(path_);
        path_ = nullptr;
    }
}

void CoreGraphicsCanvas::save() {
    CGContextSaveGState(ctx_);
    ++save_depth_;
}
void CoreGraphicsCanvas::restore() {
    if (in_transparency_layer_ > 0) {
        CGContextEndTransparencyLayer(ctx_);
        --in_transparency_layer_;
    }
    CGContextRestoreGState(ctx_);
    if (save_depth_ > 0) --save_depth_;
}

// pulp #1368 — pop GState frames repeatedly until depth matches `target`.
// CanvasWidget::paint() captures save_count() at entry and calls this at
// exit so an unbalanced JS draw script (one that emitted a `ctx.save()`
// but never reached its `ctx.restore()` due to an early-return path)
// can't leak GState — leftover transform/clip — into the parent View's
// paint scope where it would silently corrupt subsequent siblings.
void CoreGraphicsCanvas::restore_to_count(int target) {
    if (target < 0) target = 0;
    while (save_depth_ > target) {
        if (in_transparency_layer_ > 0) {
            CGContextEndTransparencyLayer(ctx_);
            --in_transparency_layer_;
        }
        CGContextRestoreGState(ctx_);
        --save_depth_;
    }
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

// pulp #1368 round 2 — diagnostic CTM snapshot for `PULP_LOG_CANVAS_PAINT=1`.
// Returns the current device matrix in CanvasRenderingContext2D affine order
// (a, b, c, d, e, f) so the env-gated CanvasWidget::paint logging can record
// what transform the inbound canvas has when paint() runs. CGAffineTransform
// already uses the same column-major convention as CanvasRenderingContext2D
// so the field mapping is 1:1.
CoreGraphicsCanvas::AffineTransform2x3 CoreGraphicsCanvas::current_transform() const {
    AffineTransform2x3 out;
    CGAffineTransform t = CGContextGetCTM(ctx_);
    out.a = static_cast<float>(t.a);
    out.b = static_cast<float>(t.b);
    out.c = static_cast<float>(t.c);
    out.d = static_cast<float>(t.d);
    out.e = static_cast<float>(t.tx);
    out.f = static_cast<float>(t.ty);
    return out;
}

void CoreGraphicsCanvas::clip_rect(float x, float y, float w, float h) {
    CGContextClipToRect(ctx_, CGRectMake(x, y, w, h));
}

// pulp #1322 — clip() intersects the current clip region with the path
// being built via begin_path/move_to/line_to/etc. Mirrors
// CanvasRenderingContext2D.clip() and SkiaCanvas::clip(). Without the
// override, the base-class no-op silently leaves the clip region wide open
// so subsequent draws spill over their intended bounds.
void CoreGraphicsCanvas::clip(FillRule rule) {
    if (!path_) return;
    CGContextAddPath(ctx_, path_);
    // pulp Wave 2 cheap wiring — honour the JS-supplied fillRule arg
    // (`ctx.clip('evenodd')`). CG distinguishes between CGContextClip
    // (nonzero winding) and CGContextEOClip (even-odd) at the API
    // surface; pre-Wave-2 the bridge always called CGContextClip.
    if (rule == FillRule::evenodd) {
        CGContextEOClip(ctx_);
    } else {
        CGContextClip(ctx_);
    }
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

// pulp #1898 — Canvas2D ctx.setLineDash() / lineDashOffset on the CG backend.
//
// Background. The base Canvas virtual is a no-op `(void)`, so before this
// override every dashed stroke on the macOS CPU paint path drew solid.
// SkiaCanvas applies the pattern via SkDashPathEffect on the per-call
// SkPaint (skia_canvas.cpp:1132-1209); CG has no per-paint dash state,
// only the GState-resident pair set by CGContextSetLineDash. We therefore
// store the intervals + phase on the canvas (mirroring Skia's fields) and
// push/clear the CG dash phase around each stroke draw via
// apply_line_dash_to_ctx() / reset_line_dash_on_ctx().
//
// The reset-to-solid after every stroke prevents the dash from leaking
// into callers that bypass our overrides (e.g. raw CG callers nested
// inside a save_layer transparency layer, or future overrides we haven't
// wrapped yet). It also keeps semantics aligned with Skia where the
// dash is a paint-local property, not a GState property.
//
// Spec: an empty/odd-length pattern clears the dash and reverts to solid
// strokes (the HTML5 spec says odd-length must be duplicated; the JS
// bridge handles that before the call reaches us, so any odd count here
// is treated as "clear" rather than risking a half-period dash).
void CoreGraphicsCanvas::set_line_dash(const float* intervals, int count,
                                        float phase) {
    if (count <= 0 || (count % 2) != 0 || !intervals) {
        line_dash_.clear();
    } else {
        line_dash_.assign(intervals, intervals + count);
    }
    line_dash_phase_ = phase;
}

void CoreGraphicsCanvas::apply_line_dash_to_ctx() {
    if (line_dash_.empty()) {
        CGContextSetLineDash(ctx_, 0.0, nullptr, 0);
        return;
    }
    // CGContextSetLineDash expects CGFloat (= double on Apple platforms);
    // convert from our float storage. The conversion is cheap (typically
    // 2-8 entries) and avoids an ABI mismatch on macOS arm64 / x86_64.
    std::vector<CGFloat> dash(line_dash_.begin(), line_dash_.end());
    CGContextSetLineDash(ctx_, static_cast<CGFloat>(line_dash_phase_),
                         dash.data(), dash.size());
}

void CoreGraphicsCanvas::reset_line_dash_on_ctx() {
    if (line_dash_.empty()) return;  // already solid — skip the CG call
    CGContextSetLineDash(ctx_, 0.0, nullptr, 0);
}

// pulp #1371 — map every BlendMode value to its CGBlendMode counterpart and
// push it into the current GState. The base Canvas default for set_blend_mode
// is a no-op `(void)mode;`, so without this override every CG-backed CPU paint
// silently lost the requested compositing op. SkiaCanvas::set_blend_mode in
// core/canvas/src/skia_canvas.cpp is the working reference implementation —
// the indices here mirror its `map[]` table 1:1, and the CG enum values cover
// the full HTML5 globalCompositeOperation surface (Apple ships a near-perfect
// match — the only caveat is `lighter`, which is `kCGBlendModePlusLighter`).
//
// The GState stack semantics match Canvas2D save()/restore() — calling
// CGContextSetBlendMode mutates the current GState, and a later GState pop
// restores whatever blend mode was active in the parent frame, exactly as
// the spec requires for ctx.save()/ctx.restore() pairs.
static CGBlendMode to_cg_blend_mode(pulp::canvas::Canvas::BlendMode mode) {
    using BM = pulp::canvas::Canvas::BlendMode;
    switch (mode) {
        // Indices 0..15 — advanced/W3C separable + non-separable blend modes.
        case BM::normal:        return kCGBlendModeNormal;
        case BM::multiply:      return kCGBlendModeMultiply;
        case BM::screen:        return kCGBlendModeScreen;
        case BM::overlay:       return kCGBlendModeOverlay;
        case BM::darken:        return kCGBlendModeDarken;
        case BM::lighten:       return kCGBlendModeLighten;
        case BM::color_dodge:   return kCGBlendModeColorDodge;
        case BM::color_burn:    return kCGBlendModeColorBurn;
        case BM::hard_light:    return kCGBlendModeHardLight;
        case BM::soft_light:    return kCGBlendModeSoftLight;
        case BM::difference:    return kCGBlendModeDifference;
        case BM::exclusion:     return kCGBlendModeExclusion;
        case BM::hue:           return kCGBlendModeHue;
        case BM::saturation:    return kCGBlendModeSaturation;
        case BM::color:         return kCGBlendModeColor;
        case BM::luminosity:    return kCGBlendModeLuminosity;
        // Indices 16..26 — Porter-Duff compositing modes (issue-896).
        case BM::source_over:        return kCGBlendModeNormal;
        case BM::destination_over:   return kCGBlendModeDestinationOver;
        case BM::source_in:          return kCGBlendModeSourceIn;
        case BM::destination_in:     return kCGBlendModeDestinationIn;
        case BM::source_out:         return kCGBlendModeSourceOut;
        case BM::destination_out:    return kCGBlendModeDestinationOut;
        case BM::source_atop:        return kCGBlendModeSourceAtop;
        case BM::destination_atop:   return kCGBlendModeDestinationAtop;
        case BM::xor_mode:           return kCGBlendModeXOR;
        case BM::copy:               return kCGBlendModeCopy;
        case BM::lighter:            return kCGBlendModePlusLighter;
    }
    // Unknown enum value — fall back to the spec default.
    return kCGBlendModeNormal;
}

void CoreGraphicsCanvas::set_blend_mode(BlendMode mode) {
    CGContextSetBlendMode(ctx_, to_cg_blend_mode(mode));
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
    if (has_stroke_gradient_) {
        CGContextBeginPath(ctx_);
        CGContextAddRect(ctx_, CGRectMake(x, y, w, h));
        stroke_with_active_paint();
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokeRect(ctx_, CGRectMake(x, y, w, h));
    reset_line_dash_on_ctx();
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
    // pulp #1359 — gate on has_gradient_ so the active gradient paints the
    // rounded rect, mirroring fill_rect / fill_current_path. Without this,
    // a gradient set via set_fill_gradient_linear/_radial silently dropped
    // back to apply_fill_color() (Spectr's CPU-mode FilterBank backplate).
    if (has_gradient_) {
        CGContextSaveGState(ctx_);
        CGContextClip(ctx_);  // clip to the path we just built
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
        return;
    }
    apply_fill_color();
    CGContextFillPath(ctx_);
}

void CoreGraphicsCanvas::stroke_rounded_rect(float x, float y, float w, float h, float radius) {
    add_rounded_rect_path(x, y, w, h, radius);
    if (has_stroke_gradient_) {
        stroke_with_active_paint();
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokePath(ctx_);
    reset_line_dash_on_ctx();
}

void CoreGraphicsCanvas::fill_circle(float cx, float cy, float radius) {
    const CGRect bounds = CGRectMake(cx - radius, cy - radius, radius * 2, radius * 2);
    // pulp #1359 — when a gradient is active, build an ellipse path and clip
    // to it so fill_with_active_paint() paints the gradient inside the circle.
    // Mirrors fill_rect / fill_rounded_rect / fill_current_path.
    if (has_gradient_) {
        CGContextSaveGState(ctx_);
        CGContextBeginPath(ctx_);
        CGContextAddEllipseInRect(ctx_, bounds);
        CGContextClip(ctx_);
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
        return;
    }
    apply_fill_color();
    CGContextFillEllipseInRect(ctx_, bounds);
}

void CoreGraphicsCanvas::stroke_circle(float cx, float cy, float radius) {
    if (has_stroke_gradient_) {
        CGContextBeginPath(ctx_);
        CGContextAddEllipseInRect(ctx_, CGRectMake(cx - radius, cy - radius, radius * 2, radius * 2));
        stroke_with_active_paint();
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokeEllipseInRect(ctx_, CGRectMake(cx - radius, cy - radius, radius * 2, radius * 2));
    reset_line_dash_on_ctx();
}

void CoreGraphicsCanvas::stroke_arc(float cx, float cy, float radius,
                                   float start_angle, float end_angle) {
    CGContextBeginPath(ctx_);
    // CG uses clockwise=0 for counterclockwise, but our coords are flipped
    CGContextAddArc(ctx_, cx, cy, radius, start_angle, end_angle, 0);
    if (has_stroke_gradient_) {
        stroke_with_active_paint();
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokePath(ctx_);
    reset_line_dash_on_ctx();
}

void CoreGraphicsCanvas::stroke_line(float x0, float y0, float x1, float y1) {
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, x0, y0);
    CGContextAddLineToPoint(ctx_, x1, y1);
    if (has_stroke_gradient_) {
        stroke_with_active_paint();
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokePath(ctx_);
    reset_line_dash_on_ctx();
}

void CoreGraphicsCanvas::stroke_path(const Point2D* points, size_t count) {
    if (count < 2) return;
    CGContextSetShouldAntialias(ctx_, true);
    CGContextSetAllowsAntialiasing(ctx_, true);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i)
        CGContextAddLineToPoint(ctx_, points[i].x, points[i].y);
    if (has_stroke_gradient_) {
        stroke_with_active_paint();
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokePath(ctx_);
    reset_line_dash_on_ctx();
}

void CoreGraphicsCanvas::fill_path(const Point2D* points, size_t count) {
    if (count < 3) return;
    CGContextSetShouldAntialias(ctx_, true);
    CGContextBeginPath(ctx_);
    CGContextMoveToPoint(ctx_, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i)
        CGContextAddLineToPoint(ctx_, points[i].x, points[i].y);
    CGContextClosePath(ctx_);
    // pulp #1359 — honor active gradient on closed point-array fills, the
    // direct CG parallel of SkiaCanvas::fill_path (#1353). Without this,
    // shapes drawn via the Point2D* overload silently dropped the gradient.
    if (has_gradient_) {
        CGContextSaveGState(ctx_);
        CGContextClip(ctx_);  // clip to the path we just built
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
        return;
    }
    apply_fill_color();
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
        if (has_gradient_) {
            // pulp #1643/#1666 followup — gradient text on CG. Use the
            // glyph silhouette as a clip mask, then paint the gradient
            // through fill_with_active_paint(). Mirrors Skia's
            // shader-based glyph fill via CGContextSetTextDrawingMode
            // kCGTextClip + Draw* gradient call.
            CGContextTranslateCTM(ctx_, draw_x, y);
            CGContextScaleCTM(ctx_, 1.0, -1.0);
            CGContextSetTextPosition(ctx_, 0, 0);
            CGContextSetTextDrawingMode(ctx_, kCGTextClip);
            CTLineDraw(line, ctx_);
            // Restore CTM so the gradient paints in canvas-world coords.
            CGContextScaleCTM(ctx_, 1.0, -1.0);
            CGContextTranslateCTM(ctx_, -draw_x, -y);
            // Reset to default fill mode (clip is independent of mode).
            CGContextSetTextDrawingMode(ctx_, kCGTextFill);
            fill_with_active_paint();
        } else {
            apply_fill_color();
            CGContextTranslateCTM(ctx_, draw_x, y);
            CGContextScaleCTM(ctx_, 1.0, -1.0);
            CGContextSetTextPosition(ctx_, 0, 0);
            CTLineDraw(line, ctx_);
        }
        CGContextRestoreGState(ctx_);

        CFRelease(line);
        CFRelease(font);
    }
}

void CoreGraphicsCanvas::fill_text_with_max_width(const std::string& text,
                                                   float x, float y,
                                                   float max_width) {
    // pulp #1525 — Canvas2D `fillText(text, x, y, maxWidth)`. Same
    // horizontal-squeeze approach as SkiaCanvas: measure naturally, and
    // if the advance exceeds `max_width`, scale around the text origin
    // before delegating to the unconstrained `fill_text` path. CoreText's
    // glyph clusters are atomic CTRun units, so horizontal scaling
    // preserves cluster integrity (matches the spec's HarfBuzz contract).
    if (max_width <= 0.0f || text.empty()) {
        fill_text(text, x, y);
        return;
    }
    const float measured = measure_text(text);
    if (measured <= max_width || measured <= 0.0f) {
        fill_text(text, x, y);
        return;
    }
    const float scale = max_width / measured;
    CGContextSaveGState(ctx_);
    CGContextTranslateCTM(ctx_, x, y);
    CGContextScaleCTM(ctx_, scale, 1.0);
    CGContextTranslateCTM(ctx_, -x, -y);
    fill_text(text, x, y);
    CGContextRestoreGState(ctx_);
}

void CoreGraphicsCanvas::stroke_text(const std::string& text, float x, float y,
                                      float max_width) {
    // pulp #1525 — true outlined-glyph rendering via CoreText. We swap
    // the text drawing mode to `kCGTextStroke` so each glyph outline is
    // honoured with the active stroke colour + line width, instead of
    // the pre-#1525 approximation that re-routed through fillText with
    // strokeStyle as the fill colour.
    @autoreleasepool {
        if (text.empty()) return;
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

        CGFloat text_width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
        float draw_x = x;
        switch (text_align_) {
            case TextAlign::center: draw_x = x - text_width * 0.5f; break;
            case TextAlign::right: draw_x = x - text_width; break;
            case TextAlign::left: break;
        }

        CGContextSaveGState(ctx_);

        // pulp #1525 — apply maxWidth squeeze around (x, y).
        if (max_width > 0.0f && text_width > max_width && text_width > 0) {
            const CGFloat scale = max_width / static_cast<CGFloat>(text_width);
            CGContextTranslateCTM(ctx_, x, y);
            CGContextScaleCTM(ctx_, scale, 1.0);
            CGContextTranslateCTM(ctx_, -x, -y);
        }

        if (has_stroke_gradient_) {
            // pulp #1666 — stroke gradient on glyph outlines: build a
            // CGPath of every glyph in the line, route through
            // stroke_with_active_paint() (which clips to the stroked
            // outline + draws the gradient).
            //
            // pulp #1747 (P1 Codex review on #1736) — the per-glyph
            // CGAffineTransform must bake in BOTH the glyph offset AND
            // the text-rendering transform (translate(draw_x, y) +
            // scale(1, -1) to flip from CT's bottom-up glyph space into
            // canvas-down space). Pre-fix the CTM was modified instead,
            // which left the gradient endpoints evaluated in text-local
            // space — same createLinearGradient stops produced different
            // colors as text x-position changed, and vertical gradients
            // rendered upside-down. With the transform baked into the
            // path itself, the CTM stays the canvas-space identity (or
            // whatever the caller had) and gradient endpoints land where
            // the caller meant them to land.
            CGMutablePathRef glyph_path = CGPathCreateMutable();
            CFArrayRef runs = CTLineGetGlyphRuns(line);
            CFIndex run_count = CFArrayGetCount(runs);
            for (CFIndex r = 0; r < run_count; ++r) {
                CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs, r);
                CFIndex glyph_count = CTRunGetGlyphCount(run);
                if (glyph_count == 0) continue;
                CFDictionaryRef attrs = CTRunGetAttributes(run);
                CTFontRef run_font = (CTFontRef)CFDictionaryGetValue(attrs, kCTFontAttributeName);
                if (!run_font) continue;
                std::vector<CGGlyph> glyphs(glyph_count);
                std::vector<CGPoint> positions(glyph_count);
                CTRunGetGlyphs(run, CFRangeMake(0, glyph_count), glyphs.data());
                CTRunGetPositions(run, CFRangeMake(0, glyph_count), positions.data());
                for (CFIndex g = 0; g < glyph_count; ++g) {
                    // Combined affine: glyph point (gx, gy) maps to
                    //   (gx + positions[g].x + draw_x,
                    //    -(gy + positions[g].y) + y)
                    // i.e. shift by glyph position, flip y, translate to
                    // (draw_x, y). CGAffineTransform fields are
                    // (a, b, c, d, tx, ty) with x' = a*x + c*y + tx and
                    // y' = b*x + d*y + ty.
                    CGAffineTransform tx = CGAffineTransformMake(
                        1.0, 0.0,
                        0.0, -1.0,
                        positions[g].x + draw_x,
                        -positions[g].y + y);
                    CGPathRef gp = CTFontCreatePathForGlyph(run_font, glyphs[g], &tx);
                    if (gp) {
                        CGPathAddPath(glyph_path, NULL, gp);
                        CGPathRelease(gp);
                    }
                }
            }
            CGContextAddPath(ctx_, glyph_path);
            CGPathRelease(glyph_path);
            stroke_with_active_paint();
        } else {
            // Stroke mode + active stroke colour. The line width was set by
            // a prior set_line_width() call and is preserved by the GState
            // we just saved — no need to mirror it here.
            CGContextSetRGBStrokeColor(ctx_,
                                       stroke_color_.r, stroke_color_.g,
                                       stroke_color_.b, stroke_color_.a);
            CGContextSetTextDrawingMode(ctx_, kCGTextStroke);

            // pulp #1898 — apply the active dash pattern around the glyph
            // outline stroke. CTLineDraw in kCGTextStroke mode strokes each
            // glyph's silhouette using the current GState dash, so this is
            // the correct injection point. The enclosing CGContextSaveGState
            // (above) snapshots the dash for us, but we still call the
            // reset explicitly to keep the GState's dash field clean in
            // case a future override skips the save/restore wrapper.
            apply_line_dash_to_ctx();
            CGContextTranslateCTM(ctx_, draw_x, y);
            CGContextScaleCTM(ctx_, 1.0, -1.0);
            CGContextSetTextPosition(ctx_, 0, 0);
            CTLineDraw(line, ctx_);
            reset_line_dash_on_ctx();

            // Reset to fill mode for subsequent draws — fill_text doesn't
            // re-set the mode and would otherwise leak our stroke setting
            // into the next text call.
            CGContextSetTextDrawingMode(ctx_, kCGTextFill);
        }
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

void CoreGraphicsCanvas::fill_current_path(FillRule rule) {
    if (!path_) return;
    // pulp Wave 2 cheap wiring — honour the JS-supplied fillRule arg
    // (`ctx.fill('evenodd')`). CG splits nonzero (CGContextFillPath /
    // CGContextClip) and even-odd (CGContextEOFillPath /
    // CGContextEOClip) at the API surface; pre-Wave-2 the bridge
    // unconditionally used the nonzero variant.
    const bool eo = (rule == FillRule::evenodd);
    if (has_gradient_) {
        // Clip to the path, then draw the gradient; restore the clip.
        CGContextSaveGState(ctx_);
        CGContextAddPath(ctx_, path_);
        if (eo) {
            CGContextEOClip(ctx_);
        } else {
            CGContextClip(ctx_);
        }
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
    } else {
        apply_fill_color();
        CGContextAddPath(ctx_, path_);
        if (eo) {
            CGContextEOFillPath(ctx_);
        } else {
            CGContextFillPath(ctx_);
        }
    }
    // pulp #1806 — Canvas2D spec: `ctx.fill()` preserves the scratch path
    // so a subsequent `ctx.stroke()` paints the outlined version of the
    // filled shape. Previously this destroyed `path_` via `release_path()`,
    // which silently dropped strokes after fills (SvgPathWidget compound
    // paths, common JS canvas idiom). `begin_path()` correctly resets the
    // path for callers that intend a fresh build.
}

void CoreGraphicsCanvas::stroke_current_path() {
    if (!path_) return;
    CGContextAddPath(ctx_, path_);
    if (has_stroke_gradient_) {
        stroke_with_active_paint();
        // pulp #1806 — preserve path; see fill_current_path comment.
        return;
    }
    apply_stroke_color();
    apply_line_dash_to_ctx();
    CGContextStrokePath(ctx_);
    reset_line_dash_on_ctx();
    // pulp #1806 — preserve path; see fill_current_path comment.
}

// ── pulp #1521 — native arc / arcTo / ellipse / roundRect path builders ──
//
// Replaces the JS shim's bezier approximation with CG's native arc APIs:
//   - CGPathAddArc          — cx/cy/r/start/end + clockwise flag
//   - CGPathAddArcToPoint   — control points + tangent radius
//   - CGPathAddRelativeArc  — wrapped in a CGAffineTransform for rotation
//   - CGPathAddRoundedRect  — Apple's helper hits the uniform-radius case
//                             only; for per-corner radii we lay out the
//                             8 segments manually.
//
// Y-axis convention: CG's default coordinate system is bottom-up, but the
// canvas widget paints under a flipped CTM (top-down to match Canvas2D).
// CGPathAddArc takes a `clockwise` flag which is the OPPOSITE of the
// HTML5 spec's `anticlockwise` because of the CTM flip — so when the
// caller asks for clockwise (anticlockwise=false), we pass clockwise=1.
void CoreGraphicsCanvas::arc(float cx, float cy, float radius,
                              float start_angle, float end_angle,
                              bool anticlockwise) {
    if (radius <= 0.0f) return;
    if (!path_) path_ = CGPathCreateMutable();
    // CG `clockwise` flag — Y is flipped relative to HTML5, so the
    // sense of "clockwise" inverts: HTML clockwise == CG clockwise=1.
    int cg_clockwise = anticlockwise ? 0 : 1;
    CGPathAddArc(path_, NULL, cx, cy, radius,
                 start_angle, end_angle, cg_clockwise);
}

void CoreGraphicsCanvas::arc_to(float x1, float y1, float x2, float y2,
                                 float radius) {
    if (!path_) path_ = CGPathCreateMutable();
    if (radius <= 0.0f) {
        CGPathAddLineToPoint(path_, NULL, x1, y1);
        return;
    }
    CGPathAddArcToPoint(path_, NULL, x1, y1, x2, y2, radius);
}

void CoreGraphicsCanvas::ellipse(float cx, float cy, float rx, float ry,
                                  float rotation,
                                  float start_angle, float end_angle,
                                  bool anticlockwise) {
    if (rx <= 0.0f || ry <= 0.0f) return;
    if (!path_) path_ = CGPathCreateMutable();
    // Use CGAffineTransform to map a unit circle to (cx,cy) + rx/ry +
    // rotation in one step. The transform is applied on the way IN to
    // the path, so the on-path arc geometry is the rotated ellipse.
    CGAffineTransform t = CGAffineTransformIdentity;
    t = CGAffineTransformTranslate(t, cx, cy);
    if (rotation != 0.0f) {
        t = CGAffineTransformRotate(t, rotation);
    }
    t = CGAffineTransformScale(t, rx, ry);
    int cg_clockwise = anticlockwise ? 0 : 1;
    // Add a unit-circle arc through the transform — CGPath maps it to the
    // rotated ellipse arc.
    CGPathAddArc(path_, &t, 0.0f, 0.0f, 1.0f,
                 start_angle, end_angle, cg_clockwise);
}

void CoreGraphicsCanvas::round_rect(float x, float y, float w, float h,
                                     float tl_x, float tl_y,
                                     float tr_x, float tr_y,
                                     float br_x, float br_y,
                                     float bl_x, float bl_y) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (!path_) path_ = CGPathCreateMutable();
    // Per-corner radii: lay out an 8-segment subpath (4 lines + 4 arcs).
    // The corner sweep direction matches Canvas2D's default clockwise
    // path (HTML clockwise == CG clockwise=1 under the flipped CTM).
    // Clamp each corner to half the dimensions so two adjacent radii
    // never overlap.
    auto clamp_pair = [](float r1, float r2, float dim) {
        float sum = r1 + r2;
        if (sum > dim && sum > 0.0f) {
            float scale = dim / sum;
            r1 *= scale;
            r2 *= scale;
        }
        return std::pair<float, float>{r1, r2};
    };
    auto [tl_x_c, tr_x_c] = clamp_pair(tl_x, tr_x, w); // top edge
    auto [bl_x_c, br_x_c] = clamp_pair(bl_x, br_x, w); // bottom edge
    auto [tl_y_c, bl_y_c] = clamp_pair(tl_y, bl_y, h); // left edge
    auto [tr_y_c, br_y_c] = clamp_pair(tr_y, br_y, h); // right edge

    CGPathMoveToPoint(path_, NULL, x + tl_x_c, y);
    CGPathAddLineToPoint(path_, NULL, x + w - tr_x_c, y);
    CGPathAddArcToPoint(path_, NULL, x + w, y, x + w, y + tr_y_c,
                        std::max(tr_x_c, tr_y_c));
    CGPathAddLineToPoint(path_, NULL, x + w, y + h - br_y_c);
    CGPathAddArcToPoint(path_, NULL, x + w, y + h, x + w - br_x_c, y + h,
                        std::max(br_x_c, br_y_c));
    CGPathAddLineToPoint(path_, NULL, x + bl_x_c, y + h);
    CGPathAddArcToPoint(path_, NULL, x, y + h, x, y + h - bl_y_c,
                        std::max(bl_x_c, bl_y_c));
    CGPathAddLineToPoint(path_, NULL, x, y + tl_y_c);
    CGPathAddArcToPoint(path_, NULL, x, y, x + tl_x_c, y,
                        std::max(tl_x_c, tl_y_c));
    CGPathCloseSubpath(path_);
}

void CoreGraphicsCanvas::stroke_with_active_paint() {
    if (!has_stroke_gradient_ || stroke_grad_colors_.empty()) {
        apply_stroke_color();
        apply_line_dash_to_ctx();
        CGContextStrokePath(ctx_);
        reset_line_dash_on_ctx();
        return;
    }
    // Convert path to its stroked outline + clip to it. Then draw gradient.
    CGContextSaveGState(ctx_);
    // pulp #1898 — push the dash pattern before CGContextReplacePathWithStrokedPath
    // so the stroked outline reflects the dash gaps. The Save/Restore frame
    // we just opened snapshots the dash and pops it on Restore, so this only
    // affects the stroked-path build itself (and the gradient draw inside the
    // clip is rect-fill semantics that ignore dash anyway).
    apply_line_dash_to_ctx();
    CGContextReplacePathWithStrokedPath(ctx_);
    CGContextClip(ctx_);

    if (stroke_gradient_kind_ == GradientKind::linear ||
        stroke_gradient_kind_ == GradientKind::radial ||
        stroke_gradient_kind_ == GradientKind::radial_two_circles) {
        // Build CGColorSpace + CGGradientRef from stops.
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        std::vector<CGFloat> components;
        components.reserve(stroke_grad_colors_.size() * 4);
        for (const auto& c : stroke_grad_colors_) {
            components.push_back(c.r);
            components.push_back(c.g);
            components.push_back(c.b);
            components.push_back(c.a);
        }
        std::vector<CGFloat> locations(stroke_grad_positions_.begin(),
                                       stroke_grad_positions_.end());
        CGGradientRef grad = CGGradientCreateWithColorComponents(
            cs, components.data(), locations.data(),
            static_cast<size_t>(stroke_grad_colors_.size()));
        if (grad) {
            CGGradientDrawingOptions opts =
                kCGGradientDrawsBeforeStartLocation |
                kCGGradientDrawsAfterEndLocation;
            if (stroke_gradient_kind_ == GradientKind::linear) {
                CGContextDrawLinearGradient(ctx_, grad,
                    CGPointMake(stroke_grad_x0_, stroke_grad_y0_),
                    CGPointMake(stroke_grad_x1_, stroke_grad_y1_),
                    opts);
            } else {
                // radial / two_circles
                CGContextDrawRadialGradient(ctx_, grad,
                    CGPointMake(stroke_grad_x0_, stroke_grad_y0_), stroke_grad_radius_inner_,
                    CGPointMake(stroke_grad_x1_, stroke_grad_y1_), stroke_grad_radius_,
                    opts);
            }
            CGGradientRelease(grad);
        }
        CGColorSpaceRelease(cs);
    } else if (stroke_gradient_kind_ == GradientKind::conic_image) {
        // Conic stroke isn't supported on CG today (would require
        // rasterising the conic image then clipping to the stroke
        // silhouette — hot path). Fall back to first-stop solid.
        if (!stroke_grad_colors_.empty()) {
            const auto& first = stroke_grad_colors_.front();
            CGContextSetRGBFillColor(ctx_, first.r, first.g, first.b, first.a);
            CGContextFillRect(ctx_, CGContextGetClipBoundingBox(ctx_));
        }
    }
    CGContextRestoreGState(ctx_);
}

// pulp #1524 — Canvas2D ctx.createPattern on the CG backend.
//
// We decode the source image via ImageIO (CGImageSource) and stash the
// CGImageRef. fill_with_active_paint() builds a CGPattern via
// CGPatternCreate at fill time, with a draw callback that emits one tile
// via CGContextDrawImage. CGPattern's tile-step controls the X/Y
// repetition: a `no_repeat` axis collapses the step in that axis to a
// large value so only the seed tile renders within the active clip.
//
// The Skia backend mirrors this via SkShader::MakeImage with SkTileMode
// per axis — same visual result for `repeat`, `repeat-x`, `repeat-y`,
// `no-repeat`.
namespace {

CGImageRef cg_decode_image_from_path_or_data(const std::string& src) {
    if (src.empty()) return nullptr;
    @autoreleasepool {
        // Tolerate both filesystem paths and `data:` URLs. ImageIO
        // accepts CFData (raw bytes) for both — for paths we read the
        // file off disk first, for `data:` URLs we let ImageIO handle
        // the base64 decode transparently via CGImageSourceCreateWithURL.
        if (src.rfind("data:", 0) == 0) {
            NSString* ns = [NSString stringWithUTF8String:src.c_str()];
            NSURL* url = [NSURL URLWithString:ns];
            if (!url) return nullptr;
            CGImageSourceRef source = CGImageSourceCreateWithURL(
                (__bridge CFURLRef)url, nullptr);
            if (!source) return nullptr;
            CGImageRef img = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
            CFRelease(source);
            return img;
        }
        NSString* ns_path = [NSString stringWithUTF8String:src.c_str()];
        NSData* data = [NSData dataWithContentsOfFile:ns_path];
        if (!data) return nullptr;
        CGImageSourceRef source = CGImageSourceCreateWithData(
            (__bridge CFDataRef)data, nullptr);
        if (!source) return nullptr;
        CGImageRef img = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        CFRelease(source);
        return img;
    }
}

} // namespace

void CoreGraphicsCanvas::set_fill_pattern(const std::string& image_src,
                                           PatternTileMode tile_x,
                                           PatternTileMode tile_y) {
    // Empty src → clear pattern (mirrors clear_fill_gradient reset shape).
    if (image_src.empty()) {
        clear_fill_gradient();
        return;
    }
    CGImageRef img = cg_decode_image_from_path_or_data(image_src);
    if (!img) {
        // Decode failed — fall back to the existing solid fill rather than
        // render garbage. Mirrors SkiaCanvas::set_fill_pattern's clear path.
        clear_fill_gradient();
        return;
    }
    // Drop any prior gradient/pattern; install the new pattern image.
    clear_fill_gradient();
    pattern_image_ = img;
    pattern_tile_x_ = tile_x;
    pattern_tile_y_ = tile_y;
    has_pattern_ = true;
    has_gradient_ = true;  // routes fill_with_active_paint into the pattern branch
    gradient_kind_ = GradientKind::none;  // pattern branch checks has_pattern_ first
}

void CoreGraphicsCanvas::set_stroke_pattern(const std::string& image_src,
                                             PatternTileMode tile_x,
                                             PatternTileMode tile_y) {
    (void)image_src; (void)tile_x; (void)tile_y;
    // Stroke patterns aren't wired through stroke_with_active_paint yet —
    // strokes continue with the existing stroke_color_. File a follow-up
    // if a CG-targeted plugin needs tiled stroke patterns.
}

// pulp #1434 bridge-thin gap-fill — Canvas2D ctx.miterLimit.
// CGContextSetMiterLimit attaches the value to the current GState, so
// save()/restore() snapshots and pops it naturally. Spec: non-positive
// or non-finite values are silently ignored (matches CG behaviour for
// the "set" itself — CG would clamp to its internal minimum, but we
// short-circuit to keep the recording-canvas test surface deterministic).
void CoreGraphicsCanvas::set_miter_limit(float limit) {
    if (!std::isfinite(limit) || limit <= 0.0f) return;
    if (ctx_) {
        CGContextSetMiterLimit(ctx_, limit);
    }
}

// pulp #1434 bridge-thin gap-fill — Canvas2D
// imageSmoothingEnabled / imageSmoothingQuality. CG exposes the same
// concept via CGContextSetInterpolationQuality on the current GState.
// We translate the spec's three quality levels onto CG's enum:
//   low    = kCGInterpolationLow     (cheap bilinear)
//   medium = kCGInterpolationMedium
//   high   = kCGInterpolationHigh    (Lanczos / cubic)
// `enabled = false` collapses to kCGInterpolationNone (nearest), which
// is what HTML5 canvas spec requires (pixel-art preservation).
void CoreGraphicsCanvas::set_image_smoothing(bool enabled,
                                              ImageSmoothingQuality quality) {
    if (!ctx_) return;
    CGInterpolationQuality cg_q = kCGInterpolationDefault;
    if (!enabled) {
        cg_q = kCGInterpolationNone;
    } else {
        switch (quality) {
            case ImageSmoothingQuality::low:    cg_q = kCGInterpolationLow;    break;
            case ImageSmoothingQuality::medium: cg_q = kCGInterpolationMedium; break;
            case ImageSmoothingQuality::high:   cg_q = kCGInterpolationHigh;   break;
        }
    }
    CGContextSetInterpolationQuality(ctx_, cg_q);
}

// pulp #1524 — sample the active conic colour stops at angle `t` in [0, 1],
// where t=0 corresponds to start_angle and t=1 wraps back to start_angle + 2π.
// Returns four CGFloat RGBA components in straight (un-premultiplied) space.
static void sample_conic_stops(const std::vector<pulp::canvas::Color>& colors,
                                const std::vector<float>& positions,
                                double t, CGFloat out_rgba[4]) {
    const size_t n = colors.size();
    if (n == 0) {
        out_rgba[0] = out_rgba[1] = out_rgba[2] = out_rgba[3] = 0;
        return;
    }
    // Wrap t into [0, 1) for spec-correct angular sweep semantics.
    t = t - std::floor(t);
    if (n == 1 || t <= positions[0]) {
        out_rgba[0] = colors[0].r;
        out_rgba[1] = colors[0].g;
        out_rgba[2] = colors[0].b;
        out_rgba[3] = colors[0].a;
        return;
    }
    if (t >= positions[n - 1]) {
        out_rgba[0] = colors[n - 1].r;
        out_rgba[1] = colors[n - 1].g;
        out_rgba[2] = colors[n - 1].b;
        out_rgba[3] = colors[n - 1].a;
        return;
    }
    // Find the interval [positions[i], positions[i+1]] enclosing t and
    // linearly interpolate the colour.
    for (size_t i = 0; i + 1 < n; ++i) {
        if (t >= positions[i] && t <= positions[i + 1]) {
            const double span = std::max<double>(positions[i + 1] - positions[i], 1e-9);
            const double k = (t - positions[i]) / span;
            const double inv = 1.0 - k;
            out_rgba[0] = inv * colors[i].r + k * colors[i + 1].r;
            out_rgba[1] = inv * colors[i].g + k * colors[i + 1].g;
            out_rgba[2] = inv * colors[i].b + k * colors[i + 1].b;
            out_rgba[3] = inv * colors[i].a + k * colors[i + 1].a;
            return;
        }
    }
    // Defensive: last-stop colour.
    out_rgba[0] = colors[n - 1].r;
    out_rgba[1] = colors[n - 1].g;
    out_rgba[2] = colors[n - 1].b;
    out_rgba[3] = colors[n - 1].a;
}

// pulp #1524 — software-rasterise a conic (sweep) gradient image covering the
// supplied bounding rectangle. Each pixel's angle is computed as
// atan2(y - cy, x - cx) - start_angle (mod 2π) and divided by 2π to give the
// stop position. Returns nullptr on allocation failure.
static CGImageRef build_conic_gradient_image(
        float cx, float cy, float start_angle,
        const std::vector<pulp::canvas::Color>& colors,
        const std::vector<float>& positions,
        CGRect bounds) {
    const int width = std::max(1, static_cast<int>(std::ceil(bounds.size.width)));
    const int height = std::max(1, static_cast<int>(std::ceil(bounds.size.height)));
    const size_t bytes_per_row = static_cast<size_t>(width) * 4u;
    const size_t total = bytes_per_row * static_cast<size_t>(height);
    std::vector<uint8_t> pixels(total, 0);
    constexpr double kTwoPi = 6.283185307179586;
    for (int py = 0; py < height; ++py) {
        // Pixel centre in canvas space — bounds.origin is top-left of the
        // image in canvas coords, so offset by (px+0.5, py+0.5).
        const double sample_y = bounds.origin.y + py + 0.5;
        for (int px = 0; px < width; ++px) {
            const double sample_x = bounds.origin.x + px + 0.5;
            const double dx = sample_x - cx;
            const double dy = sample_y - cy;
            // atan2 returns (-π, π]; subtract start_angle, then wrap to [0, 2π).
            double theta = std::atan2(dy, dx) - static_cast<double>(start_angle);
            theta = theta - kTwoPi * std::floor(theta / kTwoPi);
            const double t = theta / kTwoPi;
            CGFloat rgba[4];
            sample_conic_stops(colors, positions, t, rgba);
            // Premultiplied RGBA8 (matches kCGImageAlphaPremultipliedLast).
            const double a = std::clamp<double>(rgba[3], 0.0, 1.0);
            const uint8_t r = static_cast<uint8_t>(
                std::clamp<double>(rgba[0] * a, 0.0, 1.0) * 255.0 + 0.5);
            const uint8_t g = static_cast<uint8_t>(
                std::clamp<double>(rgba[1] * a, 0.0, 1.0) * 255.0 + 0.5);
            const uint8_t b = static_cast<uint8_t>(
                std::clamp<double>(rgba[2] * a, 0.0, 1.0) * 255.0 + 0.5);
            const uint8_t a8 = static_cast<uint8_t>(a * 255.0 + 0.5);
            const size_t off = static_cast<size_t>(py) * bytes_per_row +
                static_cast<size_t>(px) * 4u;
            pixels[off + 0] = r;
            pixels[off + 1] = g;
            pixels[off + 2] = b;
            pixels[off + 3] = a8;
        }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) return nullptr;
    CGContextRef bmp = CGBitmapContextCreate(
        pixels.data(), static_cast<size_t>(width), static_cast<size_t>(height),
        8, bytes_per_row, cs,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<uint32_t>(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(cs);
    if (!bmp) return nullptr;
    CGImageRef img = CGBitmapContextCreateImage(bmp);
    CGContextRelease(bmp);
    return img;
}

// pulp #1524 — CGPattern draw callback. The `info` field is the CGImageRef
// of the tile bitmap; CG will invoke this once per tile and we paint the
// image filling the tile rect. CG owns the pattern's draw lifetime.
static void cg_canvas_pattern_draw_tile(void* info, CGContextRef ctx) {
    CGImageRef img = static_cast<CGImageRef>(info);
    if (!img || !ctx) return;
    const CGFloat w = static_cast<CGFloat>(CGImageGetWidth(img));
    const CGFloat h = static_cast<CGFloat>(CGImageGetHeight(img));
    if (w <= 0 || h <= 0) return;
    // CGPattern tile space starts at (0, 0); CGPattern handles flipping.
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
}

static const CGPatternCallbacks kPulpCgPatternCallbacks = {
    /*version*/ 0,
    /*drawPattern*/ &cg_canvas_pattern_draw_tile,
    /*releaseInfo*/ nullptr  // image lifetime owned by CoreGraphicsCanvas
};

void CoreGraphicsCanvas::fill_with_active_paint() {
    // 1) Pattern — install a CGPattern (image tile + step mode) and fill the
    //    current clip with it. has_pattern_ takes precedence over the
    //    gradient flags so set_fill_pattern reliably wins after a
    //    set_fill_gradient_* call.
    if (has_pattern_ && pattern_image_) {
        const CGFloat tile_w = static_cast<CGFloat>(CGImageGetWidth(pattern_image_));
        const CGFloat tile_h = static_cast<CGFloat>(CGImageGetHeight(pattern_image_));
        if (tile_w <= 0 || tile_h <= 0) return;
        // For `no_repeat` axes, blow up the step to a value larger than the
        // clip bounding box so only the seed tile lands inside the clip.
        const CGRect clip_bb = CGContextGetClipBoundingBox(ctx_);
        const CGFloat huge = std::max<CGFloat>(clip_bb.size.width, clip_bb.size.height) +
                              tile_w + tile_h + 1.0f;
        const CGFloat step_x = (pattern_tile_x_ == PatternTileMode::repeat) ? tile_w : huge;
        const CGFloat step_y = (pattern_tile_y_ == PatternTileMode::repeat) ? tile_h : huge;
        const CGRect tile_bounds = CGRectMake(0, 0, tile_w, tile_h);
        // Compensate for the canvas's flipped CTM (we did
        // ScaleCTM(1, -1) at construction). Without the flip here the tile
        // image draws upside-down because CG sees tile space top-down.
        CGAffineTransform tile_xform = CGAffineTransformMakeScale(1.0f, -1.0f);
        // Translate so the seed tile lands at the clip origin.
        tile_xform = CGAffineTransformTranslate(
            tile_xform, clip_bb.origin.x, -(clip_bb.origin.y + tile_h));
        CGPatternRef pattern = CGPatternCreate(
            /*info*/ pattern_image_,
            /*bounds*/ tile_bounds,
            /*matrix*/ tile_xform,
            /*xStep*/ step_x,
            /*yStep*/ step_y,
            /*tiling*/ kCGPatternTilingNoDistortion,
            /*isColored*/ true,
            /*callbacks*/ &kPulpCgPatternCallbacks);
        if (!pattern) return;
        CGColorSpaceRef pcs = CGColorSpaceCreatePattern(nullptr);
        if (!pcs) { CGPatternRelease(pattern); return; }
        CGContextSetFillColorSpace(ctx_, pcs);
        const CGFloat alpha = 1.0f;
        CGContextSetFillPattern(ctx_, pattern, &alpha);
        CGContextFillRect(ctx_, clip_bb);
        CGColorSpaceRelease(pcs);
        CGPatternRelease(pattern);
        return;
    }

    if (!has_gradient_ || grad_colors_.empty()) {
        apply_fill_color();
        return;
    }

    // 2) Conic gradient — software-rasterise once per fill into a CGImage
    //    sized to the active clip's bounding box, then paint via
    //    CGContextDrawImage. Caches across repeated fills with the same
    //    clip; rebuilds when clip changes (cheaper than every-pixel walk
    //    inside a CG callback). Spec-correct two+ stop angular sweep.
    if (gradient_kind_ == GradientKind::conic_image) {
        const CGRect clip_bb = CGContextGetClipBoundingBox(ctx_);
        if (clip_bb.size.width <= 0 || clip_bb.size.height <= 0) return;
        // Lazy rebuild: drop the cached image if its size or origin no longer
        // matches the current clip. The cheap path keeps repeated fills (e.g.
        // animation frames over the same geometry) from re-rasterising.
        const bool needs_rebuild = !conic_image_ ||
            std::abs(conic_image_x_ - static_cast<float>(clip_bb.origin.x)) > 0.5f ||
            std::abs(conic_image_y_ - static_cast<float>(clip_bb.origin.y)) > 0.5f ||
            std::abs(conic_image_w_ - static_cast<float>(clip_bb.size.width)) > 0.5f ||
            std::abs(conic_image_h_ - static_cast<float>(clip_bb.size.height)) > 0.5f;
        if (needs_rebuild) {
            release_conic_image();
            conic_image_ = build_conic_gradient_image(
                grad_x0_, grad_y0_, grad_x1_, grad_colors_, grad_positions_,
                clip_bb);
            conic_image_x_ = static_cast<float>(clip_bb.origin.x);
            conic_image_y_ = static_cast<float>(clip_bb.origin.y);
            conic_image_w_ = static_cast<float>(clip_bb.size.width);
            conic_image_h_ = static_cast<float>(clip_bb.size.height);
        }
        if (!conic_image_) return;
        // CTM is already flipped (scale 1, -1) at construction; counter-flip
        // around the image rect so the bitmap paints right-side-up.
        CGContextSaveGState(ctx_);
        CGContextTranslateCTM(ctx_, clip_bb.origin.x, clip_bb.origin.y + clip_bb.size.height);
        CGContextScaleCTM(ctx_, 1.0f, -1.0f);
        CGContextDrawImage(ctx_,
            CGRectMake(0, 0, clip_bb.size.width, clip_bb.size.height),
            conic_image_);
        CGContextRestoreGState(ctx_);
        return;
    }

    // 3) Linear / single-circle radial / two-circle radial gradients via
    //    CGGradient + CGContextDrawLinearGradient / CGContextDrawRadialGradient.
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
    if (gradient_kind_ == GradientKind::radial_two_circles) {
        // pulp #1524 — full two-circle form. (x0,y0,r0) is the start /
        // inner circle; (x1,y1,r1) is the end / outer circle.
        CGContextDrawRadialGradient(ctx_,
            gradient,
            CGPointMake(grad_x0_, grad_y0_), grad_radius_inner_,
            CGPointMake(grad_x1_, grad_y1_), grad_radius_,
            opts);
    } else if (gradient_kind_ == GradientKind::radial || gradient_is_radial_) {
        // Single-circle form — inner circle collapses to centre, radius 0.
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

// ── Canvas2D drop-shadow state (issue-1434 batch 7) ─────────────────────────
//
// CG owns the sticky shadow state via its GState stack — `CGContextSetShadowWithColor`
// snapshots into the current GState, and a later `CGContextRestoreGState`
// restores whatever shadow was active in the parent frame. This matches
// Canvas2D save()/restore() semantics for `ctx.shadow*` exactly. We also
// hold the values in member fields so a setter mutation can rebuild the
// combined CG call (the API takes color + offset + blur in a single call,
// so each individual setter has to forward all four).
//
// Gating: the shadow renders only when (color.a > 0) AND (blur > 0 OR
// offset_x != 0 OR offset_y != 0). When inactive we explicitly clear the
// CG state with `CGContextSetShadowWithColor(ctx, CGSizeZero, 0, NULL)`,
// which is what Apple recommends for "turn off shadow".

void CoreGraphicsCanvas::apply_shadow_to_context() {
    const bool active = shadow_color_.a > 0.0f &&
        (shadow_blur_ > 0.0f || shadow_offset_x_ != 0.0f ||
         shadow_offset_y_ != 0.0f);
    if (!active) {
        CGContextSetShadowWithColor(ctx_, CGSizeZero, 0.0f, nullptr);
        return;
    }
    CGFloat components[4] = {shadow_color_.r, shadow_color_.g,
                             shadow_color_.b, shadow_color_.a};
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGColorRef color = CGColorCreate(cs, components);
    CGContextSetShadowWithColor(
        ctx_, CGSizeMake(shadow_offset_x_, shadow_offset_y_),
        shadow_blur_, color);
    CGColorRelease(color);
    CGColorSpaceRelease(cs);
}

void CoreGraphicsCanvas::set_shadow_color(Color color) {
    shadow_color_ = color;
    apply_shadow_to_context();
}
void CoreGraphicsCanvas::set_shadow_blur(float blur) {
    shadow_blur_ = (blur > 0.0f) ? blur : 0.0f;
    apply_shadow_to_context();
}
void CoreGraphicsCanvas::set_shadow_offset_x(float dx) {
    shadow_offset_x_ = dx;
    apply_shadow_to_context();
}
void CoreGraphicsCanvas::set_shadow_offset_y(float dy) {
    shadow_offset_y_ = dy;
    apply_shadow_to_context();
}

} // namespace pulp::canvas

#endif // __APPLE__
