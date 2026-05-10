#pragma once

#include <pulp/canvas/canvas.hpp>

#include <vector>

#ifdef __APPLE__

// Forward declare CG types
typedef struct CGContext* CGContextRef;
typedef struct CGPath* CGMutablePathRef;
typedef struct CGImage* CGImageRef;
typedef struct CGPattern* CGPatternRef;

namespace pulp::canvas {

// CoreGraphics-backed Canvas implementation for macOS
// Renders directly into a CGContext (from NSView drawRect: or CGBitmapContext)
class CoreGraphicsCanvas : public Canvas {
public:
    explicit CoreGraphicsCanvas(CGContextRef ctx, float width, float height);
    ~CoreGraphicsCanvas() override;

    void save() override;
    void restore() override;
    int save_count() const override { return save_depth_; }
    void restore_to_count(int target) override;
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;
    void concat_transform(float a, float b, float c,
                          float d, float e, float f) override;
    void set_transform(float a, float b, float c,
                       float d, float e, float f) override;
    void capture_paint_baseline_transform() override;
    AffineTransform2x3 current_transform() const override;
    void clip_rect(float x, float y, float w, float h) override;
    void clip(FillRule rule = FillRule::nonzero) override;

    void set_fill_color(Color c) override;
    void set_stroke_color(Color c) override;
    void set_line_width(float w) override;
    void set_line_cap(LineCap cap) override;
    void set_line_join(LineJoin join) override;

    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.miterLimit and
    // imageSmoothingEnabled / Quality. Stored on the canvas; CGContext's
    // CGContextSetMiterLimit / CGContextSetInterpolationQuality apply
    // immediately (no per-draw re-push needed since they hang off the
    // current GState).
    void set_miter_limit(float limit) override;
    void set_image_smoothing(bool enabled,
                             ImageSmoothingQuality quality) override;

    void set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                   const Color* colors, const float* positions,
                                   int count) override;
    void set_fill_gradient_radial(float cx, float cy, float radius,
                                   const Color* colors, const float* positions,
                                   int count) override;
    /// pulp #1524 — true two-circle radial gradient via
    /// CGContextDrawRadialGradient(inner_center, inner_r, outer_center, outer_r).
    /// The single-circle override silently dropped (x0,y0,r0); this honours both.
    void set_fill_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                  const Color* colors, const float* positions,
                                  int count) override;
    void clear_fill_gradient() override;

    // pulp #1666 — stroke-side gradient overrides. Parallel to fill-side
    // GradientKind state so stroke calls can route through
    // stroke_with_active_paint() when a gradient is active. Without these
    // overrides the base no-op meant stroke draws collapsed to first-stop
    // color even when the JS shim set createLinearGradient() as
    // strokeStyle.
    void set_stroke_gradient_linear(float x0, float y0, float x1, float y1,
                                     const Color* colors, const float* positions,
                                     int count) override;
    void set_stroke_gradient_radial(float cx, float cy, float radius,
                                     const Color* colors, const float* positions,
                                     int count) override;
    void set_stroke_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) override;
    void set_stroke_gradient_conic(float cx, float cy, float start_angle,
                                    const Color* colors, const float* positions,
                                    int count) override;
    void clear_stroke_gradient() override;

    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createPattern. CG
    // has no first-class pattern shader (CGPattern requires a custom
    // CGPatternRef + tiling closure dance), so degrade silently to the
    // active fill / stroke colour. Same shape as the conic-gradient
    // fallback: the call records the intent, but visually the canvas
    // continues with the previous solid paint. File a follow-up if a
    // CG-targeted plugin actually needs tiled image patterns.
    void set_fill_pattern(const std::string& image_src,
                          PatternTileMode tile_x,
                          PatternTileMode tile_y) override;
    void set_stroke_pattern(const std::string& image_src,
                            PatternTileMode tile_x,
                            PatternTileMode tile_y) override;

    void fill_rect(float x, float y, float w, float h) override;
    void clear_rect(float x, float y, float w, float h) override;
    void stroke_rect(float x, float y, float w, float h) override;
    void fill_rounded_rect(float x, float y, float w, float h, float radius) override;
    void stroke_rounded_rect(float x, float y, float w, float h, float radius) override;
    void fill_circle(float cx, float cy, float radius) override;
    void stroke_circle(float cx, float cy, float radius) override;
    void stroke_arc(float cx, float cy, float radius,
                   float start_angle, float end_angle) override;
    void stroke_line(float x0, float y0, float x1, float y1) override;
    void stroke_path(const Point2D* points, size_t count) override;
    void fill_path(const Point2D* points, size_t count) override;

    // Canvas2D-style path building (pulp #1322).
    void begin_path() override;
    void move_to(float x, float y) override;
    void line_to(float x, float y) override;
    void quad_to(float cpx, float cpy, float x, float y) override;
    void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y,
                  float x, float y) override;
    void close_path() override;
    void fill_current_path(FillRule rule = FillRule::nonzero) override;
    void stroke_current_path() override;

    // pulp #1521 — native arc subpaths via CGPath APIs.
    void arc(float cx, float cy, float radius,
             float start_angle, float end_angle,
             bool anticlockwise) override;
    void arc_to(float x1, float y1, float x2, float y2,
                float radius) override;
    void ellipse(float cx, float cy, float rx, float ry,
                 float rotation,
                 float start_angle, float end_angle,
                 bool anticlockwise) override;
    void round_rect(float x, float y, float w, float h,
                    float tl_x, float tl_y,
                    float tr_x, float tr_y,
                    float br_x, float br_y,
                    float bl_x, float bl_y) override;

    void set_opacity(float alpha) override;
    void save_layer(float x, float y, float w, float h,
                    float opacity, float blur_radius) override;

    // pulp #1371 — Canvas2D globalCompositeOperation parity. Without this
    // override the base Canvas no-op default silently dropped every blend
    // request on the CPU paint path, so JS bundles using
    // ctx.globalCompositeOperation = 'lighter'/'multiply'/etc. drew with the
    // default SrcOver — Spectr's filterbank rainbow gradient is the canonical
    // repro. CG's GState stack matches Canvas2D save()/restore() blend-mode
    // semantics, so we just push the chosen CGBlendMode into the current GState
    // and let CG carry it across draws + save/restore frames.
    void set_blend_mode(BlendMode mode) override;

    void set_font(const std::string& family, float size) override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    // pulp #1525 — Canvas2D fillText(text,x,y,maxWidth) + strokeText(text,x,y,maxWidth).
    void fill_text_with_max_width(const std::string& text,
                                  float x, float y, float max_width) override;
    void stroke_text(const std::string& text, float x, float y,
                     float max_width = 0.0f) override;
    float measure_text(const std::string& text) override;
    TextMetrics measure_text_full(const std::string& text) override;

    // Canvas2D drop-shadow state (issue-1434 batch 7). CGContext exposes
    // sticky shadow state directly via CGContextSetShadowWithColor — we
    // mirror Canvas2D's sticky `ctx.shadow*` semantics by translating
    // each setter into a CG state mutation that hangs off the current
    // GState (so save/restore correctly snapshots and pops the shadow).
    void set_shadow_color(Color color) override;
    void set_shadow_blur(float blur) override;
    void set_shadow_offset_x(float dx) override;
    void set_shadow_offset_y(float dy) override;

private:
    CGContextRef ctx_;
    float width_, height_;
    int in_transparency_layer_ = 0;
    Color fill_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    Color stroke_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    std::string font_family_ = "Helvetica";
    float font_size_ = 14.0f;
    TextAlign text_align_ = TextAlign::left;

    // Canvas2D path-building state (pulp #1322). Owned, retained CG mutable
    // path; null until the first begin_path() call.
    CGMutablePathRef path_ = nullptr;

    // Linear/radial gradient state used as the fill paint when
    // has_gradient_ is true. Mirrors the SkiaCanvas split between solid
    // fill and shader-based fill so JS-driven gradient fills paint instead
    // of falling back to a single color.
    //
    // pulp #1524 — gradient_kind_ identifies which Draw* call to issue from
    // fill_with_active_paint(). `radial_two_circles` is the spec-correct
    // two-circle form (inner + outer centres, inner_r in grad_radius_inner_,
    // outer_r in grad_radius_); `conic_image` is a software-rasterised
    // CGImage that we paint via CGContextDrawImage inside the active clip.
    enum class GradientKind { none, linear, radial, radial_two_circles, conic_image };
    bool has_gradient_ = false;
    GradientKind gradient_kind_ = GradientKind::none;
    bool gradient_is_radial_ = false;  // legacy mirror of (kind == radial), kept for older inline checks
    float grad_x0_ = 0, grad_y0_ = 0, grad_x1_ = 0, grad_y1_ = 0;
    float grad_radius_ = 0;        // outer / single radius
    float grad_radius_inner_ = 0;  // pulp #1524 — inner radius for two-circle radial
    std::vector<Color> grad_colors_;
    std::vector<float> grad_positions_;

    // pulp #1666 — parallel stroke-side state. Same shape as the fill
    // gradient slots; checked by stroke_with_active_paint() at every
    // stroke site that previously called apply_stroke_color directly.
    bool has_stroke_gradient_ = false;
    GradientKind stroke_gradient_kind_ = GradientKind::none;
    float stroke_grad_x0_ = 0, stroke_grad_y0_ = 0, stroke_grad_x1_ = 0, stroke_grad_y1_ = 0;
    float stroke_grad_radius_ = 0;
    float stroke_grad_radius_inner_ = 0;
    std::vector<Color> stroke_grad_colors_;
    std::vector<float> stroke_grad_positions_;

    // pulp #1524 — software-rasterised conic gradient. CG has no native
    // conic shader, so set_fill_gradient_conic walks every pixel of a
    // bounding-box bitmap, computes the angle from the centre, interpolates
    // the colour stops, and stores the resulting CGImage here. fill_with_active_paint
    // paints it via CGContextDrawImage inside the active clip. The image is
    // released in clear_fill_gradient / dtor / on next gradient set.
    CGImageRef conic_image_ = nullptr;
    float conic_image_x_ = 0, conic_image_y_ = 0;
    float conic_image_w_ = 0, conic_image_h_ = 0;
    void release_conic_image();

    // pulp #1524 — tiled-pattern fill state. CG exposes patterns via
    // CGPatternCreate + a draw callback; we hold the decoded image (so
    // the callback can render a tile) and the tile dimensions / repetition
    // mode here. has_pattern_ short-circuits fill_with_active_paint onto
    // CGContextSetFillPattern + CGContextFillRect/CGContextFillPath.
    bool has_pattern_ = false;
    CGImageRef pattern_image_ = nullptr;
    PatternTileMode pattern_tile_x_ = PatternTileMode::repeat;
    PatternTileMode pattern_tile_y_ = PatternTileMode::repeat;
    void release_pattern_image();

    // Paint-baseline transform captured at CanvasWidget::paint() entry,
    // matching SkiaCanvas so JS-supplied setTransform() composes onto the
    // inbound View matrix instead of overwriting it.
    bool has_baseline_ = false;
    // Storage for CGAffineTransform — declared as raw doubles so the
    // header doesn't pull <CoreGraphics/CoreGraphics.h>. Six-element layout:
    //   [a, b, c, d, tx, ty] (CGAffineTransform memory layout).
    double baseline_xform_[6] = {1, 0, 0, 1, 0, 0};

    // pulp #1368 — manual GState depth tracking. CG doesn't expose a
    // saveCount() API, so save() increments and restore() decrements
    // this counter; restore_to_count() pops repeatedly until depth
    // matches the requested target. CanvasWidget::paint() snapshots the
    // depth at entry and pops back to it at exit so an unbalanced JS
    // ctx.save() can't leak GState into the parent View's paint scope.
    int save_depth_ = 0;

    // Canvas2D shadow* state (issue-1434 batch 7). CGContext owns the
    // sticky shadow via its GState stack, so save()/restore() naturally
    // snapshots both ours and CG's. We hold the values here so a
    // setter mutation re-pushes the combined state through
    // CGContextSetShadowWithColor.
    Color shadow_color_ = Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
    float shadow_blur_     = 0.0f;
    float shadow_offset_x_ = 0.0f;
    float shadow_offset_y_ = 0.0f;
    void apply_shadow_to_context();

    void apply_fill_color();
    void apply_stroke_color();
    void add_rounded_rect_path(float x, float y, float w, float h, float radius);
    // Fills (or clips to) the current path or rect using either the solid
    // fill_color_ or the active gradient when has_gradient_ is set.
    void fill_with_active_paint();
    // Strokes the current path using either apply_stroke_color or, when
    // has_stroke_gradient_ is set, draws the active stroke gradient
    // clipped to the stroked-path outline (via CGContextReplacePathWithStrokedPath).
    void stroke_with_active_paint();
    void release_path();
};

} // namespace pulp::canvas

#endif // __APPLE__
