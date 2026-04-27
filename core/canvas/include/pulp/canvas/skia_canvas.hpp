#pragma once

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>

#ifdef PULP_HAS_SKIA

// Forward declare Skia types to avoid header dependency in public API
class SkCanvas;
class SkSurface;
class SkFont;
class SkPaint;
class SkPathBuilder;

// These need full definitions for member variables
#include "include/core/SkRefCnt.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkMatrix.h"
class SkShader;

namespace skgpu::graphite {
class Recorder;
}

namespace pulp::canvas {

// Skia-backed Canvas implementation for GPU-accelerated rendering
// Uses Skia Graphite when available, falls back to Ganesh or CPU
class SkiaCanvas : public Canvas {
public:
    // Create wrapping an existing SkCanvas (e.g., from a surface)
    explicit SkiaCanvas(SkCanvas* canvas, skgpu::graphite::Recorder* recorder = nullptr);
    ~SkiaCanvas() override;

    // ── State ────────────────────────────────────────────────────────────
    void save() override;
    void restore() override;

    // ── Transform ────────────────────────────────────────────────────────
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;
    void set_transform(float a, float b, float c,
                       float d, float e, float f) override;
    void capture_paint_baseline_transform() override;

    // ── Clipping ─────────────────────────────────────────────────────────
    void clip_rect(float x, float y, float w, float h) override;
    void clip() override;

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
    void fill_text_sdf(const std::string& text, float x, float y,
                       const SdfAtlas& atlas) override;
    float measure_text(const std::string& text) override;
    TextMetrics measure_text_full(const std::string& text) override;

    // ── Images ──────────────────────────────────────────────────────────
    bool draw_image_from_data(const uint8_t* data, size_t size,
                              float x, float y, float w, float h) override;
    bool draw_image_from_file(const std::string& path,
                               float x, float y, float w, float h) override;
    void draw_waveform(const float* samples, size_t count,
                       float x, float y, float width, float height,
                       const WaveformStyle& style) override;
    // Gradients
    void set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                   const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_radial(float cx, float cy, float radius,
                                   const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                  const Color* colors, const float* positions, int count) override;
    void clear_fill_gradient() override;

    // Blend modes
    void set_blend_mode(BlendMode mode) override;

    // Path building
    void begin_path() override;
    void move_to(float x, float y) override;
    void line_to(float x, float y) override;
    void quad_to(float cpx, float cpy, float x, float y) override;
    void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) override;
    void close_path() override;
    void fill_current_path() override;
    void stroke_current_path() override;

    // SDF shapes
    void draw_sdf_shape(SDFShape shape, float x, float y, float w, float h,
                        const SDFStyle& style) override;
    void draw_blurred_backdrop(float x, float y, float w, float h,
                               float blur_radius, float corner_radius,
                               Color tint) override;

    // Custom SkSL shader rendering (GPU-accelerated)
    bool draw_with_sksl(const std::string& sksl,
                        float x, float y, float w, float h,
                        const ShaderUniforms& uniforms) override;

    // Draw a Dawn-backed texture into the current Skia canvas when Graphite is active.
    bool draw_native_dawn_texture(void* texture_handle,
                                  uint32_t width,
                                  uint32_t height,
                                  const std::string& format,
                                  float x,
                                  float y,
                                  float w,
                                  float h);

    // Opacity & compositing layers
    void set_opacity(float alpha) override;
    void save_layer(float x, float y, float w, float h,
                    float opacity, float blur_radius) override;

private:
    SkCanvas* canvas_;        // Non-owning — owned by surface or caller
    skgpu::graphite::Recorder* recorder_ = nullptr; // Non-owning — owned by SkiaSurface
    Color fill_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    Color stroke_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    float line_width_ = 1.0f;
    LineCap line_cap_ = LineCap::butt;
    LineJoin line_join_ = LineJoin::miter;
    std::string font_family_ = "sans-serif";
    TextAlign text_align_ = TextAlign::left;

    // Gradient state
    bool has_gradient_ = false;
    sk_sp<SkShader> gradient_shader_;

    // Path building state
    std::unique_ptr<SkPathBuilder> path_builder_;

    // Blend mode
    SkBlendMode blend_mode_ = SkBlendMode::kSrcOver;

    // Paint-baseline matrix — captured at CanvasWidget::paint() entry so that
    // JS-driven setTransform() composes onto the inbound View transform
    // instead of overwriting it. Defaults to identity for callers (e.g.
    // screenshot host) that drive SkiaCanvas directly without going through
    // CanvasWidget — there setTransform behaves per the HTML spec literally.
    SkMatrix paint_baseline_ = SkMatrix::I();
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
