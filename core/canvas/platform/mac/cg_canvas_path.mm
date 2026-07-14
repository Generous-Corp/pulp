// cg_canvas_path.mm — retained Path draws and handle-based compositing layers
// for the CoreGraphics backend.
//
// Split out of cg_canvas.mm alongside cg_canvas_gradients.mm and
// cg_canvas_shadow.mm.
//
// COORDINATE NOTE. The CoreGraphicsCanvas constructor flips the CTM
// (translate(0, h); scale(1, -1)) so callers get Pulp's y-down space on top of
// CG's y-up space. Geometry needs no special handling, but IMAGES do: CG draws
// a CGImage bottom-up, so a layer blitted through the flipped CTM would appear
// upside down. Every image draw below counter-flips around its own destination
// rect — the same idiom fill_with_active_paint() uses for the conic gradient.

#ifdef __APPLE__

#include <pulp/canvas/cg_canvas.hpp>
#include <pulp/canvas/path.hpp>

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::canvas {

namespace {

/// Largest offscreen layer we will allocate, per axis — a broken layout pass
/// must not be able to ask for a multi-gigabyte bitmap.
constexpr int kMaxLayerDimension = 8192;

CGLineCap to_cg_cap(LineCap cap) {
    switch (cap) {
        case LineCap::round:  return kCGLineCapRound;
        case LineCap::square: return kCGLineCapSquare;
        case LineCap::butt:   break;
    }
    return kCGLineCapButt;
}

CGLineJoin to_cg_join(LineJoin join) {
    switch (join) {
        case LineJoin::round: return kCGLineJoinRound;
        case LineJoin::bevel: return kCGLineJoinBevel;
        case LineJoin::miter: break;
    }
    return kCGLineJoinMiter;
}

}  // namespace

CGMutablePathRef CoreGraphicsCanvas::make_cg_path(const Path& path) const {
    CGMutablePathRef p = CGPathCreateMutable();
    for (Path::Element el : path) {
        switch (el.verb) {
            case Path::Verb::move:
                CGPathMoveToPoint(p, nullptr, el.points[0].x, el.points[0].y);
                break;
            case Path::Verb::line:
                CGPathAddLineToPoint(p, nullptr, el.points[0].x, el.points[0].y);
                break;
            case Path::Verb::quad:
                CGPathAddQuadCurveToPoint(p, nullptr,
                                          el.points[0].x, el.points[0].y,
                                          el.points[1].x, el.points[1].y);
                break;
            case Path::Verb::cubic:
                CGPathAddCurveToPoint(p, nullptr,
                                      el.points[0].x, el.points[0].y,
                                      el.points[1].x, el.points[1].y,
                                      el.points[2].x, el.points[2].y);
                break;
            case Path::Verb::close:
                CGPathCloseSubpath(p);
                break;
        }
    }
    return p;
}

// ── Retained path draws ──────────────────────────────────────────────────

void CoreGraphicsCanvas::fill_path(const Path& path, FillRule rule) {
    if (!ctx_ || path.is_empty()) return;
    const bool eo = (rule == FillRule::evenodd);

    CGMutablePathRef p = make_cg_path(path);
    CGContextSetShouldAntialias(ctx_, true);
    CGContextBeginPath(ctx_);
    CGContextAddPath(ctx_, p);
    CGPathRelease(p);

    if (has_gradient_) {
        // CG has no "fill with shader" — clip to the path, then paint the
        // gradient across the clip. Same approach as the polygon overload.
        CGContextSaveGState(ctx_);
        if (eo) CGContextEOClip(ctx_);
        else    CGContextClip(ctx_);
        fill_with_active_paint();
        CGContextRestoreGState(ctx_);
        return;
    }

    apply_fill_color();
    if (eo) CGContextEOFillPath(ctx_);
    else    CGContextFillPath(ctx_);
}

void CoreGraphicsCanvas::stroke_path(const Path& path, const StrokeStyle& style) {
    if (!ctx_ || path.is_empty()) return;

    CGMutablePathRef p = make_cg_path(path);
    CGContextSetShouldAntialias(ctx_, true);
    CGContextSetAllowsAntialiasing(ctx_, true);

    // Scope the style to this one draw. The immediate-mode setters leave their
    // state on the context; a save/restore pair means stroke_path(path, style)
    // cannot leak its width/cap/join/dash into the next unrelated stroke.
    CGContextSaveGState(ctx_);

    CGContextSetLineWidth(ctx_, style.width);
    CGContextSetLineCap(ctx_, to_cg_cap(style.cap));
    CGContextSetLineJoin(ctx_, to_cg_join(style.join));
    CGContextSetMiterLimit(ctx_, style.miter_limit);

    if (!style.dash.empty()) {
        // SVG / Canvas2D: an odd-length pattern repeats to become even.
        std::vector<CGFloat> intervals(style.dash.begin(), style.dash.end());
        if (intervals.size() % 2 == 1)
            intervals.insert(intervals.end(), style.dash.begin(), style.dash.end());
        const bool any_positive =
            std::any_of(intervals.begin(), intervals.end(),
                        [](CGFloat v) { return v > 0.0; });
        if (any_positive) {
            CGContextSetLineDash(ctx_, style.dash_phase,
                                 intervals.data(), intervals.size());
        }
    } else {
        CGContextSetLineDash(ctx_, 0.0, nullptr, 0);
    }

    CGContextBeginPath(ctx_);
    CGContextAddPath(ctx_, p);
    CGPathRelease(p);

    if (has_stroke_gradient_) {
        stroke_with_active_paint();
    } else {
        apply_stroke_color();
        CGContextStrokePath(ctx_);
    }

    CGContextRestoreGState(ctx_);
}

void CoreGraphicsCanvas::clip_path(const Path& path, FillRule rule) {
    if (!ctx_ || path.is_empty()) return;
    CGMutablePathRef p = make_cg_path(path);
    CGContextBeginPath(ctx_);
    CGContextAddPath(ctx_, p);
    CGPathRelease(p);
    if (rule == FillRule::evenodd) CGContextEOClip(ctx_);
    else                           CGContextClip(ctx_);
}

void CoreGraphicsCanvas::draw_path_shadow(const Path& path, float dx, float dy,
                                          float blur, float spread, Color color) {
    if (!ctx_ || path.is_empty()) return;

    // Grow the silhouette by `spread` before blurring (CSS box-shadow model).
    // Exact for convex paths, approximate for concave ones.
    Path shadow = path;
    if (spread != 0.0f) {
        const Rect2D b = path.bounds();
        if (b.width > 0.0f && b.height > 0.0f) {
            const float sx = (b.width + 2.0f * spread) / b.width;
            const float sy = (b.height + 2.0f * spread) / b.height;
            shadow.apply_transform(AffineTransform::scaling(
                sx, sy, b.x + b.width * 0.5f, b.y + b.height * 0.5f));
        }
    }

    CGContextSaveGState(ctx_);

    // The trick that makes this shadow-ONLY: set a CG shadow, then paint the
    // path in FULLY TRANSPARENT ink. CG still casts the shadow (the shadow is
    // computed from the geometry's coverage, not its colour), but the path
    // itself contributes nothing. A transparency layer keeps the shadow from
    // being cast by each subpath independently.
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    const CGFloat comps[4] = {color.r, color.g, color.b, color.a};
    CGColorRef shadow_color = CGColorCreate(cs, comps);

    // CG's shadow offset is in the *base* (unflipped, y-up) space, so the
    // constructor's y-flip means a positive Pulp dy (downward) needs a
    // negative CG dy. Getting this wrong puts every shadow on the wrong side.
    CGContextSetShadowWithColor(ctx_, CGSizeMake(dx, -dy),
                                std::max(0.0f, blur), shadow_color);
    CGColorRelease(shadow_color);
    CGColorSpaceRelease(cs);

    CGMutablePathRef p = make_cg_path(shadow);
    CGContextSetShouldAntialias(ctx_, true);
    CGContextBeginPath(ctx_);
    CGContextAddPath(ctx_, p);
    CGPathRelease(p);
    CGContextSetRGBFillColor(ctx_, 0.0, 0.0, 0.0, 0.0);  // invisible source
    CGContextFillPath(ctx_);

    CGContextRestoreGState(ctx_);
}

// ── Retained compositing layers ──────────────────────────────────────────

CoreGraphicsCanvas::CgLayer* CoreGraphicsCanvas::find_layer(uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& [lid, rec] : layers_)
        if (lid == id) return &rec;
    return nullptr;
}

const CoreGraphicsCanvas::CgLayer* CoreGraphicsCanvas::find_layer(uint64_t id) const {
    if (id == 0) return nullptr;
    for (const auto& [lid, rec] : layers_)
        if (lid == id) return &rec;
    return nullptr;
}

void CoreGraphicsCanvas::release_layer(CgLayer& layer) {
    if (layer.image != nullptr) {
        CGImageRelease(layer.image);
        layer.image = nullptr;
    }
    if (layer.ctx != nullptr) {
        CGContextRelease(layer.ctx);
        layer.ctx = nullptr;
    }
}

Canvas::LayerHandle CoreGraphicsCanvas::begin_layer(Rect2D bounds, bool cacheable) {
    if (!ctx_ || bounds.width <= 0.0f || bounds.height <= 0.0f)
        return LayerHandle{};

    // Allocate at device resolution so a layer recorded under a 2x CTM is not
    // blown up from a 1x bitmap.
    const CGAffineTransform m = CGContextGetCTM(ctx_);
    float sx = static_cast<float>(std::hypot(m.a, m.b));
    float sy = static_cast<float>(std::hypot(m.c, m.d));
    if (!(sx > 0.0f) || !std::isfinite(sx)) sx = 1.0f;
    if (!(sy > 0.0f) || !std::isfinite(sy)) sy = 1.0f;

    const int pw = std::clamp(
        static_cast<int>(std::ceil(bounds.width * sx)), 1, kMaxLayerDimension);
    const int ph = std::clamp(
        static_cast<int>(std::ceil(bounds.height * sy)), 1, kMaxLayerDimension);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef layer_ctx = CGBitmapContextCreate(
        nullptr, static_cast<size_t>(pw), static_cast<size_t>(ph),
        8, 0, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (layer_ctx == nullptr) return LayerHandle{};  // OOM — caller draws direct

    // Give the layer context the SAME y-down space the caller is drawing in:
    // first the constructor-style flip, then the device scale, then the
    // translate that puts bounds' origin at the layer's top-left.
    CGContextTranslateCTM(layer_ctx, 0, ph);
    CGContextScaleCTM(layer_ctx, 1.0, -1.0);
    CGContextScaleCTM(layer_ctx, sx, sy);
    CGContextTranslateCTM(layer_ctx, -bounds.x, -bounds.y);

    const uint64_t id = next_layer_id_++;
    CgLayer rec;
    rec.ctx = layer_ctx;
    rec.bounds = bounds;
    rec.scale_x = sx;
    rec.scale_y = sy;
    rec.cacheable = cacheable;
    layers_.emplace_back(id, rec);

    open_layers_.push_back(CgOpenLayer{id, ctx_});
    ctx_ = layer_ctx;  // redirect every subsequent draw into the layer
    return LayerHandle{id};
}

Canvas::LayerHandle CoreGraphicsCanvas::end_layer() {
    if (open_layers_.empty()) return LayerHandle{};  // unbalanced — ignore

    const CgOpenLayer open = open_layers_.back();
    open_layers_.pop_back();
    ctx_ = open.previous_ctx;  // stop drawing into the layer

    CgLayer* rec = find_layer(open.id);
    if (rec == nullptr) return LayerHandle{};

    if (rec->ctx != nullptr) {
        // Seal it. This CGImageRef is what OUTLIVES the frame.
        rec->image = CGBitmapContextCreateImage(rec->ctx);
        CGContextRelease(rec->ctx);
        rec->ctx = nullptr;
    }
    if (rec->image == nullptr) {
        invalidate_layer(LayerHandle{open.id});
        return LayerHandle{};
    }
    return LayerHandle{open.id};
}

void CoreGraphicsCanvas::draw_layer(LayerHandle layer, float alpha, BlendMode mode) {
    const CgLayer* rec = find_layer(layer.id);
    if (!ctx_ || rec == nullptr || rec->image == nullptr) return;

    CGContextSaveGState(ctx_);
    CGContextSetAlpha(ctx_, std::clamp(alpha, 0.0f, 1.0f));
    // Reuse the existing BlendMode -> CGBlendMode mapping rather than
    // duplicating the table; the enclosing save/restore scopes it to this draw.
    set_blend_mode(mode);
    // Counter-flip around the destination rect: CG draws images bottom-up and
    // our CTM is already y-down.
    CGContextTranslateCTM(ctx_, rec->bounds.x, rec->bounds.y + rec->bounds.height);
    CGContextScaleCTM(ctx_, 1.0, -1.0);
    CGContextDrawImage(ctx_,
                       CGRectMake(0, 0, rec->bounds.width, rec->bounds.height),
                       rec->image);
    CGContextRestoreGState(ctx_);

    if (!rec->cacheable) invalidate_layer(layer);
}

void CoreGraphicsCanvas::draw_layer_fitted(LayerHandle layer, Rect2D dest) {
    const CgLayer* rec = find_layer(layer.id);
    if (!ctx_ || rec == nullptr || rec->image == nullptr) return;
    if (dest.width <= 0.0f || dest.height <= 0.0f) return;

    CGContextSaveGState(ctx_);
    CGContextTranslateCTM(ctx_, dest.x, dest.y + dest.height);
    CGContextScaleCTM(ctx_, 1.0, -1.0);
    CGContextDrawImage(ctx_, CGRectMake(0, 0, dest.width, dest.height), rec->image);
    CGContextRestoreGState(ctx_);

    if (!rec->cacheable) invalidate_layer(layer);
}

void CoreGraphicsCanvas::draw_layer_rotated(LayerHandle layer, float angle_rad) {
    const CgLayer* rec = find_layer(layer.id);
    if (!ctx_ || rec == nullptr || rec->image == nullptr) return;

    const float cx = rec->bounds.x + rec->bounds.width * 0.5f;
    const float cy = rec->bounds.y + rec->bounds.height * 0.5f;

    CGContextSaveGState(ctx_);
    // Rotate about the layer's own center, in the caller's y-down space.
    CGContextTranslateCTM(ctx_, cx, cy);
    CGContextRotateCTM(ctx_, angle_rad);
    CGContextTranslateCTM(ctx_, -cx, -cy);
    // Then the usual image counter-flip.
    CGContextTranslateCTM(ctx_, rec->bounds.x, rec->bounds.y + rec->bounds.height);
    CGContextScaleCTM(ctx_, 1.0, -1.0);
    CGContextDrawImage(ctx_,
                       CGRectMake(0, 0, rec->bounds.width, rec->bounds.height),
                       rec->image);
    CGContextRestoreGState(ctx_);

    if (!rec->cacheable) invalidate_layer(layer);
}

bool CoreGraphicsCanvas::layer_valid(LayerHandle layer) const {
    const CgLayer* rec = find_layer(layer.id);
    // "Sealed and still resident" — an image exists that can be composited
    // WITHOUT re-recording. A layer still open for recording is not valid.
    return rec != nullptr && rec->image != nullptr;
}

void CoreGraphicsCanvas::invalidate_layer(LayerHandle layer) {
    if (layer.id == 0) return;
    for (auto it = layers_.begin(); it != layers_.end(); ++it) {
        if (it->first == layer.id) {
            release_layer(it->second);
            layers_.erase(it);
            return;
        }
    }
}

}  // namespace pulp::canvas

#endif  // __APPLE__
