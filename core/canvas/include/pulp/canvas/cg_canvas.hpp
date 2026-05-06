#pragma once

#include <pulp/canvas/canvas.hpp>

#include <vector>

#ifdef __APPLE__

// Forward declare CG types
typedef struct CGContext* CGContextRef;
typedef struct CGPath* CGMutablePathRef;

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
    void clip() override;

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
    void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                  const Color* colors, const float* positions,
                                  int count) override;
    void clear_fill_gradient() override;

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
    void fill_current_path() override;
    void stroke_current_path() override;

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
    bool has_gradient_ = false;
    bool gradient_is_radial_ = false;
    float grad_x0_ = 0, grad_y0_ = 0, grad_x1_ = 0, grad_y1_ = 0;
    float grad_radius_ = 0;
    std::vector<Color> grad_colors_;
    std::vector<float> grad_positions_;

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
    void release_path();
};

} // namespace pulp::canvas

#endif // __APPLE__
