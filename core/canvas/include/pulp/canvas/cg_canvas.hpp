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
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;
    void concat_transform(float a, float b, float c,
                          float d, float e, float f) override;
    void set_transform(float a, float b, float c,
                       float d, float e, float f) override;
    void capture_paint_baseline_transform() override;
    void clip_rect(float x, float y, float w, float h) override;
    void clip() override;

    void set_fill_color(Color c) override;
    void set_stroke_color(Color c) override;
    void set_line_width(float w) override;
    void set_line_cap(LineCap cap) override;
    void set_line_join(LineJoin join) override;

    void set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                   const Color* colors, const float* positions,
                                   int count) override;
    void set_fill_gradient_radial(float cx, float cy, float radius,
                                   const Color* colors, const float* positions,
                                   int count) override;
    void clear_fill_gradient() override;

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

    void set_font(const std::string& family, float size) override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    float measure_text(const std::string& text) override;
    TextMetrics measure_text_full(const std::string& text) override;

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
