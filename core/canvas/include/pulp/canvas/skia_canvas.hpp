#pragma once

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>

#ifdef PULP_HAS_SKIA

// Forward declare Skia types to avoid header dependency in public API
class SkCanvas;
class SkSurface;
class SkFont;
class SkImage;
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
class GrDirectContext;

// Which Skia GPU backend this build links. Native builds link Graphite on
// Dawn; the Emscripten slice links Ganesh on WebGL2, which ships no Graphite
// or Dawn symbols at all. The two are mutually exclusive, and neither is
// defined for a CPU-raster-only build. These macros gate implementation code
// only — the SkiaCanvas layout below is identical on every backend so a TU
// that misses the Skia compile definitions can still consume this header.
#if defined(SK_GRAPHITE)
#define PULP_CANVAS_GRAPHITE 1
#else
#define PULP_CANVAS_GRAPHITE 0
#endif
#if defined(SK_GANESH)
#define PULP_CANVAS_GANESH 1
#else
#define PULP_CANVAS_GANESH 0
#endif

namespace pulp::canvas {

// Skia-backed Canvas implementation for GPU-accelerated rendering
// Uses Skia Graphite when available, falls back to Ganesh or CPU
class SkiaCanvas : public Canvas {
public:
    // Create wrapping an existing SkCanvas (e.g., from a surface)
    explicit SkiaCanvas(SkCanvas* canvas, skgpu::graphite::Recorder* recorder = nullptr);
    ~SkiaCanvas() override;

    // Attach the Ganesh direct context that owns the surface this canvas draws
    // into. Raster-decoded images must be uploaded to a GPU texture before a
    // GPU-backed canvas can draw them — Graphite canvases do that through the
    // recorder passed to the constructor, Ganesh canvases through this context.
    // Leaving it null keeps the CPU-raster behavior (the decoded image is
    // drawn as-is), which is correct for raster surfaces and degraded — draws
    // are dropped by the backend — for a GPU surface.
    void set_gpu_upload_context(GrDirectContext* context);

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

    // Canvas2D ctx.miterLimit and ctx.imageSmoothingEnabled /
    // ctx.imageSmoothingQuality. Sticky paint state honored by subsequent
    // stroke / drawImage calls.
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

    // ── Polyline / polygon ───────────────────────────────────────────────
    // Build one SkPath and paint it once. The base-class stroke_path fallback
    // degrades to N independent stroke_line calls (each with its own caps and
    // no joins, which beads a dense curve), and the base fill_path round-trips
    // through the path-recording state. Overriding both keeps the Skia backend
    // at parity with CgCanvas. fill_path maps `rule` onto the SkPath fill type
    // (kEvenOdd / kWinding).
    void stroke_path(const Point2D* points, size_t count) override;
    void fill_path(const Point2D* points, size_t count,
                   FillRule rule = FillRule::nonzero) override;

    // ── Retained paths (pulp::canvas::Path) ──────────────────────────────
    // Implemented in skia_canvas_path.cpp. Each converts the Path to one
    // SkPath and issues a single draw, rather than replaying verbs through the
    // scratch path builder. The `using` declarations keep the polygon
    // overloads above visible — a same-named override would otherwise hide
    // them from anyone holding a SkiaCanvas&.
    using Canvas::fill_path;
    using Canvas::stroke_path;
    void fill_path(const Path& path, FillRule rule = FillRule::nonzero) override;
    void stroke_path(const Path& path, const StrokeStyle& style) override;
    void clip_path(const Path& path, FillRule rule = FillRule::nonzero) override;
    void draw_path_shadow(const Path& path, float dx, float dy,
                          float blur, float spread, Color color) override;

    // ── Retained compositing layers ──────────────────────────────────────
    // Real offscreen SkSurfaces, sealed to SkImages that OUTLIVE the frame
    // when `cacheable` — which is the whole point of the handle API.
    LayerHandle begin_layer(Rect2D bounds, bool cacheable = false) override;
    LayerHandle end_layer() override;
    void draw_layer(LayerHandle layer, float alpha = 1.0f,
                    BlendMode mode = BlendMode::normal) override;
    void draw_layer_fitted(LayerHandle layer, Rect2D dest) override;
    void draw_layer_rotated(LayerHandle layer, float angle_rad) override;
    bool layer_valid(LayerHandle layer) const override;
    void invalidate_layer(LayerHandle layer) override;

    // ── Text ─────────────────────────────────────────────────────────────
    void set_font(const std::string& family, float size) override;
    void set_font_full(const std::string& family, float size,
                       int weight, int slant, float letter_spacing) override;
    // OpenType feature flags via SkShaper Feature API.
    // Captured into font_features_ vector and flushed at shape time.
    void set_font_features(std::vector<FontFeature> features) override;
    void clear_font_features() override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    // Typed text-anchor variant.
    void fill_text_anchored(const std::string& text, float x, float y,
                            TextAnchor anchor) override;
    // Canvas2D fillText(text,x,y,maxWidth) + strokeText(text,x,y,maxWidth).
    void fill_text_with_max_width(const std::string& text,
                                  float x, float y, float max_width) override;
    void stroke_text(const std::string& text, float x, float y,
                     float max_width = 0.0f) override;
    void fill_text_sdf(const std::string& text, float x, float y,
                       const SdfAtlas& atlas) override;
    float measure_text(const std::string& text) override;
    TextMetrics measure_text_full(const std::string& text) override;
    // Caret x for a byte boundary within the full shaped run (same
    // make_paragraph(...) fill_text uses), so kerned / letter-spaced text
    // reports the exact painted advance.
    float text_x_for_byte(const std::string& text,
                          std::size_t byte_index) override;

    // ── Images ──────────────────────────────────────────────────────────
    bool draw_image_from_data(const uint8_t* data, size_t size,
                              float x, float y, float w, float h) override;
    bool draw_image_from_file(const std::string& path,
                               float x, float y, float w, float h) override;
    bool draw_svg(const std::string& svg_document,
                  float x, float y, float w, float h) override;
    // 9-arg drawImage source-rect form: routes through
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

    // Skia-specific: draw an already-constructed SkImage (e.g. a GPU
    // surface snapshot or atlas texture) into the active rect. The
    // decode-from-bytes paths above always yield raster images; this
    // overload is the seam for drawing a GPU-texture-backed image, which
    // is the case the `.skp` capture's image proc must handle. Returns
    // false when the canvas or image is null.
    bool draw_skia_image(const sk_sp<SkImage>& image,
                         float x, float y, float w, float h);

    // ── Line dash / pixel manipulation ─────────────────────────────────
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
    /// True two-circle radial gradient via SkShaders::TwoPointConicalGradient.
    void set_fill_gradient_radial_two_circles(
        float x0, float y0, float r0,
        float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                  const Color* colors, const float* positions, int count) override;
    void clear_fill_gradient() override;

    // Canvas2D `ctx.strokeStyle = createLinearGradient(...)` (and radial /
    // two-circle / conic counterparts). Routes through SkShaders::*Gradient and
    // stores the resulting shader on `stroke_shader_`, which
    // `apply_stroke_state` attaches to every stroke paint. Mirrors the fill-side
    // surface so the bridge can expose `canvasSetStrokeLinearGradient` etc.
    // without inventing a separate paint pipeline.
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

    // Canvas2D ctx.createPattern.
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

    // Native arc subpaths via SkPath::arcTo / SkRRect.
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

    // Box shadow uses SkImageFilters::DropShadowOnly for outset shadows; inset
    // shadows clip to the box and stroke with a blurred mask.
    void draw_box_shadow(float x, float y, float w, float h,
                         float dx, float dy, float blur, float spread,
                         Color color, bool inset, float corner_radius) override;

    // Canvas2D drop-shadow state. Sticky values attach an
    // SkImageFilters::DropShadow to the active fill/stroke paints until cleared
    // (color alpha == 0 or blur == 0 with both offsets == 0). See
    // current_fill_paint() and apply_shadow_filter() for the wrapping logic.
    void set_shadow_color(Color color) override;
    void set_shadow_blur(float blur) override;
    void set_shadow_offset_x(float dx) override;
    void set_shadow_offset_y(float dy) override;

    // Canvas2D ctx.direction / ctx.filter sticky state.
    // Direction selects SkShaper's leftToRight flag (and later the HarfBuzz
    // buffer direction for proper bidi mixed-script). The
    // filter parses a CSS <filter-function-list> string ("blur(5px)
    // sepia(80%) ...") into an SkImageFilter chain that wraps each
    // subsequent paint via current_fill_paint() / apply_stroke_state.
    void set_direction(TextDirection direction) override;
    void set_filter(const std::string& filter) override;

    // Custom SkSL shader rendering (GPU-accelerated)
    bool draw_with_sksl(const std::string& sksl,
                        float x, float y, float w, float h,
                        const ShaderUniforms& uniforms) override;

    // Child-shader compositor: post-process an already-painted layer with a
    // custom SkSL shader (declares `uniform shader content`).
    bool save_layer_with_sksl_post_effect(float x, float y, float w, float h,
                                          const std::string& sksl,
                                          const ShaderUniforms& uniforms,
                                          float sample_radius = 0.0f) override;

    // Draw a Dawn-backed texture into the current Skia canvas when Graphite is active.
    bool draw_native_dawn_texture(void* texture_handle,
                                  uint32_t width,
                                  uint32_t height,
                                  const std::string& format,
                                  float x,
                                  float y,
                                  float w,
                                  float h);

    // Standalone text measurement. Returns full HTML5
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
    // saveLayer with explicit blend mode (CSS / RN mix-blend-mode). The Skia
    // backend honors the requested blend mode on the layer-paint so the subtree
    // composites back with multiply / screen / overlay / etc.
    void save_layer_with_blend(float x, float y, float w, float h,
                               float opacity, float blur_radius,
                               Canvas::BlendMode mode) override;
    // Full CSS filter chain.
    void save_layer_with_filters(float x, float y, float w, float h,
                                  float opacity,
                                  const FilterChainEntry* chain,
                                  int count) override;

    // CSS mask-image + mask-size paint composite.
    // SkiaCanvas implements the 2-saveLayer pattern with kDstIn:
    //   1. open the content layer (saveLayer with opacity)
    //   2. caller paints the subtree
    //   3. caller calls restore() — SkiaCanvas::restore() detects this
    //      layer has a pending mask, draws the mask shader via a kDstIn
    //      saveLayer first, then closes the content layer
    // Pending masks are tracked on `pending_masks_` keyed by Skia's
    // internal save count so the restore-side dispatcher knows which
    // restore call belongs to which mask layer. SkiaCanvas::restore() handles
    // the deferred mask composite: it walks pending_masks_ for a matching save
    // count before delegating to canvas_->restore().
    void save_layer_with_mask(float x, float y, float w, float h,
                               float opacity,
                               const std::string& mask_image,
                               const std::string& mask_size) override;
    void save_layer_with_bloom(float x, float y, float w, float h,
                               float intensity, float threshold,
                               float radius) override;

private:
    // Shared saveLayer for every compositing-layer entry point (opacity, blur,
    // blend, bloom, CSS filter chain, SkSL post-effect). Builds the layer paint
    // from the given properties — `opacity < 1` sets the layer alpha; a non-null
    // `image_filter` is used as the layer's image filter, else `blur_radius > 0`
    // adds a Gaussian blur; `mode != normal` sets the composite blend — pushes
    // the layer, and tracks it on the non-opaque-layer stack when the layer is
    // not fully opaque (opacity < 1 OR `force_non_opaque`, e.g. a filter chain
    // that reduces coverage). Consolidates what were four near-identical bodies.
    void push_layer(float x, float y, float w, float h,
                    float opacity, float blur_radius, Canvas::BlendMode mode,
                    sk_sp<SkImageFilter> image_filter,
                    bool force_non_opaque = false);

    // Build the active fill paint, honoring `gradient_shader_` when set
    // so shape fills (rect / rrect / circle / arc / oval / polygon) render
    // gradients consistently with `fill_current_path()`.
    SkPaint current_fill_paint() const;

    // Apply the sticky Canvas2D shadow* state to an
    // arbitrary paint (typically a stroke paint built via the free
    // `make_stroke_paint` helper). Members can't be reached from the
    // free helper, so the call sites that build a stroke paint apply
    // the shadow filter via this method before draw. No-op when shadow
    // is inactive (alpha == 0 OR (blur == 0 AND offsets == 0)).
    void apply_shadow_filter(SkPaint& paint) const;

    // Predicate used by `apply_shadow_filter` AND `current_fill_paint`
    // so the gating logic stays in one place.
    bool shadow_is_active() const;

    // Apply sticky stroke join + miter limit to a freshly-constructed stroke
    // paint. Called from the stroke_* paths after make_stroke_paint() but
    // before any paint submit. Centralises the policy so future stroke-state
    // setters can join here without touching every site.
    void apply_stroke_state(SkPaint& paint) const;

    // Translate the sticky imageSmoothingEnabled / imageSmoothingQuality state
    // into an SkSamplingOptions for drawImageRect. Spec defaults match Skia
    // defaults so non-set callers keep getting kLinear.
    SkSamplingOptions sampling_options_for_image_smoothing() const;

    // Upload a raster-decoded SkImage to a GPU texture when a Graphite
    // recorder or a Ganesh context is attached. Returns the input image
    // unchanged on the CPU raster path or if the upload fails. Centralised so
    // every draw_image_* method takes the same branch.
    sk_sp<SkImage> ensure_gpu_image(sk_sp<SkImage> image) const;

    SkCanvas* canvas_;        // Non-owning — owned by surface or caller
    skgpu::graphite::Recorder* recorder_ = nullptr; // Non-owning — owned by SkiaSurface
    GrDirectContext* gr_context_ = nullptr;         // Non-owning — owned by the GL host
    Color fill_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    Color stroke_color_ = Color::rgba(1.0f, 1.0f, 1.0f);
    float line_width_ = 1.0f;
    LineCap line_cap_ = LineCap::butt;
    LineJoin line_join_ = LineJoin::miter;
    // Canvas2D ctx.miterLimit. Spec default is 10 (matches Skia's SkPaint
    // default).
    float miter_limit_ = 10.0f;
    // Canvas2D ctx.imageSmoothingEnabled / ctx.imageSmoothingQuality. Defaults
    // match the spec (`true`, `"low"`). Honored by draw_image_from_data /
    // draw_image_from_file.
    bool image_smoothing_enabled_ = true;
    ImageSmoothingQuality image_smoothing_quality_ = ImageSmoothingQuality::low;
    std::string font_family_ = "sans-serif";
    int font_weight_ = 400;             ///< CSS weight 100..900
    int font_slant_ = 0;                ///< 0=upright, 1=italic
    float letter_spacing_ = 0.0f;       ///< Extra advance per glyph in px
    // OpenType feature flags (e.g. tnum / smcp / onum /
    // lnum / pnum) for CSS font-variant. Captured by set_font_features
    // / cleared by clear_font_features. Applied via
    // `TextStyle::addFontFeature` in `make_paragraph()` at
    // fill_text / stroke_text / measure_text time. Empty vector → no
    // OpenType features set.
    std::vector<FontFeature> font_features_;

    // Pending mask state for save_layer_with_mask.
    // Each entry is the deferred mask composite to apply when the
    // matching restore() fires. Tracked by Skia's getSaveCount() so
    // restore() can look up "is the layer I'm about to close a
    // mask layer". sk_sp<SkShader> may be null when the mask string
    // didn't parse — restore() then just closes the layer plainly.
    struct PendingMask {
        sk_sp<class SkShader> shader;  // forward-declared at top of file
        struct { float left, top, right, bottom; } bounds;
        int save_count_after_open;     // canvas_->getSaveCount() AFTER saveLayer
    };
    std::vector<PendingMask> pending_masks_;

public:
    // Returns true when at least one currently-open save_layer* has alpha < 1
    // on its layer-paint. Text paint paths (fill_text, stroke_text) consult this
    // at paint time and select greyscale AA over LCD subpixel AA: Skia's LCD
    // subpixel patterns can't antialias correctly into a partially transparent
    // pixel, so glyphs render faint inside CSS-opacity layers without this flip.
    // Browsers (Blink / WebKit) do the same. Exposed publicly so tests can
    // assert the stack-tracking around nested save_layer / restore /
    // restore_to_count.
    bool inside_non_opaque_layer() const {
        return !non_opaque_layer_stack_.empty();
    }

private:
    // Each entry is Skia's getSaveCount() after the layer was opened.
    // restore() pops the top entry if its save count matches the canvas's
    // current save count; restore_to_count() pops any entries strictly above
    // the target (mirrors SkCanvas::restoreToCount). Plain save() / save_layer
    // with opacity == 1 do not push.
    std::vector<int> non_opaque_layer_stack_;

    TextAlign text_align_ = TextAlign::left;

    // Gradient state
    bool has_gradient_ = false;
    sk_sp<SkShader> gradient_shader_;

    // Canvas2D ctx.createPattern stroke shader. Lives separately from
    // gradient_shader_ so stroke paths can opt in via apply_stroke_state without
    // disturbing fill state.
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

    // Line-dash pattern. Empty == solid stroke (no dashing).
    // Held as floats so the stroke-paint helper can rebuild the SkDashPathEffect
    // each time stroke_color/line_width change.
    std::vector<float> line_dash_;
    float line_dash_phase_ = 0.0f;

    // Canvas2D drop-shadow state. Sticky: every
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

    // Canvas2D ctx.direction sticky state. Defaults to ltr, rtl flips the
    // shaper flag, and inherit currently behaves as ltr until a per-View
    // writing-direction lookup lands.
    TextDirection direction_ = TextDirection::ltr;

    // Canvas2D ctx.filter sticky state. Stored as the raw
    // CSS string for round-trip diagnostics; the parsed SkImageFilter
    // chain caches alongside so we don't re-parse on every draw. Both
    // are reset together by set_filter(). When `filter_image_filter_`
    // is null we skip the wrap entirely (the "none" / no-op path).
    std::string filter_source_ = "none";
    sk_sp<SkImageFilter> filter_image_filter_;

    // ── Retained layer state (skia_canvas_path.cpp) ──────────────────────
    // A sealed layer is an SkImage that survives past end_layer(). Cacheable
    // layers stay in `layers_` until explicitly invalidated — across frames,
    // across paints, until the owner says otherwise. Non-cacheable ones are
    // dropped the first time they are drawn.
    struct SkiaLayer {
        sk_sp<SkImage> image;      ///< null until end_layer() seals it
        sk_sp<SkSurface> surface;  ///< live only while recording
        Rect2D bounds;               ///< in the user space of begin_layer()
        float scale_x = 1.0f;      ///< device pixels per user unit at record time
        float scale_y = 1.0f;
        bool cacheable = false;
    };
    std::vector<std::pair<uint64_t, SkiaLayer>> layers_;

    /// Canvases we redirected away from, innermost last. `canvas_` points at
    /// the layer surface while recording; these are what we put back.
    struct OpenLayer {
        uint64_t id = 0;
        SkCanvas* previous_canvas = nullptr;
    };
    std::vector<OpenLayer> open_layers_;
    uint64_t next_layer_id_ = 1;

    SkiaLayer* find_layer(uint64_t id);
    const SkiaLayer* find_layer(uint64_t id) const;
    /// Shared by draw_layer / draw_layer_fitted / draw_layer_rotated.
    void draw_layer_common(LayerHandle layer, const SkMatrix& placement,
                           float alpha, BlendMode mode);

public:
    // Wrap an arbitrary paint with the active ctx.filter
    // SkImageFilter chain (no-op when filter is "none" or unparsable).
    // Centralised so fill / stroke / text / image draws can opt in
    // without touching every call site.
    // Public for test_canvas.cpp filter-pipeline tests; the implementation is a
    // pure paint-state mutation, no class invariant gets violated by external
    // callers.
    void apply_filter(SkPaint& paint) const;
};

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
