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
// SkSamplingOptions appears as the return type of
// sampling_options_for_image_smoothing(); without the full type the
// declaration breaks compilation on consumers that include this
// header without separately bringing in SkSamplingOptions.h.
#include "include/core/SkSamplingOptions.h"
#include <vector>
class SkShader;
class SkPathEffect;
class SkImageFilter;

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
    int save_count() const override;
    void restore_to_count(int target) override;

    // ── Transform ────────────────────────────────────────────────────────
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;
    void set_transform(float a, float b, float c,
                       float d, float e, float f) override;
    void capture_paint_baseline_transform() override;
    void concat_transform(float a, float b, float c,
                          float d, float e, float f) override;
    AffineTransform2x3 current_transform() const override;

    // ── Clipping ─────────────────────────────────────────────────────────
    void clip_rect(float x, float y, float w, float h) override;
    void clip(FillRule rule = FillRule::nonzero) override;
    void clip_path_svg(const std::string& svg_path_d) override;

    // ── Fill and stroke ──────────────────────────────────────────────────
    void set_fill_color(Color c) override;
    void set_stroke_color(Color c) override;
    void set_line_width(float w) override;
    void set_line_cap(LineCap cap) override;
    void set_line_join(LineJoin join) override;

    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.miterLimit and
    // ctx.imageSmoothingEnabled / ctx.imageSmoothingQuality. Sticky paint
    // state honored by subsequent stroke / drawImage calls.
    void set_miter_limit(float limit) override;
    void set_image_smoothing(bool enabled,
                             ImageSmoothingQuality quality) override;

    // ── Shapes ───────────────────────────────────────────────────────────
    void fill_rect(float x, float y, float w, float h) override;
    void clear_rect(float x, float y, float w, float h) override;
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
    void set_font_full(const std::string& family, float size,
                       int weight, int slant, float letter_spacing) override;
    // pulp #1737 — OpenType feature flags via SkShaper Feature API.
    // Captured into font_features_ vector and flushed at shape time.
    void set_font_features(std::vector<FontFeature> features) override;
    void clear_font_features() override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    // pulp #1525 — Canvas2D fillText(text,x,y,maxWidth) + strokeText(text,x,y,maxWidth).
    void fill_text_with_max_width(const std::string& text,
                                  float x, float y, float max_width) override;
    void stroke_text(const std::string& text, float x, float y,
                     float max_width = 0.0f) override;
    void fill_text_sdf(const std::string& text, float x, float y,
                       const SdfAtlas& atlas) override;
    float measure_text(const std::string& text) override;
    TextMetrics measure_text_full(const std::string& text) override;

    // ── Images ──────────────────────────────────────────────────────────
    bool draw_image_from_data(const uint8_t* data, size_t size,
                              float x, float y, float w, float h) override;
    bool draw_image_from_file(const std::string& path,
                               float x, float y, float w, float h) override;
    // pulp #1737 — 9-arg drawImage source-rect form: routes through
    // SkCanvas::drawImageRect(image, src, dst, sampling) so a sub-rect of
    // the decoded SkImage maps onto the destination rect. Supports
    // sprite-sheet slicing without re-decoding the source bitmap.
    bool draw_image_from_data_rect(const uint8_t* data, size_t size,
                                    float sx, float sy, float sw, float sh,
                                    float dx, float dy, float dw, float dh) override;
    bool draw_image_from_file_rect(const std::string& path,
                                    float sx, float sy, float sw, float sh,
                                    float dx, float dy, float dw, float dh) override;
    bool measure_image_from_file(const std::string& path,
                                  float& out_width, float& out_height) override;

    // ── Line dash / pixel manipulation (issue-916) ─────────────────────
    void set_line_dash(const float* intervals, int count, float phase) override;
    bool read_pixels(int x, int y, int width, int height, uint8_t* out) override;
    bool write_pixels(const uint8_t* data, int width, int height,
                      int dx, int dy) override;
    void draw_waveform(const float* samples, size_t count,
                       float x, float y, float width, float height,
                       const WaveformStyle& style) override;
    // Gradients
    void set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                   const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_radial(float cx, float cy, float radius,
                                   const Color* colors, const float* positions, int count) override;
    /// pulp #1524 — true two-circle radial gradient via SkGradientShader::MakeTwoPointConical.
    void set_fill_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                  const Color* colors, const float* positions, int count) override;
    void clear_fill_gradient() override;

    // pulp Wave 3 c2d.7 — Canvas2D `ctx.strokeStyle = createLinearGradient(...)`
    // (and radial / two-circle / conic counterparts). Routes through
    // SkGradientShader::Make* and stores the resulting shader on
    // `stroke_shader_`, which `apply_stroke_state` already attaches to
    // every stroke paint. Mirrors the fill-side surface so the bridge
    // can expose `canvasSetStrokeLinearGradient` etc. without inventing
    // a separate paint pipeline.
    void set_stroke_gradient_linear(float x0, float y0, float x1, float y1,
                                     const Color* colors, const float* positions, int count) override;
    void set_stroke_gradient_radial(float cx, float cy, float radius,
                                     const Color* colors, const float* positions, int count) override;
    void set_stroke_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) override;
    void set_stroke_gradient_conic(float cx, float cy, float start_angle,
                                    const Color* colors, const float* positions, int count) override;
    void clear_stroke_gradient() override;

    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createPattern.
    // Routes through SkShader::MakeImage with SkTileMode per axis.
    // Stored on the same gradient_shader_ field used by gradient fills
    // (the field already represents "active non-solid fill paint"), so
    // current_fill_paint() picks it up uniformly.
    void set_fill_pattern(const std::string& image_src,
                          PatternTileMode tile_x,
                          PatternTileMode tile_y) override;
    void set_stroke_pattern(const std::string& image_src,
                            PatternTileMode tile_x,
                            PatternTileMode tile_y) override;

    // Blend modes
    void set_blend_mode(BlendMode mode) override;

    // Path building
    void begin_path() override;
    void move_to(float x, float y) override;
    void line_to(float x, float y) override;
    void quad_to(float cpx, float cpy, float x, float y) override;
    void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) override;
    void close_path() override;
    void fill_current_path(FillRule rule = FillRule::nonzero) override;
    void stroke_current_path() override;

    // pulp #1521 — native arc subpaths via SkPath::arcTo / SkRRect.
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

    // SDF shapes
    void draw_sdf_shape(SDFShape shape, float x, float y, float w, float h,
                        const SDFStyle& style) override;
    void draw_blurred_backdrop(float x, float y, float w, float h,
                               float blur_radius, float corner_radius,
                               Color tint) override;
    void save_backdrop_filter(float x, float y, float w, float h,
                              float blur_radius) override;

    // Box shadow (issue-925) — uses SkImageFilters::DropShadowOnly for
    // outset shadows; inset shadows clip to the box and stroke with a
    // blurred mask.
    void draw_box_shadow(float x, float y, float w, float h,
                         float dx, float dy, float blur, float spread,
                         Color color, bool inset, float corner_radius) override;

    // Canvas2D drop-shadow state (issue-1434 batch 7). Sticky values that
    // attach an SkImageFilters::DropShadow to the active fill/stroke
    // paints until cleared (color alpha == 0 or blur == 0 with both
    // offsets == 0). See current_fill_paint() / current_stroke_paint()
    // for the wrapping logic.
    void set_shadow_color(Color color) override;
    void set_shadow_blur(float blur) override;
    void set_shadow_offset_x(float dx) override;
    void set_shadow_offset_y(float dy) override;

    // pulp #1520 — Canvas2D ctx.direction / ctx.filter sticky state.
    // Direction selects SkShaper's leftToRight flag (and, future-state,
    // the HarfBuzz buffer direction for proper bidi mixed-script). The
    // filter parses a CSS <filter-function-list> string ("blur(5px)
    // sepia(80%) ...") into an SkImageFilter chain that wraps each
    // subsequent paint via current_fill_paint() / apply_stroke_state.
    void set_direction(TextDirection direction) override;
    void set_filter(const std::string& filter) override;

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

    // Standalone text measurement (issue-916). Returns full HTML5
    // TextMetrics for `text` rendered with `family` at `size` pixels —
    // no canvas instance required. Used by the JS bridge's
    // canvasMeasureText so JS callers can pre-measure text for layout
    // without needing an active draw surface.
    static Canvas::TextMetrics measure_text_with_font(
        const std::string& family, float size, const std::string& text);

    // Opacity & compositing layers
    void set_opacity(float alpha) override;
    void save_layer(float x, float y, float w, float h,
                    float opacity, float blur_radius) override;
    // pulp #1549 — saveLayer with explicit blend mode (CSS / RN
    // mix-blend-mode). The Skia backend honors the requested blend
    // mode on the layer-paint so the subtree composites back with
    // multiply / screen / overlay / etc.
    void save_layer_with_blend(float x, float y, float w, float h,
                               float opacity, float blur_radius,
                               Canvas::BlendMode mode) override;
    // pulp #1434 Phase A2-4 — full CSS filter chain.
    void save_layer_with_filters(float x, float y, float w, float h,
                                  float opacity,
                                  const FilterChainEntry* chain,
                                  int count) override;

private:
    // Build the active fill paint, honoring `gradient_shader_` when set
    // so shape fills (rect / rrect / circle / arc / oval / polygon) render
    // gradients consistently with `fill_current_path()` (#1350).
    SkPaint current_fill_paint() const;

    // issue-1434 batch 7 — apply the sticky Canvas2D shadow* state to an
    // arbitrary paint (typically a stroke paint built via the free
    // `make_stroke_paint` helper). Members can't be reached from the
    // free helper, so the call sites that build a stroke paint apply
    // the shadow filter via this method before draw. No-op when shadow
    // is inactive (alpha == 0 OR (blur == 0 AND offsets == 0)).
    void apply_shadow_filter(SkPaint& paint) const;

    // Predicate used by `apply_shadow_filter` AND `current_fill_paint`
    // so the gating logic stays in one place.
    bool shadow_is_active() const;

    // pulp #1434 bridge-thin gap-fill — apply sticky stroke join + miter
    // limit to a freshly-constructed stroke paint. Called from the
    // stroke_* paths after make_stroke_paint() but before any paint
    // submit. Centralises the policy so future stroke-state setters
    // (lineCap and friends) can join here without touching every site.
    void apply_stroke_state(SkPaint& paint) const;

    // pulp #1434 bridge-thin gap-fill — translate the sticky
    // imageSmoothingEnabled / imageSmoothingQuality state into an
    // SkSamplingOptions for drawImageRect. Spec defaults match Skia
    // defaults so non-set callers keep getting kLinear.
    SkSamplingOptions sampling_options_for_image_smoothing() const;

    SkCanvas* canvas_;        // Non-owning — owned by surface or caller
    skgpu::graphite::Recorder* recorder_ = nullptr; // Non-owning — owned by SkiaSurface
    Color fill_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    Color stroke_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    float line_width_ = 1.0f;
    LineCap line_cap_ = LineCap::butt;
    LineJoin line_join_ = LineJoin::miter;
    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.miterLimit. Spec
    // default is 10 (matches Skia's SkPaint default).
    float miter_limit_ = 10.0f;
    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.imageSmoothingEnabled
    // / ctx.imageSmoothingQuality. Defaults match the spec (`true`,
    // `"low"`). Honored by draw_image_from_data / draw_image_from_file.
    bool image_smoothing_enabled_ = true;
    ImageSmoothingQuality image_smoothing_quality_ = ImageSmoothingQuality::low;
    std::string font_family_ = "sans-serif";
    int font_weight_ = 400;             ///< CSS weight 100..900 (pulp #927)
    int font_slant_ = 0;                ///< 0=upright, 1=italic (pulp #927)
    float letter_spacing_ = 0.0f;       ///< Extra advance per glyph in px (pulp #927)
    // pulp #1737 — OpenType feature flags (e.g. tnum / smcp / onum /
    // lnum / pnum) for CSS font-variant. Captured by set_font_features
    // / cleared by clear_font_features. Flushed into SkShaper's 8-arg
    // shape() overload at fill_text / stroke_text time. Empty vector
    // → SkShaper's default font features apply.
    std::vector<FontFeature> font_features_;
    // Internal helper — routes a shape() call through the 8-arg
    // overload + trivial RunIterators so font_features_ reaches
    // HarfBuzz. Defined in skia_canvas.cpp; declared here to keep
    // private. Forward-declared types (SkShaper, SkFont,
    // SkTextBlobBuilderRunHandler) come from existing #includes.
    void shape_with_features(class SkShaper& shaper,
                             const std::string& text,
                             const class SkFont& font,
                             bool ltr,
                             class SkTextBlobBuilderRunHandler* handler);
    TextAlign text_align_ = TextAlign::left;

    // Gradient state
    bool has_gradient_ = false;
    sk_sp<SkShader> gradient_shader_;

    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createPattern stroke
    // shader (rare: most plugins set patterns on fillStyle, not strokeStyle).
    // Lives separately from gradient_shader_ so stroke paths can opt in
    // via apply_stroke_state without disturbing fill state.
    sk_sp<SkShader> stroke_shader_;

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

    // Line-dash pattern (issue-916). Empty == solid stroke (no dashing).
    // Held as floats so the stroke-paint helper can rebuild the SkDashPathEffect
    // each time stroke_color/line_width change.
    std::vector<float> line_dash_;
    float line_dash_phase_ = 0.0f;

    // Canvas2D drop-shadow state (issue-1434 batch 7). Sticky — every
    // draw helper queries `make_shadow_filter()` and, when a shadow is
    // active, wraps its paint via `SkPaint::setImageFilter`. The shadow
    // is gated on color.a > 0 AND (blur > 0 OR dx != 0 OR dy != 0) to
    // match Canvas2D spec ("if all four shadow attributes are at their
    // defaults, no shadow is rendered"). Default = transparent black
    // shadow with zero blur + zero offset = inactive.
    Color shadow_color_ = Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
    float shadow_blur_     = 0.0f;
    float shadow_offset_x_ = 0.0f;
    float shadow_offset_y_ = 0.0f;

    // pulp #1520 — Canvas2D ctx.direction sticky state. Defaults to ltr
    // (matches the previous hard-coded SkShaper leftToRight=true). rtl
    // flips the shaper flag; inherit currently behaves as ltr until a
    // per-View writing-direction lookup lands (#1506).
    TextDirection direction_ = TextDirection::ltr;

    // pulp #1520 — Canvas2D ctx.filter sticky state. Stored as the raw
    // CSS string for round-trip diagnostics; the parsed SkImageFilter
    // chain caches alongside so we don't re-parse on every draw. Both
    // are reset together by set_filter(). When `filter_image_filter_`
    // is null we skip the wrap entirely (the "none" / no-op path).
    std::string filter_source_ = "none";
    sk_sp<SkImageFilter> filter_image_filter_;

    // pulp #1520 — wrap an arbitrary paint with the active ctx.filter
    // SkImageFilter chain (no-op when filter is "none" or unparsable).
    // Centralised so fill / stroke / text / image draws can opt in
    // without touching every call site.
    void apply_filter(SkPaint& paint) const;
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
