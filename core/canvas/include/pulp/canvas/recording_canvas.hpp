#pragma once

// RecordingCanvas: a Canvas that captures draw commands instead of rendering,
// for inspection and tests. canvas.hpp includes this header at its tail, so
// existing consumers keep compiling unchanged; new code should include this
// header directly.

#include <pulp/canvas/canvas.hpp>

namespace pulp::canvas {

// ── Recording canvas ─────────────────────────────────────────────────────────
// Captures draw commands for inspection/testing
// Does not actually render anything

struct DrawCommand {
    enum class Type {
        save, restore,
        translate, scale, rotate, clip_rect,
        set_transform, clip, set_blend_mode,
        concat_transform,
        set_fill_color, set_stroke_color, set_line_width,
        set_line_cap, set_line_join,
        fill_rect, stroke_rect, fill_rounded_rect, stroke_rounded_rect,
        fill_circle, stroke_circle, stroke_arc, stroke_line,
        set_font, set_text_align, fill_text,
        // Canvas2D `strokeText(text, x, y, maxWidth)` records
        // a distinct cmd so tests can assert that the bridge routed the
        // call through the dedicated stroke_text path (rather than the
        // fillText-with-stroke-color approximation). Layout
        // matches fill_text: text in `text`, (x,y) in f[0..1], optional
        // maxWidth in f[2] (<=0 = no limit).
        stroke_text,
        // Full font setter: family in `text`, size/weight/slant/
        // letter_spacing in f[0..3]. Emitted alongside (in addition to) the
        // legacy set_font command so existing tests that count set_font
        // continue to pass.
        set_font_full,
        // ── Canvas2D API coverage ────────────────────────────────────
        set_line_dash,      ///< intervals stored in `floats`, phase in f[0]
        draw_image,         ///< source path/url in `text`, dst rect in f[0..3]
        write_pixels,       ///< RGBA bytes in `text` (binary), w/h in f[0..1], dst in f[2..3]
        // ── setBoxShadow / draw_box_shadow ──────────────────────────
        draw_box_shadow,    ///< x/y/w/h in f[0..3], blur in f[4], spread/offsets via floats payload
        // ── Canvas2D shadow* state setters ──────────────────────────
        // Sticky state changes; the recording target captures one cmd
        // per setter so tests can assert on the bridge's flush order.
        set_shadow_color,    ///< color in `color`
        set_shadow_blur,     ///< blur (px) in f[0]
        set_shadow_offset_x, ///< dx (px) in f[0]
        set_shadow_offset_y, ///< dy (px) in f[0]
        // ── Canvas2D state setters ──────────────────────────────────
        // Sticky stroke / image state. Captured one cmd per setter so
        // the canvas2d bridge harness can assert flush order.
        set_miter_limit,     ///< limit in f[0]
        set_image_smoothing, ///< enabled in f[0] (0/1), quality in f[1] (0=low,1=med,2=high)
        // Canvas2D ctx.direction / ctx.filter sticky setters.
        // Direction enum (0=ltr, 1=rtl, 2=inherit) packed into f[0];
        // filter raw CSS <filter-function-list> string (e.g.
        // "blur(5px) sepia(80%)") in `text`. RecordingCanvas captures
        // each setter so tests can assert the JS shim flushed the
        // sticky state before the next text/image/fill draw.
        set_direction,
        set_filter,
        // Canvas2D ctx.createPattern.
        // image source path / data URI in `text`, tile modes packed into
        // f[0] (x) and f[1] (y) — 0 = repeat, 1 = no_repeat.
        set_fill_pattern,
        set_stroke_pattern,
        // Canvas2D `ctx.strokeStyle =
        // createLinearGradient(...) | createRadialGradient(...) |
        // createConicGradient(...)`. Mirrors the fill-side gradient cmds
        // so canvas2d harness tests can assert that the bridge plumbed
        // the stroke-gradient setter at the right point in the stream.
        // Layout matches the fill counterparts: linear (x0,y0)→(x1,y1)
        // packed as f[0..3]; radial (cx,cy,r) as (f[0],f[1],f[2]); conic
        // (cx,cy,startAngle) as (f[0],f[1],f[2]); two-circle (x0,y0,r0,
        // x1,y1,r1) as (f[0]..f[5]). Stops in `floats` interleaved as
        // [pos0, r0, g0, b0, a0, pos1, r1, g1, b1, a1, ...] — same shape
        // RecordingCanvas already uses for fill gradients.
        set_stroke_gradient_linear,
        set_stroke_gradient_radial,
        set_stroke_gradient_radial_two_circles,
        set_stroke_gradient_conic,
        clear_stroke_gradient,
        // ── FILL gradients ──────────────────────────────────────────
        // The stroke-side gradient commands above existed without these
        // fill-side counterparts, so RecordingCanvas silently dropped every
        // `set_fill_gradient_*` call: a headless test that set a fill
        // gradient and asserted on the result was really asserting on the
        // last solid `set_fill_color`, and would keep passing if the
        // gradient plumbing broke entirely. Layout mirrors the stroke
        // commands exactly — linear (x0,y0)→(x1,y1) in f[0..3]; radial
        // (cx,cy,r) in f[0..2]; conic (cx,cy,startAngle) in f[0..2];
        // two-circle (x0,y0,r0,x1,y1,r1) in f[0..5]; stops interleaved in
        // `floats` as [pos, r, g, b, a, ...].
        set_fill_gradient_linear,
        set_fill_gradient_radial,
        set_fill_gradient_radial_two_circles,
        set_fill_gradient_conic,
        clear_fill_gradient,
        // ── Retained Path draws ─────────────────────────────────────
        // The path's geometry is captured as an SVG `d` string in `text`
        // (so a test can assert on the exact geometry without walking a
        // command stream), and the fill rule / stroke style in `f`.
        fill_path_object,     ///< path `d` in `text`; fill rule in f[0] (0=nonzero, 1=evenodd)
        stroke_path_object,   ///< path `d` in `text`; width f[0], cap f[1], join f[2], miter f[3], dash phase f[4]; dash intervals in `floats`
        clip_path_object,     ///< path `d` in `text`; fill rule in f[0]
        draw_path_shadow,     ///< path `d` in `text`; dx/dy/blur/spread in f[0..3]; shadow color in `color`
        // ── Retained compositing LAYERS (handle-based) ──────────────
        // begin/end bracket the recording of a layer; draw_layer composites
        // a sealed one. The handle id is captured in f[0] so a test can
        // assert that the SAME layer was reused across frames rather than
        // silently re-recorded.
        begin_layer,          ///< bounds x/y/w/h in f[0..3]; cacheable in f[4]; id in f[5]
        end_layer,            ///< sealed handle id in f[0]
        draw_layer,           ///< id f[0], alpha f[1], blend mode f[2]
        draw_layer_fitted,    ///< id f[0]; dest x/y/w/h in f[1..4]
        draw_layer_rotated,   ///< id f[0]; angle (radians) in f[1]
        invalidate_layer,     ///< id f[0]
        // ── save_backdrop_filter for frosted-glass overlays ─────────
        save_backdrop_filter, ///< x/y/w/h in f[0..3], blur_radius in f[4]
        // ── CSS clip-path: path("...") ──────────────────────────────
        // Recording target captures the SVG-path-d string in `text` so
        // tests can assert that View::paint_all installs the clip
        // before painting children. Skia parses via SkParsePath /
        // SkPath::FromSVGString; backends without a path parser
        // degrade to a no-op (no clip applied).
        clip_path_svg,         ///< SVG path data in `text`
        // ── real clearRect that replaces pixels ─────────────────────
        clear_rect,          ///< clear rect, x/y/w/h in f[0..3]
        // ── Canvas2D path API recording ─────────────────────────────
        // Captured so widgets that emit path commands (SvgPathWidget,
        // CanvasWidget JS path-replays) can be asserted at the
        // command-stream level without a Skia raster surface.
        begin_path,           ///< no payload
        move_to,              ///< (x, y) in f[0..1]
        line_to,              ///< (x, y) in f[0..1]
        quad_to,              ///< (cpx, cpy, x, y) in f[0..3]
        cubic_to,             ///< (cp1x, cp1y, cp2x, cp2y, x, y) in f[0..5]
        close_path,           ///< no payload
        fill_current_path,    ///< no payload — uses last set_fill_color
        stroke_current_path,  ///< no payload — uses last set_stroke_color + set_line_width
        // ── native arc subpaths ─
        // Captured so widgets that emit native arc commands can be
        // asserted at the command-stream level without a Skia raster
        // surface. Maps 1:1 to the new Canvas::arc / arc_to / ellipse /
        // round_rect virtual methods.
        arc,                  ///< (cx, cy, r, start, end, anticlockwise as 0/1) in f[0..5]
        arc_to,               ///< (x1, y1, x2, y2, radius) in f[0..4]
        ellipse,              ///< (cx, cy, rx, ry, rotation, start) in f[0..5]; (end, anticlockwise as 0/1) extras tracked in `floats[0..1]`
        round_rect,           ///< (x, y, w, h, tl_x, tl_y) in f[0..5]; tr/br/bl x/y in `floats[0..5]`
        // ── Compositing layer saves (save_layer family) ─────────────
        // Before these existed, RecordingCanvas inherited the base
        // save_layer* methods, which all collapse to a bare save(). A
        // recording therefore could not distinguish an opacity / filter /
        // blend / mask compositing layer from a plain save() — which is
        // exactly why the effect facades (bloom/vignette/chromatic) went
        // undetected by command-stream tests. Each of these records the
        // layer intent, and its RecordingCanvas override still increments
        // the save stack by exactly one so save_count() balance holds.
        save_layer,           ///< bounds x/y/w/h in f[0..3]; opacity f[4]; blur f[5]
        save_layer_blend,     ///< bounds f[0..3]; opacity f[4]; blur f[5]; blend mode (int) in floats[0]
        save_layer_filters,   ///< bounds f[0..3]; effective opacity f[4]; summed blur f[5]; filter count in floats[0]
        save_layer_mask,      ///< bounds f[0..3]; opacity f[4]; mask-image CSS value in `text` (mask-size not captured)
        save_layer_bloom,     ///< bounds f[0..3]; intensity f[4]; threshold f[5]; blur radius in floats[0]
        // Custom SkSL draw recorded as intent. The base draw_with_sksl
        // fills a CPU placeholder rect and returns false; RecordingCanvas
        // instead captures the shader source + rect and returns false
        // (a recorder cannot compile/execute SkSL). Callers that test the
        // base CPU fallback must invoke `rc.Canvas::draw_with_sksl(...)`.
        draw_sksl             ///< rect x/y/w/h in f[0..3]; value f[4]; time f[5]; SkSL source in `text`
    };

    Type type;
    // Generic storage for command parameters
    float f[6] = {};
    Color color{};
    std::string text;
    // Optional variable-length payload — used by set_line_dash and
    // write_pixels (which store raw bytes packed as floats / characters
    // respectively). Kept off the default cmd to avoid bloating the
    // happy path.
    std::vector<float> floats;
};

class RecordingCanvas : public Canvas {
public:
    const std::vector<DrawCommand>& commands() const { return commands_; }
    void clear() { commands_.clear(); }
    size_t command_count() const { return commands_.size(); }

    // Count commands of a specific type
    size_t count(DrawCommand::Type type) const;

    // Number of times capture_paint_baseline_transform() was called — useful
    // for asserting CanvasWidget::paint() snapshots the inbound device matrix
    // exactly once at entry.
    size_t baseline_capture_count() const { return baseline_capture_count_; }

    void save() override;
    void restore() override;
    int save_count() const override { return save_depth_; }
    void restore_to_count(int target) override;
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;
    void set_transform(float a, float b, float c,
                       float d, float e, float f) override;
    void capture_paint_baseline_transform() override;
    void concat_transform(float a, float b, float c,
                          float d, float e, float f) override;
    AffineTransform2x3 current_transform() const override;
    void clip_rect(float x, float y, float w, float h) override;
    void clip(FillRule rule = FillRule::nonzero) override;
    void clip_path_svg(const std::string& svg_path_d) override;
    void set_blend_mode(BlendMode mode) override;
    void set_fill_color(Color c) override;
    void set_stroke_color(Color c) override;
    void set_line_width(float w) override;
    void set_line_cap(LineCap cap) override;
    void set_line_join(LineJoin join) override;
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
    void set_font(const std::string& family, float size) override;
    void set_font_full(const std::string& family, float size,
                       int weight, int slant, float letter_spacing) override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    // Canvas2D fillText(text,x,y,maxWidth) + strokeText.
    // The recording target captures `max_width` in `f[2]` so harness
    // tests can assert the bridge plumbed the optional fourth arg
    // through. `<=0` is the spec sentinel for "no constraint".
    void fill_text_with_max_width(const std::string& text,
                                  float x, float y, float max_width) override;
    void stroke_text(const std::string& text, float x, float y,
                     float max_width = 0.0f) override;
    float measure_text(const std::string& text) override;

    // Capture Canvas2D API commands so JS-driven tests can assert
    // on them via DrawCommand::Type::set_line_dash / draw_image /
    // write_pixels. Source path is stored in DrawCommand::text.
    void set_line_dash(const float* intervals, int count, float phase) override;
    // Capture the opaque-handle image draw so the image transform / fit / tile
    // helpers are assertable headlessly. Records a `draw_image` command with
    // the destination rect in f[0..3] and an EMPTY `text` (file / data draws
    // put the source in `text`, so empty text distinguishes a handle draw).
    void draw_image(void* native_handle,
                    float x, float y, float w, float h) override;
    bool draw_image_from_data(const uint8_t* data, size_t size,
                              float x, float y, float w, float h) override;
    bool draw_image_from_file(const std::string& path,
                              float x, float y, float w, float h) override;
    // Record the source-rect overload so widget-bridge tests
    // can assert that the JS shim plumbed sx/sy/sw/sh through the bridge.
    // Source rect is stored in `floats[0..3]` (sx, sy, sw, sh); dst rect
    // stays in f[0..3] for backward-compat with the 5-arg test path.
    bool draw_image_from_file_rect(const std::string& path,
                                    float sx, float sy, float sw, float sh,
                                    float dx, float dy, float dw, float dh) override;
    bool draw_image_from_data_rect(const uint8_t* data, size_t size,
                                    float sx, float sy, float sw, float sh,
                                    float dx, float dy, float dw, float dh) override;
    bool write_pixels(const uint8_t* data, int width, int height,
                      int dx, int dy) override;
    void save_backdrop_filter(float x, float y, float w, float h,
                              float blur_radius) override;

    // ── Compositing layer saves (save_layer family) ──────────────────────
    // Record the layer intent as a distinct command (see the DrawCommand::Type
    // comments) instead of the base-class collapse to a bare save(). Each
    // increments the save stack by exactly one, so a save_layer / restore pair
    // stays balanced against save_count() just like the base backends.
    void save_layer(float x, float y, float w, float h,
                    float opacity = 1.0f, float blur_radius = 0.0f) override;
    void save_layer_with_blend(float x, float y, float w, float h,
                               float opacity, float blur_radius,
                               BlendMode mode) override;
    void save_layer_with_filters(float x, float y, float w, float h,
                                 float opacity,
                                 const FilterChainEntry* chain,
                                 int count) override;
    void save_layer_with_mask(float x, float y, float w, float h,
                              float opacity,
                              const std::string& mask_image,
                              const std::string& mask_size) override;
    void save_layer_with_bloom(float x, float y, float w, float h,
                               float intensity, float threshold,
                               float radius) override;

    // Record a custom SkSL draw as intent and return true — the recorder
    // accepted and captured the draw (the success path). This lets a headless
    // test assert that an effect / widget routed a shader draw through the
    // canvas without a GPU, and keeps shader widgets on the shader path rather
    // than recording their C++ fallback body (see draw_custom_shader_body).
    bool draw_with_sksl(const std::string& sksl, float x, float y,
                        float w, float h,
                        const ShaderUniforms& uniforms) override;

    // Capture a single box-shadow command so JS-driven tests
    // can assert on inset / color / offsets without having to walk the
    // CPU-fallback rectangle stack.
    void draw_box_shadow(float x, float y, float w, float h,
                         float dx, float dy, float blur, float spread,
                         Color color, bool inset, float corner_radius) override;

    // ── FILL gradients ───────────────────────────────────────────────────
    // Previously unrecorded: the base-class no-ops swallowed these, so a
    // headless fill-gradient test was really asserting on the last solid
    // fill color. See the DrawCommand::Type comment.
    void set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                  const Color* colors, const float* positions,
                                  int count) override;
    void set_fill_gradient_radial(float cx, float cy, float radius,
                                  const Color* colors, const float* positions,
                                  int count) override;
    void set_fill_gradient_radial_two_circles(
        float x0, float y0, float r0, float x1, float y1, float r1,
        const Color* colors, const float* positions, int count) override;
    void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                 const Color* colors, const float* positions,
                                 int count) override;
    void clear_fill_gradient() override;

    // ── Retained paths ───────────────────────────────────────────────────
    // `using` re-exposes the base-class polygon overloads: declaring
    // fill_path(const Path&) here would otherwise HIDE
    // fill_path(const Point2D*, size_t) for anyone holding a RecordingCanvas&.
    using Canvas::fill_path;
    using Canvas::stroke_path;
    void fill_path(const Path& path, FillRule rule = FillRule::nonzero) override;
    void stroke_path(const Path& path, const StrokeStyle& style) override;
    void clip_path(const Path& path, FillRule rule = FillRule::nonzero) override;
    void draw_path_shadow(const Path& path, float dx, float dy,
                          float blur, float spread, Color color) override;

    // ── Retained layers ──────────────────────────────────────────────────
    // The recording target models layer lifetime for real (it hands out ids,
    // tracks which are sealed, and honours invalidate), so a test can assert
    // that a cacheable layer SURVIVES ACROSS FRAMES without needing a GPU.
    LayerHandle begin_layer(Rect2D bounds, bool cacheable = false) override;
    LayerHandle end_layer() override;
    void draw_layer(LayerHandle layer, float alpha = 1.0f,
                    BlendMode mode = BlendMode::normal) override;
    void draw_layer_fitted(LayerHandle layer, Rect2D dest) override;
    void draw_layer_rotated(LayerHandle layer, float angle_rad) override;
    bool layer_valid(LayerHandle layer) const override;
    void invalidate_layer(LayerHandle layer) override;

    // Capture sticky Canvas2D shadow state setters
    // so tests can assert that JS `ctx.shadowColor = ...; ctx.shadowBlur =
    // ...; ctx.fillRect(...)` flushes the shadow state through to the
    // canvas before the geometry is recorded.
    void set_shadow_color(Color color) override;
    void set_shadow_blur(float blur) override;
    void set_shadow_offset_x(float dx) override;
    void set_shadow_offset_y(float dy) override;

    // Capture sticky stroke / image
    // state so tests can assert that JS `ctx.miterLimit = ...` and
    // `ctx.imageSmoothingEnabled = ...` flush through to the canvas
    // before the next geometry is recorded.
    void set_miter_limit(float limit) override;
    void set_image_smoothing(bool enabled,
                             ImageSmoothingQuality quality) override;

    // Canvas2D ctx.direction / ctx.filter capture.
    void set_direction(TextDirection direction) override;
    void set_filter(const std::string& filter) override;

    // Capture pattern setter intents
    // so canvas2d harness tests can assert flush order without needing
    // a real raster surface or decoded image.
    void set_fill_pattern(const std::string& image_src,
                          PatternTileMode tile_x,
                          PatternTileMode tile_y) override;
    void set_stroke_pattern(const std::string& image_src,
                            PatternTileMode tile_x,
                            PatternTileMode tile_y) override;

    // Capture stroke-gradient setter intents so the
    // canvas2d harness can assert the bridge plumbed `ctx.strokeStyle =
    // createLinearGradient(...)` (etc.) end-to-end. Stops are flattened
    // into `floats` as [pos0, r0, g0, b0, a0, pos1, r1, g1, b1, a1, ...]
    // — same shape as set_line_dash but per-stop instead of per-interval.
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

    // Canvas2D path API recording. Each call appends one
    // DrawCommand so widget tests can assert on emit order and shape
    // without needing a real raster surface. Pure capture; no geometry
    // is computed.
    void begin_path() override;
    void move_to(float x, float y) override;
    void line_to(float x, float y) override;
    void quad_to(float cpx, float cpy, float x, float y) override;
    void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y,
                  float x, float y) override;
    void close_path() override;
    void fill_current_path(FillRule rule = FillRule::nonzero) override;
    void stroke_current_path() override;

    // Native arc subpaths (recorded as DrawCommands so
    // widget tests can assert emit order without a raster surface).
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

private:
    std::vector<DrawCommand> commands_;
    size_t baseline_capture_count_ = 0;
    // Track save/restore depth so RecordingCanvas can model
    // the same save_count() / restore_to_count() contract as the live
    // backends. This lets CanvasWidget::paint() unit tests assert that the
    // outer save/restore wrapper drops any leftover saves emitted by an
    // unbalanced JS draw script.
    int save_depth_ = 0;
    // Track the current device matrix so
    // current_transform() returns a faithful CTM in unit tests. The matrix
    // is saved/restored alongside save_depth_, and translate/scale/rotate/
    // set_transform/concat_transform mutate it. Layout in column-major
    // CanvasRenderingContext2D order: [a, b, c, d, e, f].
    AffineTransform2x3 ctm_{};
    std::vector<AffineTransform2x3> ctm_stack_;

    // ── Layer bookkeeping ────────────────────────────────────────────────
    // Modeled faithfully (not stubbed) so that "does this layer survive to
    // the next frame?" is testable headlessly. A cacheable layer stays valid
    // until it is explicitly invalidated; a non-cacheable one is dropped as
    // soon as it is drawn, which is exactly the contract the GPU backends
    // implement with real textures.
    struct LayerRecord {
        Rect2D bounds;
        bool cacheable = false;
        bool sealed = false;
    };
    std::vector<std::pair<uint64_t, LayerRecord>> layers_;
    std::vector<uint64_t> open_layers_;   ///< begin_layer stack
    uint64_t next_layer_id_ = 1;

    LayerRecord* find_layer(uint64_t id);
    const LayerRecord* find_layer(uint64_t id) const;
};

} // namespace pulp::canvas
