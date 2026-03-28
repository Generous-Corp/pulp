#pragma once

#include <pulp/canvas/canvas.hpp>

#ifdef PULP_HAS_SKIA

// Forward declare Skia types to avoid header dependency in public API
class SkCanvas;
class SkSurface;
class SkFont;
class SkPaint;

namespace pulp::canvas {

// Skia-backed Canvas implementation for GPU-accelerated rendering
// Uses Skia Graphite when available, falls back to Ganesh or CPU
class SkiaCanvas : public Canvas {
public:
    // Create wrapping an existing SkCanvas (e.g., from a surface)
    explicit SkiaCanvas(SkCanvas* canvas);
    ~SkiaCanvas() override;

    // ── State ────────────────────────────────────────────────────────────
    void save() override;
    void restore() override;

    // ── Transform ────────────────────────────────────────────────────────
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;

    // ── Clipping ─────────────────────────────────────────────────────────
    void clip_rect(float x, float y, float w, float h) override;

    // ── Fill and stroke ──────────────────────────────────────────────────
    void set_fill_color(Color c) override;
    void set_stroke_color(Color c) override;
    void set_line_width(float w) override;
    void set_line_cap(LineCap cap) override;
    void set_line_join(LineJoin join) override;

    // ── Shapes ───────────────────────────────────────────────────────────
    void fill_rect(float x, float y, float w, float h) override;
    void stroke_rect(float x, float y, float w, float h) override;
    void fill_rounded_rect(float x, float y, float w, float h, float radius) override;
    void stroke_rounded_rect(float x, float y, float w, float h, float radius) override;
    void fill_circle(float cx, float cy, float radius) override;
    void stroke_circle(float cx, float cy, float radius) override;

    // ── Arcs ─────────────────────────────────────────────────────────────
    void stroke_arc(float cx, float cy, float radius,
                   float start_angle, float end_angle) override;

    // ── Lines ────────────────────────────────────────────────────────────
    void stroke_line(float x0, float y0, float x1, float y1) override;

    // ── Text ─────────────────────────────────────────────────────────────
    void set_font(const std::string& family, float size) override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    float measure_text(const std::string& text) override;
    TextMetrics measure_text_full(const std::string& text) override;
    void draw_waveform(const float* samples, size_t count,
                       float x, float y, float width, float height,
                       const WaveformStyle& style) override;
    void draw_blurred_backdrop(float x, float y, float w, float h,
                               float blur_radius, float corner_radius,
                               Color tint) override;

private:
    SkCanvas* canvas_;        // Non-owning — owned by surface or caller
    Color fill_color_{255, 255, 255, 255};
    Color stroke_color_{255, 255, 255, 255};
    float line_width_ = 1.0f;
    LineCap line_cap_ = LineCap::butt;
    LineJoin line_join_ = LineJoin::miter;
    std::string font_family_ = "sans-serif";
    TextAlign text_align_ = TextAlign::left;
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
