#pragma once

#include <pulp/canvas/canvas.hpp>

#ifdef __APPLE__

// Forward declare CGContext
typedef struct CGContext* CGContextRef;

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
    void clip_rect(float x, float y, float w, float h) override;

    void set_fill_color(Color c) override;
    void set_stroke_color(Color c) override;
    void set_line_width(float w) override;
    void set_line_cap(LineCap cap) override;
    void set_line_join(LineJoin join) override;

    void fill_rect(float x, float y, float w, float h) override;
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

    void apply_fill_color();
    void apply_stroke_color();
    void add_rounded_rect_path(float x, float y, float w, float h, float radius);
};

} // namespace pulp::canvas

#endif // __APPLE__
