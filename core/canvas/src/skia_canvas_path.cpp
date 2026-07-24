// skia_canvas_path.cpp — retained Path draws and handle-based compositing
// layers for the Skia backend.
//
// Split out of skia_canvas.cpp for the same reason as skia_canvas_box_shadow /
// skia_canvas_opacity: the feature is self-contained and skia_canvas.cpp is
// already long. Shared helpers come from skia_canvas_internal.hpp.

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/path.hpp>
#include <pulp/canvas/skia_canvas.hpp>

#include "skia_canvas_internal.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Surface.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace pulp::canvas {

class SkiaCanvas::RetainedLayerStore {
public:
    std::vector<std::pair<uint64_t, SkiaLayer>> layers;
    const void* owner = nullptr;
    uint8_t backend_kind = 0;
};

std::shared_ptr<SkiaCanvas::RetainedLayerStore>
SkiaCanvas::create_retained_layer_store() {
    return std::make_shared<RetainedLayerStore>();
}

namespace {

uint64_t next_layer_id() {
    static std::atomic<uint64_t> next{1};
    uint64_t candidate = next.load(std::memory_order_relaxed);
    while (candidate != 0 &&
           candidate != std::numeric_limits<uint64_t>::max()) {
        if (next.compare_exchange_weak(candidate,
                                       candidate + 1,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
            return candidate;
        }
    }
    // Never recycle an ID: a stale handle must not resolve to new content.
    return 0;
}

/// Largest offscreen layer we will allocate, per axis. A layer bounds fed in
/// from a broken layout pass (or a hostile design import) can otherwise ask
/// for a multi-gigabyte surface.
constexpr int kMaxLayerDimension = 8192;

SkPath to_sk_path(const Path& path, FillRule rule) {
    SkPathBuilder b;
    for (Path::Element el : path) {
        switch (el.verb) {
            case Path::Verb::move:
                b.moveTo(el.points[0].x, el.points[0].y);
                break;
            case Path::Verb::line:
                b.lineTo(el.points[0].x, el.points[0].y);
                break;
            case Path::Verb::quad:
                b.quadTo(el.points[0].x, el.points[0].y,
                         el.points[1].x, el.points[1].y);
                break;
            case Path::Verb::cubic:
                b.cubicTo(el.points[0].x, el.points[0].y,
                          el.points[1].x, el.points[1].y,
                          el.points[2].x, el.points[2].y);
                break;
            case Path::Verb::close:
                b.close();
                break;
        }
    }
    b.setFillType(rule == FillRule::evenodd ? SkPathFillType::kEvenOdd
                                            : SkPathFillType::kWinding);
    return b.detach();
}

SkPaint::Cap to_sk_cap(LineCap cap) {
    switch (cap) {
        case LineCap::round:  return SkPaint::kRound_Cap;
        case LineCap::square: return SkPaint::kSquare_Cap;
        case LineCap::butt:   break;
    }
    return SkPaint::kButt_Cap;
}

SkPaint::Join to_sk_join(LineJoin join) {
    switch (join) {
        case LineJoin::round: return SkPaint::kRound_Join;
        case LineJoin::bevel: return SkPaint::kBevel_Join;
        case LineJoin::miter: break;
    }
    return SkPaint::kMiter_Join;
}

}  // namespace

void SkiaCanvas::bind_retained_layer_store(const void* owner,
                                           uint8_t backend_kind) {
    if (!owner || backend_kind == 0) return;
    if (retained_layers_->owner &&
        (retained_layers_->owner != owner ||
         retained_layers_->backend_kind != backend_kind)) {
        // GPU images cannot cross the recorder/context that created them.
        retained_layers_->layers.clear();
    }
    retained_layers_->owner = owner;
    retained_layers_->backend_kind = backend_kind;
}

SkiaCanvas::SkiaCanvas(SkCanvas* canvas, skgpu::graphite::Recorder* recorder)
    : SkiaCanvas(canvas, recorder, create_retained_layer_store()) {
    // A private per-instance store cannot promise persistence when a host
    // replaces this canvas on the next frame.
    retained_layer_cache_supported_ = false;
}

SkiaCanvas::SkiaCanvas(
    SkCanvas* canvas,
    skgpu::graphite::Recorder* recorder,
    std::shared_ptr<RetainedLayerStore> retained_layers)
    : canvas_(canvas),
      recorder_(recorder),
      retained_layers_(retained_layers
                           ? retained_layers
                           : create_retained_layer_store()),
      retained_layer_cache_supported_(retained_layers != nullptr) {
    if (recorder_) bind_retained_layer_store(recorder_, 1);
}

SkiaCanvas::~SkiaCanvas() {
    // Unbalanced layers are canvas-local. Unwind the redirects before their
    // temporary surfaces are released.
    while (!open_layers_.empty()) {
        canvas_ = open_layers_.back().previous_canvas;
        open_layers_.pop_back();
    }
}

void SkiaCanvas::set_gpu_upload_context(GrDirectContext* context) {
    gr_context_ = context;
    if (context) bind_retained_layer_store(context, 2);
}

// ── Retained path draws ──────────────────────────────────────────────────

void SkiaCanvas::fill_path(const Path& path, FillRule rule) {
    if (!canvas_ || path.is_empty()) return;

    SkPaint paint;
    paint.setAntiAlias(true);
    // Mirror fill_current_path's paint construction: an active gradient shader
    // wins over the solid fill color.
    if (has_gradient_ && gradient_shader_) {
        paint.setShader(gradient_shader_);
    } else {
        paint.setColor4f(to_sk_color4f(fill_color_));
    }
    paint.setBlendMode(blend_mode_);
    apply_shadow_filter(paint);
    apply_filter(paint);

    canvas_->drawPath(to_sk_path(path, rule), paint);
}

void SkiaCanvas::stroke_path(const Path& path, const StrokeStyle& style) {
    if (!canvas_ || path.is_empty()) return;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setColor4f(to_sk_color4f(stroke_color_));
    paint.setBlendMode(blend_mode_);

    // The style is the single source of truth for this draw — deliberately NOT
    // merged with the canvas's sticky line_width_ / line_cap_ state, so a
    // stroke_path(path, style) draws exactly `style` no matter what the
    // immediate-mode setters were last left on.
    paint.setStrokeWidth(style.width);
    paint.setStrokeCap(to_sk_cap(style.cap));
    paint.setStrokeJoin(to_sk_join(style.join));
    paint.setStrokeMiter(style.miter_limit);

    if (!style.dash.empty()) {
        // SVG / Canvas2D rule: an odd-length dash pattern repeats to make it
        // even. Skia requires an even count and asserts otherwise.
        std::vector<SkScalar> intervals(style.dash.begin(), style.dash.end());
        if (intervals.size() % 2 == 1)
            intervals.insert(intervals.end(), style.dash.begin(), style.dash.end());
        // An all-zero pattern is a degenerate no-op in Skia; skip it.
        const bool any_positive =
            std::any_of(intervals.begin(), intervals.end(),
                        [](SkScalar v) { return v > 0.0f; });
        if (any_positive) {
            paint.setPathEffect(SkDashPathEffect::Make(
                SkSpan<const SkScalar>(intervals.data(), intervals.size()),
                style.dash_phase));
        }
    }

    if (stroke_shader_) paint.setShader(stroke_shader_);
    apply_shadow_filter(paint);
    apply_filter(paint);

    canvas_->drawPath(to_sk_path(path, FillRule::nonzero), paint);
}

void SkiaCanvas::clip_path(const Path& path, FillRule rule) {
    if (!canvas_ || path.is_empty()) return;
    canvas_->clipPath(to_sk_path(path, rule), /*doAntiAlias=*/true);
}

void SkiaCanvas::draw_path_shadow(const Path& path, float dx, float dy,
                                  float blur, float spread, Color color) {
    if (!canvas_ || path.is_empty()) return;

    // Grow the silhouette by `spread` BEFORE blurring, per the CSS box-shadow
    // model. Scaling about the center is exact for a convex path and an
    // approximation for a concave one (a true spread is a path outset, which
    // Skia does not expose as a cheap primitive).
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

    // Skia's drop-shadow sigma is roughly half the CSS blur radius — the same
    // conversion skia_canvas_box_shadow.cpp uses.
    const float sigma = std::max(0.0f, blur * 0.5f);

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setBlendMode(blend_mode_);
    // DropShadowOnly renders the shadow WITHOUT the source geometry — which is
    // the entire contract of this method. (DropShadow, without the suffix,
    // would also paint the path itself.)
    paint.setImageFilter(SkImageFilters::DropShadowOnly(
        dx, dy, sigma, sigma, to_sk_color4f(color).toSkColor(), nullptr));

    canvas_->drawPath(to_sk_path(shadow, FillRule::nonzero), paint);
}

// ── Retained compositing layers ──────────────────────────────────────────

SkiaCanvas::SkiaLayer* SkiaCanvas::find_layer(uint64_t id) {
    if (id == 0) return nullptr;
    for (auto& [lid, rec] : retained_layers_->layers)
        if (lid == id) return &rec;
    return nullptr;
}

const SkiaCanvas::SkiaLayer* SkiaCanvas::find_layer(uint64_t id) const {
    if (id == 0) return nullptr;
    for (const auto& [lid, rec] : retained_layers_->layers)
        if (lid == id) return &rec;
    return nullptr;
}

Canvas::LayerHandle SkiaCanvas::begin_layer(Rect2D bounds, bool cacheable) {
    if (!canvas_ || bounds.width <= 0.0f || bounds.height <= 0.0f)
        return LayerHandle{};

    // Allocate the offscreen at DEVICE resolution, so a layer recorded under a
    // 2x DPI transform is not resampled up from a 1x texture and does not go
    // soft. The scale is read off the live CTM (including any skew).
    const SkMatrix m = canvas_->getTotalMatrix();
    float sx = std::hypot(m.getScaleX(), m.getSkewY());
    float sy = std::hypot(m.getScaleY(), m.getSkewX());
    if (!(sx > 0.0f) || !std::isfinite(sx)) sx = 1.0f;
    if (!(sy > 0.0f) || !std::isfinite(sy)) sy = 1.0f;

    const int pw = std::clamp(
        static_cast<int>(std::ceil(bounds.width * sx)), 1, kMaxLayerDimension);
    const int ph = std::clamp(
        static_cast<int>(std::ceil(bounds.height * sy)), 1, kMaxLayerDimension);

    const SkImageInfo info =
        SkImageInfo::MakeN32Premul(pw, ph, canvas_->imageInfo().refColorSpace());

    // Match the surface to whatever backend owns this canvas, so a GPU layer
    // stays on the GPU instead of round-tripping through system memory.
    sk_sp<SkSurface> surface;
    if (recorder_ != nullptr) {
        surface = SkSurfaces::RenderTarget(recorder_, info);
    } else if (gr_context_ != nullptr) {
        surface = SkSurfaces::RenderTarget(gr_context_, skgpu::Budgeted::kYes, info);
    }
    if (!surface) surface = SkSurfaces::Raster(info);
    if (!surface) return LayerHandle{};  // out of memory — caller draws direct

    SkCanvas* lc = surface->getCanvas();
    if (lc == nullptr) return LayerHandle{};
    lc->clear(SK_ColorTRANSPARENT);
    // Map the caller's user space onto the layer's pixels: content drawn at
    // bounds.x/bounds.y lands at the layer's top-left.
    lc->scale(sx, sy);
    lc->translate(-bounds.x, -bounds.y);

    const uint64_t id = next_layer_id();
    if (id == 0) return LayerHandle{};
    SkiaLayer rec;
    rec.surface = surface;
    rec.bounds = bounds;
    rec.scale_x = sx;
    rec.scale_y = sy;
    rec.cacheable = cacheable;
    open_layers_.push_back(OpenLayer{id, canvas_, std::move(rec), false});
    canvas_ = lc;  // redirect every subsequent draw into the layer
    return LayerHandle{id};
}

Canvas::LayerHandle SkiaCanvas::end_layer() {
    if (open_layers_.empty()) return LayerHandle{};  // unbalanced — ignore

    OpenLayer open = std::move(open_layers_.back());
    open_layers_.pop_back();
    canvas_ = open.previous_canvas;  // stop drawing into the layer

    if (open.cancelled) return LayerHandle{};

    if (open.layer.surface) {
        // Seal it. The snapshot is what OUTLIVES the frame — this is the step
        // a scoped save_layer/restore pair can never hand back to a caller.
        open.layer.image = open.layer.surface->makeImageSnapshot();
        open.layer.surface.reset();  // the surface's job is done; free it
    }
    if (!open.layer.image) return LayerHandle{};

    // Only sealed images enter the renderer-owned cross-frame store. An open
    // surface stays local, so invalidation cannot leave a dangling SkCanvas.
    retained_layers_->layers.emplace_back(open.id, std::move(open.layer));
    return LayerHandle{open.id};
}

void SkiaCanvas::draw_layer_common(LayerHandle layer, const SkMatrix& placement,
                                   float alpha, BlendMode mode) {
    if (!canvas_) return;
    const SkiaLayer* rec = find_layer(layer.id);
    if (rec == nullptr || !rec->image) return;

    SkPaint paint;
    paint.setAlphaf(std::clamp(alpha, 0.0f, 1.0f));
    paint.setBlendMode(skia_blend_mode_for(mode));
    apply_filter(paint);

    canvas_->save();
    canvas_->concat(placement);
    canvas_->drawImageRect(
        rec->image,
        SkRect::MakeXYWH(rec->bounds.x, rec->bounds.y,
                         rec->bounds.width, rec->bounds.height),
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone), &paint);
    canvas_->restore();

    // A non-cacheable layer is consumed by its first draw — drop the texture
    // now rather than holding it to the end of the frame.
    if (!rec->cacheable) invalidate_layer(layer);
}

void SkiaCanvas::draw_layer(LayerHandle layer, float alpha, BlendMode mode) {
    draw_layer_common(layer, SkMatrix::I(), alpha, mode);
}

void SkiaCanvas::draw_layer_fitted(LayerHandle layer, Rect2D dest) {
    const SkiaLayer* rec = find_layer(layer.id);
    if (rec == nullptr || !rec->image) return;
    if (rec->bounds.width <= 0.0f || rec->bounds.height <= 0.0f) return;
    if (dest.width <= 0.0f || dest.height <= 0.0f) return;

    // Map the layer's recorded bounds onto `dest`.
    const SkMatrix placement = SkMatrix::RectToRect(
        SkRect::MakeXYWH(rec->bounds.x, rec->bounds.y,
                         rec->bounds.width, rec->bounds.height),
        SkRect::MakeXYWH(dest.x, dest.y, dest.width, dest.height),
        SkMatrix::kFill_ScaleToFit);

    draw_layer_common(layer, placement, 1.0f, BlendMode::normal);
}

void SkiaCanvas::draw_layer_rotated(LayerHandle layer, float angle_rad) {
    const SkiaLayer* rec = find_layer(layer.id);
    if (rec == nullptr || !rec->image) return;

    const float cx = rec->bounds.x + rec->bounds.width * 0.5f;
    const float cy = rec->bounds.y + rec->bounds.height * 0.5f;
    const SkMatrix placement = SkMatrix::RotateDeg(
        angle_rad * 180.0f / 3.14159265358979323846f, {cx, cy});

    draw_layer_common(layer, placement, 1.0f, BlendMode::normal);
}

bool SkiaCanvas::layer_valid(LayerHandle layer) const {
    const SkiaLayer* rec = find_layer(layer.id);
    // Valid means "sealed and still resident" — an image exists that can be
    // composited WITHOUT re-recording the contents. A layer still open for
    // recording is not valid.
    return rec != nullptr && rec->image != nullptr;
}

void SkiaCanvas::invalidate_layer(LayerHandle layer) {
    if (layer.id == 0) return;
    for (auto& open : open_layers_) {
        if (open.id == layer.id) {
            open.cancelled = true;
            return;
        }
    }
    auto& layers = retained_layers_->layers;
    for (auto it = layers.begin(); it != layers.end(); ++it) {
        if (it->first == layer.id) {
            layers.erase(it);
            return;
        }
    }
}

}  // namespace pulp::canvas

#endif  // PULP_HAS_SKIA
