#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <variant>
#include <functional>

namespace pulp::canvas {

// ── Color ────────────────────────────────────────────────────────────────────

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    /// Construct from float channels [0,1] (>1.0 allowed for HDR)
    static constexpr Color rgba(float r, float g, float b, float a = 1.0f) {
        return {r, g, b, a};
    }

    /// Construct from 8-bit channels [0,255] — convenience for legacy/hex code
    static constexpr Color rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }

    /// Construct from hex 0xRRGGBB (alpha = 1.0)
    static Color hex(uint32_t rgb) {
        return rgba8(static_cast<uint8_t>((rgb >> 16) & 0xFF),
                     static_cast<uint8_t>((rgb >> 8) & 0xFF),
                     static_cast<uint8_t>(rgb & 0xFF));
    }

    // ── Color space conversions ─────────────────────────────────────

    struct HSV { float h = 0, s = 0, v = 0; };
    struct HSL { float h = 0, s = 0, l = 0; };
    struct OKLCH { float L = 0, C = 0, h = 0; };

    /// Convert to HSV (h in [0,360), s and v in [0,1])
    HSV to_hsv() const;
    static Color from_hsv(HSV hsv, float alpha = 1.0f);

    /// Convert to HSL (h in [0,360), s and l in [0,1])
    HSL to_hsl() const;
    static Color from_hsl(HSL hsl, float alpha = 1.0f);

    /// Convert to OKLCH (perceptually uniform, CSS Color Level 4)
    /// L in [0,1], C in [0,~0.4], h in [0,360)
    OKLCH to_oklch() const;
    static Color from_oklch(OKLCH oklch, float alpha = 1.0f);

    /// Serialize to compact binary (4x float LE, 16 bytes)
    void encode(uint8_t* out) const;
    static Color decode(const uint8_t* data);

    // ── Color math ──────────────────────────────────────────────────

    /// Interpolate between this color and other by factor t [0,1]
    Color interpolate(const Color& other, float t) const {
        return {r + (other.r - r) * t,
                g + (other.g - g) * t,
                b + (other.b - b) * t,
                a + (other.a - a) * t};
    }

    /// Scale color intensity for HDR (channels can exceed 1.0)
    Color with_hdr_intensity(float multiplier) const {
        return {r * multiplier, g * multiplier, b * multiplier, a};
    }

    /// Return copy with different alpha
    Color with_alpha(float alpha) const {
        return {r, g, b, alpha};
    }

    /// Convert to 8-bit values for serialization/interop
    uint8_t r8() const { return static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f); }
    uint8_t g8() const { return static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f); }
    uint8_t b8() const { return static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f); }
    uint8_t a8() const { return static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f); }

    /// Pack to uint32_t ARGB (clamped to [0,255]) for Skia/platform interop
    uint32_t to_argb32() const {
        return (static_cast<uint32_t>(a8()) << 24) |
               (static_cast<uint32_t>(r8()) << 16) |
               (static_cast<uint32_t>(g8()) << 8) |
               static_cast<uint32_t>(b8());
    }

    /// Unpack from uint32_t ARGB
    static Color from_argb32(uint32_t argb) {
        return rgba8(static_cast<uint8_t>((argb >> 16) & 0xFF),
                     static_cast<uint8_t>((argb >> 8) & 0xFF),
                     static_cast<uint8_t>(argb & 0xFF),
                     static_cast<uint8_t>((argb >> 24) & 0xFF));
    }

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const Color& other) const { return !(*this == other); }
};

// ── Paint ────────────────────────────────────────────────────────────────────

struct GradientStop {
    float position; // 0.0 to 1.0
    Color color;
};

struct LinearGradient {
    float x0, y0, x1, y1;
    std::vector<GradientStop> stops;
};

struct RadialGradient {
    float cx, cy, radius;
    float focal_x = 0, focal_y = 0;  ///< Focal point offset for spotlight effects
    std::vector<GradientStop> stops;
};

/// Conic (sweep) gradient — colors sweep around a center point.
struct ConicGradient {
    float cx, cy;         ///< Center point
    float start_angle;    ///< Starting angle in radians
    std::vector<GradientStop> stops;
};

/// Gradient repeat mode.
enum class GradientTileMode { clamp, repeat, mirror };

/// Unified fill style — solid color, linear, radial, or conic gradient.
/// Usable as fill or stroke paint.
class FillStyle {
public:
    FillStyle() = default;
    FillStyle(Color c) : color_(c) {} // NOLINT: implicit for convenience
    FillStyle(LinearGradient g) : linear_(std::move(g)), type_(1) {}
    FillStyle(RadialGradient g) : radial_(std::move(g)), type_(2) {}
    FillStyle(ConicGradient g) : conic_(std::move(g)), type_(3) {}

    bool is_solid() const { return type_ == 0; }
    bool is_linear() const { return type_ == 1; }
    bool is_radial() const { return type_ == 2; }
    bool is_conic() const { return type_ == 3; }

    const Color& color() const { return color_; }
    const LinearGradient& linear() const { return linear_; }
    const RadialGradient& radial() const { return radial_; }
    const ConicGradient& conic() const { return conic_; }

    GradientTileMode tile_mode() const { return tile_mode_; }
    void set_tile_mode(GradientTileMode m) { tile_mode_ = m; }

private:
    Color color_;
    LinearGradient linear_;
    RadialGradient radial_;
    ConicGradient conic_;
    int type_ = 0;  // 0=solid, 1=linear, 2=radial, 3=conic
    GradientTileMode tile_mode_ = GradientTileMode::clamp;
};

using Paint = std::variant<Color, LinearGradient, RadialGradient, ConicGradient>;

// ── Drawing commands ─────────────────────────────────────────────────────────

enum class LineCap { butt, round, square };
enum class LineJoin { miter, round, bevel };
// pulp Wave 2 canvas2d cheap wiring — Canvas2D fill rule for fill() / clip().
// Mirrors the spec's ('nonzero' | 'evenodd') string enum. Default 'nonzero'
// matches CSS behaviour and the JS shim's fillRule==='evenodd'?1:0 mapping in
// `core/view/js/web-compat-canvas.js` so existing call sites remain
// behaviourally identical.
enum class FillRule { nonzero, evenodd };
// pulp #1434 — added `justify` for CSS / RN `text-align: justify`.
// SkiaCanvas dispatches `kJustify` via SkParagraph when the backend
// supports it; CG / RecordingCanvas back-ends approximate as `left`
// (no kerning-controlled space distribution) until full SkParagraph
// integration lands. `auto` (writing-direction-relative) is resolved
// at the widget layer before reaching the canvas — Label::paint
// translates `auto` → `left` (LTR) or `right` (RTL).
enum class TextAlign { left, center, right, justify };
enum class TextVerticalAlign { top, center, bottom, baseline };
enum class TextBaseline { top, middle, bottom };
enum class TextDirection { left_to_right, right_to_left, top_to_bottom, bottom_to_top };

// Abstract canvas for 2D drawing
// Widgets paint against this interface. Concrete backends (Skia, CoreGraphics,
// software) implement the virtual methods.
class Canvas {
public:
    virtual ~Canvas() = default;

    // ── State ────────────────────────────────────────────────────────────
    virtual void save() = 0;
    virtual void restore() = 0;

    /// Return the current save-stack depth. Used by CanvasWidget::paint()
    /// (pulp #1368) to defend against JS-driven `ctx.save()` / `ctx.restore()`
    /// imbalance: snapshot the depth at paint entry, replay the queued
    /// commands, then `restore_to_count(initial_depth)` to drop any leftover
    /// saves. Mirrors SkCanvas::getSaveCount / CGContext save-stack depth.
    /// Default returns 0 — backends without an introspectable stack rely on
    /// the default `restore_to_count` no-op below to leave behavior unchanged.
    virtual int save_count() const { return 0; }

    /// Pop the save stack down to `target` (typically captured earlier via
    /// `save_count()`). Backends that support it pop any leftover saves so an
    /// unbalanced JS draw script can't leak a `ctx.save()` into the parent
    /// View's paint scope (pulp #1368). Default is a no-op so non-tracking
    /// backends keep their existing behavior; the CanvasWidget defense is
    /// meaningful only on backends that override both methods.
    virtual void restore_to_count(int target) { (void)target; }

    // ── Transform ────────────────────────────────────────────────────────
    virtual void translate(float x, float y) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotate(float radians) = 0;

    /// Replace the current transform with the affine matrix
    /// [ a c e ]
    /// [ b d f ]
    /// [ 0 0 1 ]
    /// Mirrors CanvasRenderingContext2D.setTransform(a, b, c, d, e, f).
    /// Default implementation is a no-op so non-GPU backends still compile;
    /// SkiaCanvas overrides to compose the supplied matrix onto the paint
    /// baseline captured via capture_paint_baseline_transform().
    virtual void set_transform(float a, float b, float c,
                               float d, float e, float f) {
        (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    }

    /// Snapshot the current device matrix as the baseline for subsequent
    /// set_transform() calls. CanvasWidget::paint() calls this at entry so
    /// JS-supplied setTransform() composes onto the parent View transform
    /// rather than wiping it. Default no-op for non-Skia backends.
    virtual void capture_paint_baseline_transform() {}

    /// Concat (multiply) the supplied affine matrix onto the current
    /// transform — does NOT replace it. Used by View::paint_all() when a
    /// JS-supplied setTransform(id,a,b,c,d,e,f) is active on a View, so the
    /// View's transform composes with parent transforms (translate to bounds,
    /// outer transforms, etc.) rather than wiping them. Mirrors SkCanvas::concat.
    /// Matrix layout matches CanvasRenderingContext2D.setTransform:
    ///   [ a c e ]
    ///   [ b d f ]
    ///   [ 0 0 1 ]
    /// Default no-op so non-Skia backends compile; Skia overrides to call
    /// SkCanvas::concat, RecordingCanvas overrides to capture the command.
    virtual void concat_transform(float a, float b, float c,
                                  float d, float e, float f) {
        (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    }

    /// Affine 2x3 transform snapshot, six floats laid out as
    /// CanvasRenderingContext2D.setTransform / DOMMatrix:
    ///   [ a c e ]
    ///   [ b d f ]
    ///   [ 0 0 1 ]
    /// Identity is `{1, 0, 0, 1, 0, 0}`.
    struct AffineTransform2x3 {
        float a = 1.0f, b = 0.0f;
        float c = 0.0f, d = 1.0f;
        float e = 0.0f, f = 0.0f;
    };

    /// Snapshot the current device matrix (CTM) without mutating canvas
    /// state. Used by `CanvasWidget::paint()` diagnostics (pulp #1368) to
    /// log the inbound transform per paint when `PULP_LOG_CANVAS_PAINT=1`
    /// is set, so we can confirm whether a missing canvas paint is caused
    /// by the CTM ending up off-window vs the widget never being painted
    /// at all. Default returns identity for backends without an
    /// introspectable matrix; SkiaCanvas (`getTotalMatrix`),
    /// CoreGraphicsCanvas (`CGContextGetCTM`), and RecordingCanvas (manual
    /// matrix tracking) override.
    virtual AffineTransform2x3 current_transform() const {
        return {};
    }

    // ── Clipping ─────────────────────────────────────────────────────────
    virtual void clip_rect(float x, float y, float w, float h) = 0;

    /// Intersect the current clip region with the current path.
    /// Mirrors CanvasRenderingContext2D.clip(). Default no-op so
    /// backends without a path builder remain unaffected.
    /// `rule` selects nonzero vs evenodd winding (Wave 2 cheap wiring).
    virtual void clip(FillRule rule = FillRule::nonzero) { (void)rule; }

    /// CSS `clip-path: path("...")` — intersect the current clip with
    /// an SVG-path-d string (pulp #1515). Skia maps to
    /// `SkPath::FromSVGString` + `SkCanvas::clipPath`. RecordingCanvas
    /// captures a `clip_path_svg` command for tests; backends without a
    /// path parser fall back to a no-op so callers can invoke it
    /// unconditionally without breaking save/restore bookkeeping. The
    /// caller is responsible for save()/restore() balancing — typical
    /// usage is `save(); clip_path_svg("M..."); ...children...; restore();`.
    virtual void clip_path_svg(const std::string& svg_path_d) {
        (void)svg_path_d;
    }

    // ── Fill and stroke style ────────────────────────────────────────────
    virtual void set_fill_color(Color c) = 0;
    virtual void set_stroke_color(Color c) = 0;
    virtual void set_line_width(float w) = 0;
    virtual void set_line_cap(LineCap cap) = 0;
    virtual void set_line_join(LineJoin join) = 0;

    /// Canvas2D `ctx.miterLimit`. Sticky stroke state — controls when a
    /// `miter` join collapses to a `bevel` (Skia / CG default = 10). Spec
    /// requires non-finite or non-positive values to be silently ignored;
    /// callers that want spec semantics should clamp at the JS bridge.
    /// Default no-op so backends without miter-limit support compile;
    /// SkiaCanvas (`SkPaint::setStrokeMiter`) and CoreGraphicsCanvas
    /// (`CGContextSetMiterLimit`) override. RecordingCanvas captures a
    /// `set_miter_limit` command for tests. pulp #1434.
    virtual void set_miter_limit(float limit) { (void)limit; }

    /// Canvas2D image-smoothing quality enum. Mirrors the spec's
    /// `imageSmoothingQuality` attribute. The bridge maps the JS string
    /// values (`"low" | "medium" | "high"`) onto these.
    enum class ImageSmoothingQuality { low, medium, high };

    /// Canvas2D `ctx.imageSmoothingEnabled` + `imageSmoothingQuality`.
    /// Sticky paint flag honored by subsequent `drawImage` calls. When
    /// `enabled` is false the backend uses nearest-neighbour sampling;
    /// when true the quality enum picks the resampler. Default no-op so
    /// backends without smoothing support compile; SkiaCanvas
    /// (`SkSamplingOptions`) and CoreGraphicsCanvas
    /// (`CGContextSetInterpolationQuality`) override. RecordingCanvas
    /// captures a `set_image_smoothing` command for tests. pulp #1434.
    virtual void set_image_smoothing(bool enabled,
                                     ImageSmoothingQuality quality
                                         = ImageSmoothingQuality::low) {
        (void)enabled; (void)quality;
    }

    // ── Gradients ────────────────────────────────────────────────────────
    /// Set a linear gradient as the fill paint.
    virtual void set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                          const Color* colors, const float* positions,
                                          int count) {
        if (count > 0) set_fill_color(colors[0]); // fallback: first color
    }

    /// Set a radial gradient as the fill paint.
    virtual void set_fill_gradient_radial(float cx, float cy, float radius,
                                          const Color* colors, const float* positions,
                                          int count) {
        if (count > 0) set_fill_color(colors[0]);
    }

    /// pulp #1524 — Canvas2D `ctx.createRadialGradient(x0,y0,r0,x1,y1,r1)`
    /// two-circle form. (x0,y0,r0) is the inner / start circle, (x1,y1,r1)
    /// is the outer / end circle. Backends with a real two-circle shader
    /// (Skia `MakeTwoPointConical`, CG `CGContextDrawRadialGradient`)
    /// override; the default forwards to the single-circle overload using
    /// the outer circle so older fallbacks still get a usable gradient.
    virtual void set_fill_gradient_radial_two_circles(
            float x0, float y0, float r0,
            float x1, float y1, float r1,
            const Color* colors, const float* positions, int count) {
        (void)x0; (void)y0; (void)r0;
        set_fill_gradient_radial(x1, y1, r1, colors, positions, count);
    }

    /// Set a conic (sweep) gradient as the fill paint.
    virtual void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                          const Color* colors, const float* positions,
                                          int count) {
        if (count > 0) set_fill_color(colors[0]); // fallback
    }

    /// Clear gradient, return to solid fill color.
    virtual void clear_fill_gradient() {}

    // ── Stroke gradients (pulp Wave 3 c2d.7) ─────────────────────────────
    //
    // Mirror of the fill-gradient surface for `ctx.strokeStyle =
    // createLinearGradient(...) | createRadialGradient(...) |
    // createConicGradient(...)`. Defaults degrade to the first-stop
    // colour via `set_stroke_color`, matching the prior pre-Wave-3
    // behavior; SkiaCanvas overrides with real `SkGradientShader`
    // shaders stored on `stroke_shader_` so subsequent stroke paths
    // (stroke_rect / stroke_current_path / stroke_text) pick the
    // gradient up via `apply_stroke_state`.
    virtual void set_stroke_gradient_linear(float x0, float y0, float x1, float y1,
                                             const Color* colors, const float* positions,
                                             int count) {
        if (count > 0) set_stroke_color(colors[0]); // fallback: first color
    }

    virtual void set_stroke_gradient_radial(float cx, float cy, float radius,
                                             const Color* colors, const float* positions,
                                             int count) {
        if (count > 0) set_stroke_color(colors[0]);
    }

    /// Two-circle radial gradient on stroke (mirrors fill counterpart).
    virtual void set_stroke_gradient_radial_two_circles(
            float x0, float y0, float r0,
            float x1, float y1, float r1,
            const Color* colors, const float* positions, int count) {
        (void)x0; (void)y0; (void)r0;
        set_stroke_gradient_radial(x1, y1, r1, colors, positions, count);
    }

    virtual void set_stroke_gradient_conic(float cx, float cy, float start_angle,
                                            const Color* colors, const float* positions,
                                            int count) {
        if (count > 0) set_stroke_color(colors[0]);
    }

    /// Clear stroke gradient/pattern shader, restore solid stroke color.
    virtual void clear_stroke_gradient() {}

    /// pulp #1434 bridge-thin gap-fill — Canvas2D `ctx.createPattern`.
    /// Tile mode per axis: `repeat` mirrors Skia's `SkTileMode::kRepeat`,
    /// `no_repeat` mirrors `SkTileMode::kDecal`. Spec values map as:
    ///   "repeat"     → (repeat,    repeat)
    ///   "repeat-x"   → (repeat,    no_repeat)
    ///   "repeat-y"   → (no_repeat, repeat)
    ///   "no-repeat"  → (no_repeat, no_repeat)
    enum class PatternTileMode { repeat, no_repeat };

    /// Set an image pattern as the fill paint. `image_src` is a file path
    /// or `data:` URL — same identifier shape `draw_image_from_file`
    /// consumes, so backends share one decode path. Default is no-op so
    /// CPU-only / minimal canvases compile; SkiaCanvas overrides with a
    /// real `SkShader::MakeImage`. Empty `image_src` clears any active
    /// pattern (mirrors `clear_fill_gradient`'s reset semantics).
    virtual void set_fill_pattern(const std::string& image_src,
                                   PatternTileMode tile_x,
                                   PatternTileMode tile_y) {
        (void)image_src; (void)tile_x; (void)tile_y;
    }

    /// Stroke counterpart. Rare in production code; default no-op.
    /// SkiaCanvas overrides via the same `SkShader::MakeImage` path
    /// applied to the stroke paint.
    virtual void set_stroke_pattern(const std::string& image_src,
                                     PatternTileMode tile_x,
                                     PatternTileMode tile_y) {
        (void)image_src; (void)tile_x; (void)tile_y;
    }

    // ── Blend modes ─────────────────────────────────────────────────────
    /// Indices 0..15 match the existing W3C "advanced" composite ops and
    /// must stay stable — set_blend_mode in the JS bridge currently
    /// passes int_val 1/2/3 = multiply/screen/overlay. Indices 16+ are
    /// Porter-Duff and "extra" CSS composite ops added so the JS bridge
    /// can support the full CanvasRenderingContext2D.globalCompositeOperation
    /// surface (issue-896).
    enum class BlendMode {
        // Advanced (separable & non-separable W3C blend modes)
        normal = 0, multiply, screen, overlay, darken, lighten,
        color_dodge, color_burn, hard_light, soft_light,
        difference, exclusion, hue, saturation, color, luminosity,
        // Porter-Duff compositing modes (issue-896)
        source_over,        // 16 — alias for normal/source-over (CSS default)
        destination_over,   // 17
        source_in,          // 18
        destination_in,     // 19
        source_out,         // 20
        destination_out,    // 21
        source_atop,        // 22
        destination_atop,   // 23
        xor_mode,           // 24 — "xor" CSS string
        copy,               // 25
        lighter             // 26 — additive ("plus" / SVG kPlus)
    };

    /// Set the compositing blend mode for subsequent draw operations.
    virtual void set_blend_mode(BlendMode mode) { (void)mode; }

    // ── Path building ───────────────────────────────────────────────────
    /// Begin a new path.
    virtual void begin_path() {}
    /// Move the pen to (x, y) without drawing.
    virtual void move_to(float x, float y) { (void)x; (void)y; }
    /// Add a line segment to (x, y).
    virtual void line_to(float x, float y) { (void)x; (void)y; }
    /// Add a quadratic bezier curve to (x, y) with control point (cpx, cpy).
    virtual void quad_to(float cpx, float cpy, float x, float y) {
        (void)cpx; (void)cpy; line_to(x, y); // fallback: straight line
    }
    /// Add a cubic bezier curve.
    virtual void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y,
                          float x, float y) {
        (void)cp1x; (void)cp1y; (void)cp2x; (void)cp2y; line_to(x, y);
    }
    /// pulp #1521 — native arc subpath. Maps to Canvas2D
    /// `ctx.arc(cx, cy, r, startAngle, endAngle, anticlockwise)` and to
    /// SkPath::arcTo(SkRect, startDeg, sweepDeg, false) on Skia. Replaces
    /// the JS shim's bezier approximation. The default fallback emits a
    /// quadrant-segmented cubic-bezier polyline so capture-only backends
    /// (RecordingCanvas) still produce a meaningful command stream when
    /// they choose not to override.
    virtual void arc(float cx, float cy, float radius,
                     float start_angle, float end_angle,
                     bool anticlockwise) {
        (void)cx; (void)cy; (void)radius;
        (void)start_angle; (void)end_angle; (void)anticlockwise;
    }
    /// pulp #1521 — native arcTo. Maps to Canvas2D
    /// `ctx.arcTo(x1, y1, x2, y2, radius)` and to
    /// SkPath::arcTo(p1, p2, radius) on Skia (the 5-arg overload that
    /// computes the tangent arc between the current point, p1, and p2).
    virtual void arc_to(float x1, float y1, float x2, float y2, float radius) {
        (void)x1; (void)y1; (void)x2; (void)y2; (void)radius;
    }
    /// pulp #1521 — native ellipse subpath. Maps to Canvas2D
    /// `ctx.ellipse(cx, cy, rx, ry, rotation, startAngle, endAngle,
    /// anticlockwise)`. Honours `rotation` (rx/ry axis rotation in
    /// radians) by transforming the arc through SkMatrix on Skia.
    virtual void ellipse(float cx, float cy, float rx, float ry,
                         float rotation,
                         float start_angle, float end_angle,
                         bool anticlockwise) {
        (void)cx; (void)cy; (void)rx; (void)ry; (void)rotation;
        (void)start_angle; (void)end_angle; (void)anticlockwise;
    }
    /// pulp #1521 — native rounded-rectangle subpath with per-corner
    /// radii. Maps to Canvas2D `ctx.roundRect(x, y, w, h, radii)` and to
    /// SkRRect::MakeRectRadii on Skia. Radii layout matches the CSS
    /// 8-value form: top-left x/y, top-right x/y, bottom-right x/y,
    /// bottom-left x/y. The JS shim normalizes the 1/2/3/4-value forms
    /// to 8 floats before crossing the bridge.
    virtual void round_rect(float x, float y, float w, float h,
                            float tl_x, float tl_y,
                            float tr_x, float tr_y,
                            float br_x, float br_y,
                            float bl_x, float bl_y) {
        (void)x; (void)y; (void)w; (void)h;
        (void)tl_x; (void)tl_y; (void)tr_x; (void)tr_y;
        (void)br_x; (void)br_y; (void)bl_x; (void)bl_y;
    }
    /// Close the current path subpath.
    virtual void close_path() {}
    /// Fill the current path. `rule` selects nonzero vs evenodd winding
    /// (Wave 2 cheap wiring; matches Canvas2D `ctx.fill('evenodd')`).
    virtual void fill_current_path(FillRule rule = FillRule::nonzero) {
        (void)rule;
    }
    /// Stroke the current path.
    virtual void stroke_current_path() {}

    // ── Bloom / Glow post-effect ────────────────────────────────────────
    /// Apply bloom/glow effect to the current layer.
    /// intensity: glow strength (0=none, 1=full), threshold: brightness cutoff (0-1).
    virtual void set_bloom(float intensity, float threshold = 0.7f) {
        (void)intensity; (void)threshold;
    }

    // ── Shapes ───────────────────────────────────────────────────────────
    virtual void fill_rect(float x, float y, float w, float h) = 0;
    virtual void stroke_rect(float x, float y, float w, float h) = 0;
    virtual void fill_rounded_rect(float x, float y, float w, float h, float radius) = 0;
    virtual void stroke_rounded_rect(float x, float y, float w, float h, float radius) = 0;
    virtual void fill_circle(float cx, float cy, float radius) = 0;
    virtual void stroke_circle(float cx, float cy, float radius) = 0;

    /// Clear a rectangular region to transparent black (rgba 0,0,0,0).
    /// Equivalent to CanvasRenderingContext2D.clearRect — replaces existing
    /// pixels rather than compositing over them. Pixel backends (SkiaCanvas)
    /// override with a kSrc/kClear paint so the underlying texels actually
    /// become transparent; the default falls back to a SrcOver fill with a
    /// transparent color (a no-op on most surfaces, but keeps recording /
    /// introspection backends inert). See pulp issue #929 — without this,
    /// ctx.clearRect() on the canvas widget would not clear residual or
    /// parent-painted pixels.
    virtual void clear_rect(float x, float y, float w, float h) {
        // Default: best-effort SrcOver fill with transparent color. Pixel
        // backends should override to use kSrc / kClear blend.
        set_fill_color(Color::rgba(0.0f, 0.0f, 0.0f, 0.0f));
        fill_rect(x, y, w, h);
    }

    // ── Arcs ─────────────────────────────────────────────────────────────
    virtual void stroke_arc(float cx, float cy, float radius,
                           float start_angle, float end_angle) = 0;

    // ── Lines ────────────────────────────────────────────────────────────
    virtual void stroke_line(float x0, float y0, float x1, float y1) = 0;

    /// Stroke a continuous polyline through an array of points.
    /// Much smoother than individual stroke_line calls — produces proper
    /// anti-aliased curves with line joins between segments.
    struct Point2D { float x, y; };

    /// Stroke a continuous polyline — much smoother than individual stroke_line calls.
    virtual void stroke_path(const Point2D* points, size_t count) {
        // Default fallback: individual line segments (subclass should override)
        for (size_t i = 1; i < count; ++i)
            stroke_line(points[i-1].x, points[i-1].y, points[i].x, points[i].y);
    }

    /// Fill a closed polygon defined by points.
    virtual void fill_path(const Point2D* points, size_t count) {
        (void)points; (void)count; // Default: no-op, subclass should override
    }

    /// Draw an image identified by an opaque, platform-specific handle
    /// into the rectangle (x, y, w, h). The handle is the \c native_handle
    /// published by \c pulp::view::ImageCache (see core/view/include/pulp/view/image_cache.hpp).
    /// Backends know how to interpret their own handles:
    ///   * CoreGraphicsCanvas: CGImageRef
    ///   * SkiaCanvas: SkImage*
    /// Canvases without an image pipeline are a no-op by default so
    /// widgets can call this unconditionally — the placeholder layer
    /// behind them keeps the UI non-blank.
    ///
    /// Workstream 07 slice B4 follow-up (#255).
    virtual void draw_image(void* native_handle,
                            float x, float y, float w, float h) {
        (void)native_handle; (void)x; (void)y; (void)w; (void)h;
    }

    // ── Opacity & Layers ──────────────────────────────────────────────────
    /// Set global alpha for subsequent drawing operations (0.0-1.0).
    virtual void set_opacity(float alpha) { (void)alpha; }

    /// Save a compositing layer. All drawing until restore() is composited
    /// with the given opacity and optional blur. This is the correct way to
    /// implement CSS opacity and filter:blur() — the subtree paints into an
    /// offscreen buffer and is composited back as a single unit.
    virtual void save_layer(float x, float y, float w, float h,
                            float opacity = 1.0f, float blur_radius = 0.0f) {
        save(); // fallback: just save state
        (void)x; (void)y; (void)w; (void)h;
        (void)opacity; (void)blur_radius;
    }

    /// pulp #1549 — Save a compositing layer with an explicit blend mode.
    /// Used by CSS / RN `mix-blend-mode`: the subtree paints into the
    /// offscreen layer and the layer-paint composites back with the
    /// requested blend mode. Default `BlendMode::normal` is equivalent to
    /// the plain `save_layer` overload (kSrcOver). Subclasses that don't
    /// know how to honor the blend mode fall back to the plain overload.
    virtual void save_layer_with_blend(float x, float y, float w, float h,
                                       float opacity, float blur_radius,
                                       BlendMode mode) {
        (void)mode;
        save_layer(x, y, w, h, opacity, blur_radius);
    }

    /// pulp #1434 Phase A2-4 — full CSS filter-chain layer save.
    /// Each entry in `chain` is one filter function; the canvas backend
    /// composes them via `SkImageFilters::Compose` (Skia) or, for
    /// CG-only paths, falls back to whichever entries the platform can
    /// render natively. Default impl falls through to `save_layer` with
    /// any blur entries collapsed to a single radius — preserves the
    /// blur-only behavior that platforms without filter-chain support
    /// already had.
    struct FilterChainEntry {
        enum class Kind {
            blur,
            brightness,
            contrast,
            grayscale,
            hue_rotate,
            invert,
            opacity,
            saturate,
            sepia,
            drop_shadow,
        };
        Kind kind = Kind::blur;
        float amount = 0.0f;
        float angle_deg = 0.0f;
        float ds_offset_x = 0.0f;
        float ds_offset_y = 0.0f;
        float ds_blur = 0.0f;
        Color ds_color{};
    };
    virtual void save_layer_with_filters(float x, float y, float w, float h,
                                          float opacity,
                                          const FilterChainEntry* chain,
                                          int count) {
        // Fallback: collapse to single-blur save_layer (prior behavior
        // for platforms without filter-chain support).
        float blur = 0.0f;
        for (int i = 0; i < count; ++i) {
            if (chain[i].kind == FilterChainEntry::Kind::blur) {
                blur += chain[i].amount;
            } else if (chain[i].kind == FilterChainEntry::Kind::opacity) {
                opacity *= chain[i].amount;
            }
        }
        save_layer(x, y, w, h, opacity, blur);
    }

    // ── Text ─────────────────────────────────────────────────────────────
    virtual void set_font(const std::string& family, float size) = 0;
    virtual void set_text_align(TextAlign align) = 0;
    virtual void fill_text(const std::string& text, float x, float y) = 0;
    virtual float measure_text(const std::string& text) = 0;

    /// pulp #1525 — Canvas2D `fillText(text, x, y, maxWidth)` spec form.
    /// When `max_width > 0` and the natural rendered advance exceeds it,
    /// the backend MUST scale the text horizontally so the resulting run
    /// is exactly `max_width` px wide (vertical metrics unchanged). The
    /// default implementation falls through to `fill_text(text, x, y)`,
    /// preserving legacy behaviour for backends that don't yet honour
    /// the constraint (RecordingCanvas, base mocks). Skia and CoreGraphics
    /// override this to apply an `SkMatrix::Scale(maxWidth/measured, 1)`
    /// (Skia) / `CGContextScaleCTM(scale, 1)` (CG) around the text origin.
    /// Glyph cluster boundaries are unaffected — HarfBuzz already shapes
    /// each cluster as an indivisible unit, so horizontal compression
    /// preserves cluster integrity by construction.
    virtual void fill_text_with_max_width(const std::string& text,
                                          float x, float y, float max_width) {
        (void)max_width;
        fill_text(text, x, y);
    }

    /// pulp #1525 — Canvas2D `strokeText(text, x, y, maxWidth)` spec form.
    /// True outlined-glyph rendering: builds a stroked paint
    /// (`SkPaint::kStroke_Style` on Skia, CG text drawing mode
    /// kCTLineDrawingModeStroke on CoreGraphics) so the glyph outlines
    /// honour the current `lineWidth` / `strokeStyle`. The default
    /// implementation degrades to `fill_text` with the stroke color
    /// pre-set by the caller — visually approximate but spec-incompatible.
    /// Backends override to render real glyph outlines. `max_width`
    /// semantics match `fill_text_with_max_width`.
    virtual void stroke_text(const std::string& text, float x, float y,
                             float max_width = 0.0f) {
        (void)max_width;
        // Fallback: caller must have set fill color to stroke color before
        // invoking. Matches the pre-#1525 strokeText shim approximation.
        fill_text(text, x, y);
    }

    /// Richer font setter that propagates CSS font-weight (100..900),
    /// font-slant (0=upright, 1=italic), and letter-spacing (px between
    /// glyphs) through to the backend. Default implementation forwards to
    /// the legacy `set_font(family, size)` so non-Label callers keep
    /// working unchanged. Backends that honor these properties (Skia,
    /// CoreText, RecordingCanvas) override this to capture or apply them.
    /// pulp #927 — Label widget honors setFontFamily / setFontWeight /
    /// setLetterSpacing from JS.
    virtual void set_font_full(const std::string& family, float size,
                                int weight, int slant, float letter_spacing) {
        (void)weight; (void)slant; (void)letter_spacing;
        set_font(family, size);
    }

    /// Full text metrics for layout and intrinsic sizing.
    /// Mirrors HTML5 TextMetrics — fields beyond width/ascent/descent are
    /// the bounding-box-only metrics required by CanvasRenderingContext2D
    /// (issue-916). All values are positive and in pixels at the current
    /// font size.
    struct TextMetrics {
        float width = 0;        ///< Advance width of the text
        float ascent = 0;       ///< Distance above baseline (positive value, ==fontBoundingBoxAscent)
        float descent = 0;      ///< Distance below baseline (positive value, ==fontBoundingBoxDescent)
        float line_height = 0;  ///< Recommended line spacing (ascent + descent + leading)

        // ── HTML5 TextMetrics extensions (issue-916) ────────────────
        /// Distance from text origin to the left edge of the rendering bounding box.
        /// Positive when the bounding box extends left of the origin.
        float actual_bounding_box_left = 0;
        /// Distance from text origin to the right edge of the rendering bounding box.
        float actual_bounding_box_right = 0;
        /// Distance from baseline to top of the rendering bounding box.
        float actual_bounding_box_ascent = 0;
        /// Distance from baseline to bottom of the rendering bounding box.
        float actual_bounding_box_descent = 0;
    };

    // ── SDF Text (GPU-accelerated, resolution-independent) ────────────

    /// Render text using an SDF glyph atlas for resolution-independent
    /// rendering. The atlas must be pre-built via SdfAtlas::build().
    /// Falls back to fill_text() if GPU is unavailable or the atlas
    /// doesn't contain the required glyphs.
    virtual void fill_text_sdf(const std::string& text, float x, float y,
                               const class SdfAtlas& atlas) {
        // Default: fall back to standard text rendering
        (void)atlas;
        fill_text(text, x, y);
    }

    // ── Images ───────────────────────────────────────────────────────────
    /// Draw an image from encoded data (PNG, JPEG, WebP) at the given rect.
    /// Returns true if the image was decoded and drawn successfully.
    virtual bool draw_image_from_data(const uint8_t* data, size_t size,
                                      float x, float y, float w, float h) {
        (void)data; (void)size; (void)x; (void)y; (void)w; (void)h;
        return false; // Base class can't draw images
    }

    /// Draw an image from a file path at the given rect.
    virtual bool draw_image_from_file(const std::string& path,
                                       float x, float y, float w, float h) {
        (void)path; (void)x; (void)y; (void)w; (void)h;
        return false; // Override in Skia backend
    }

    // pulp #1737 — 9-arg drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh) source-rect
    // slicing. Default impl delegates to the dst-only form (loses the slice);
    // Skia + CG override to use drawImageRect(src, dst) / CGContextDrawImage
    // on a CGImageCreateWithImageInRect-cropped sub-image.
    virtual bool draw_image_from_file_rect(const std::string& path,
                                            float sx, float sy, float sw, float sh,
                                            float dx, float dy, float dw, float dh) {
        (void)sx; (void)sy; (void)sw; (void)sh;
        return draw_image_from_file(path, dx, dy, dw, dh);
    }
    virtual bool draw_image_from_data_rect(const uint8_t* data, size_t size,
                                            float sx, float sy, float sw, float sh,
                                            float dx, float dy, float dw, float dh) {
        (void)sx; (void)sy; (void)sw; (void)sh;
        return draw_image_from_data(data, size, dx, dy, dw, dh);
    }

    /// Measure text with full font metrics using current font settings.
    virtual TextMetrics measure_text_full(const std::string& text) {
        TextMetrics m;
        m.width = measure_text(text);
        m.ascent = font_size_ * 0.75f;   // approximate
        m.descent = font_size_ * 0.25f;
        m.line_height = font_size_ * 1.2f;
        // HTML5 TextMetrics fields (issue-916) — fall back to font-metric
        // estimates when no shaped bounding box is available.
        m.actual_bounding_box_left = 0;
        m.actual_bounding_box_right = m.width;
        m.actual_bounding_box_ascent = m.ascent;
        m.actual_bounding_box_descent = m.descent;
        return m;
    }

    // ── Line Dash (issue-916) ──────────────────────────────────────────
    /// Set the line dash pattern for stroke operations.
    /// Mirrors CanvasRenderingContext2D.setLineDash().
    /// `intervals` alternate "on" / "off" lengths in pixels. An empty
    /// pattern (count == 0) clears the dash and reverts to solid strokes.
    /// HTML5 spec: an odd-length pattern is duplicated to even length;
    /// callers that want spec compliance must duplicate before invoking
    /// this method (the JS bridge does so). Default no-op for backends
    /// without dash support.
    virtual void set_line_dash(const float* intervals, int count, float phase = 0.0f) {
        (void)intervals; (void)count; (void)phase;
    }

    // ── Pixel manipulation (issue-916) ─────────────────────────────────
    /// Read RGBA pixels from the current surface into `out` (size must be
    /// at least width*height*4). Returns true on success. Default returns
    /// false (RecordingCanvas, CG fallback) — only Skia raster surfaces
    /// implement this end-to-end.
    /// Mirrors CanvasRenderingContext2D.getImageData().
    virtual bool read_pixels(int x, int y, int width, int height, uint8_t* out) {
        (void)x; (void)y; (void)width; (void)height; (void)out;
        return false;
    }

    /// Write RGBA pixels (`data` size = width*height*4) to the current
    /// surface at destination (`dx`, `dy`). Returns true on success.
    /// Mirrors CanvasRenderingContext2D.putImageData().
    virtual bool write_pixels(const uint8_t* data, int width, int height,
                              int dx, int dy) {
        (void)data; (void)width; (void)height; (void)dx; (void)dy;
        return false;
    }

    // ── SDF Shape Primitives (GPU-accelerated) ─────────────────────────
    /// Draw an SDF shape with anti-aliased edges via GPU shader.
    enum class SDFShape {
        rect,           // 0 — axis-aligned rectangle
        circle,         // 1 — circle (min dimension as radius)
        rounded_rect,   // 2 — rectangle with corner_radius
        arc,            // 3 — ring arc (arc_start + arc_sweep)
        diamond,        // 4 — rotated square
        squircle,       // 5 — continuous-curvature rounded rect (power param)
        triangle,       // 6 — equilateral triangle
        ring,           // 7 — circle with inner radius (donut)
        stadium,        // 8 — pill/capsule shape
        cross,          // 9 — plus sign with configurable arm width
        flat_segment,   // 10 — line segment with flat caps
        rounded_segment,// 11 — line segment with rounded caps
        flat_arc,       // 12 — arc with thickness and flat caps
        quadratic_bezier,// 13 — quadratic bezier curve with thickness
    };

    struct SDFStyle {
        Color fill_color = Color::rgba(0.392f, 1.0f, 0.392f);
        Color stroke_color = Color::rgba(0.392f, 1.0f, 0.392f);
        float stroke_width = 0;       ///< 0 = filled, >0 = stroked
        float corner_radius = 0;      ///< For rounded_rect
        float arc_start = 0;          ///< For arc (radians)
        float arc_sweep = 4.712f;     ///< For arc (radians, default 270°)
        float squircle_power = 4.0f;  ///< For squircle (higher = more rectangular)
        float inner_radius = 0.5f;    ///< For ring (fraction of outer radius)
        float arm_width = 0.3f;       ///< For cross (fraction of half-size)
        float bezier_cx = 0.0f;       ///< For quadratic_bezier: control point X (normalized -1..1)
        float bezier_cy = -1.0f;      ///< For quadratic_bezier: control point Y (normalized -1..1)
    };

    virtual void draw_sdf_shape(SDFShape shape, float x, float y, float w, float h,
                                const SDFStyle& style) {
        // CPU fallback: use existing rect/rounded_rect
        if (style.stroke_width > 0) {
            set_stroke_color(style.stroke_color);
            set_line_width(style.stroke_width);
            if (shape == SDFShape::circle)
                stroke_rounded_rect(x, y, w, h, std::min(w, h) * 0.5f);
            else if (shape == SDFShape::rounded_rect)
                stroke_rounded_rect(x, y, w, h, style.corner_radius);
            else
                stroke_rounded_rect(x, y, w, h, 0);
        } else {
            set_fill_color(style.fill_color);
            if (shape == SDFShape::circle)
                fill_rounded_rect(x, y, w, h, std::min(w, h) * 0.5f);
            else if (shape == SDFShape::rounded_rect)
                fill_rounded_rect(x, y, w, h, style.corner_radius);
            else
                fill_rounded_rect(x, y, w, h, 0);
        }
    }

    // ── Blur / Backdrop filter ─────────────────────────────────────────
    /// Save a blurred snapshot of the current canvas content as a backdrop.
    /// Call before painting the overlay content.
    virtual void draw_blurred_backdrop(float x, float y, float w, float h,
                                       float blur_radius, float corner_radius = 0,
                                       Color tint = Color::rgba(0.0f, 0.0f, 0.0f, 0.314f)) {
        // CPU fallback: just draw a semi-transparent rect (no blur)
        set_fill_color(tint);
        fill_rounded_rect(x, y, w, h, corner_radius);
    }

    /// CSS `backdrop-filter: blur(Npx)` — push a compositing layer whose
    /// initial contents are the parent surface filtered through a Gaussian
    /// blur of `blur_radius`. Subsequent draws into this layer composite
    /// over the blurred backdrop. Must be paired with restore() (issue-926).
    ///
    /// Skia maps to `SkCanvas::saveLayer(SaveLayerRec{ .fBackdrop = Blur })`.
    /// CPU/recording fallback is a plain save() so the matching restore()
    /// stays balanced — visual fidelity downgrades to an unblurred overlay.
    virtual void save_backdrop_filter(float x, float y, float w, float h,
                                      float blur_radius) {
        (void)x; (void)y; (void)w; (void)h; (void)blur_radius;
        save();
    }

    // ── Box shadow (issue-925) ─────────────────────────────────────────
    /// Draw a CSS-style box shadow around (or inside, when inset) a
    /// rounded rectangle anchored at (x, y, w, h). When `inset` is false
    /// this is a drop shadow rendered outside the box; when true it is an
    /// inner shadow rendered inside the box clipped to the box geometry.
    /// Default implementation is a CPU fallback that approximates the
    /// blur with stacked translucent rounded rects — Skia overrides with
    /// SkImageFilters::DropShadowOnly for a true Gaussian shadow. The
    /// out-of-line definition lives in core/canvas/src/recording_canvas.cpp
    /// so the CPU fallback is exercised by the canvas-level tests during
    /// coverage runs.
    virtual void draw_box_shadow(float x, float y, float w, float h,
                                 float dx, float dy, float blur, float spread,
                                 Color color, bool inset = false,
                                 float corner_radius = 0.0f);

    // ── Canvas2D drop shadow state (issue-1434 batch 7) ────────────────
    /// Sticky drop-shadow state that wraps subsequent draw operations,
    /// matching the CanvasRenderingContext2D `shadowColor` / `shadowBlur`
    /// / `shadowOffsetX` / `shadowOffsetY` properties. The shadow renders
    /// only when `color.a > 0` AND (blur > 0 OR offset_x != 0 OR
    /// offset_y != 0) — same gate Chromium / WebKit use. Setting color to
    /// fully transparent (alpha == 0) effectively disables the shadow even
    /// if blur/offset are non-zero, mirroring spec semantics.
    ///
    /// Backends that draw via SkPaint / CGContext layer the shadow onto
    /// the next draw call by configuring an image filter (Skia
    /// DropShadow) or `CGContextSetShadowWithColor` respectively. The
    /// default implementation here is a no-op so backends that haven't
    /// opted into shadow support compile unchanged; SkiaCanvas,
    /// CoreGraphicsCanvas, and RecordingCanvas override to actually
    /// honor (or capture) the state.
    virtual void set_shadow_color(Color color) { (void)color; }
    virtual void set_shadow_blur(float blur)   { (void)blur; }
    virtual void set_shadow_offset_x(float dx) { (void)dx; }
    virtual void set_shadow_offset_y(float dy) { (void)dy; }

    // ── Canvas2D direction / filter (pulp #1520) ───────────────────────
    /// Canvas2D `ctx.direction`. Sticky text-shaping direction that
    /// applies to subsequent fillText / strokeText calls. Spec values:
    ///   ltr     — left-to-right (default; matches SkShaper leftToRight=true)
    ///   rtl     — right-to-left (HarfBuzz buffer direction RTL)
    ///   inherit — pulled from the canvas element / document writing
    ///             direction. On backends without a per-View writing
    ///             direction yet, treated as ltr (the most common case).
    /// Default no-op so backends without a real bidi/HarfBuzz path
    /// remain unaffected; SkiaCanvas overrides to wire through to the
    /// SkShaper invocation flag, RecordingCanvas captures one
    /// `set_direction` command per setter so canvas2d harness tests
    /// can assert flush order. Real bidi support (mixed-script
    /// paragraphs requiring the Bidi algorithm) tracks separately.
    enum class TextDirection { ltr, rtl, inherit };
    virtual void set_direction(TextDirection direction) { (void)direction; }

    /// Canvas2D `ctx.filter`. Sticky CSS <filter-function-list> string
    /// applied to subsequent fill / stroke / text / image draws. Spec
    /// supports: blur, brightness, contrast, drop-shadow, grayscale,
    /// hue-rotate, invert, opacity, saturate, sepia. The default is
    /// "none". SkiaCanvas parses the string into an SkImageFilter chain
    /// and applies via SkPaint::setImageFilter; CG and other backends
    /// can store the value but render unfiltered. RecordingCanvas
    /// captures the raw string. Distinct from CSS `filter` on a View
    /// (#1503) — that filter applies to the View element, this one
    /// applies to the per-context Canvas2D paints inside it.
    virtual void set_filter(const std::string& filter) { (void)filter; }

    // ── Waveform (GPU-accelerated) ─────────────────────────────────────
    /// Draw a waveform using GPU shader (SDF anti-aliased line + fill).
    /// Samples are normalized -1 to 1. Default implementation falls back to polyline.
    struct WaveformStyle {
        Color line_color = Color::rgba(0.392f, 0.706f, 0.980f);
        Color fill_color = Color::rgba(0.392f, 0.706f, 0.980f, 0.157f);
        float line_thickness = 1.5f;
        bool show_fill = true;
        float fill_center = 0.5f;  ///< 0=top, 0.5=center, 1=bottom
    };

    virtual void draw_waveform(const float* samples, size_t count,
                               float x, float y, float width, float height,
                               const WaveformStyle& style) {
        // CPU fallback: connected line strip
        if (count < 2) return;
        set_stroke_color(style.line_color);
        set_line_width(style.line_thickness);
        float cy = y + height * style.fill_center;
        float half_h = height * 0.5f;
        float prev_x = x;
        float prev_y = cy - samples[0] * half_h;
        for (size_t i = 1; i < count; ++i) {
            float sx = x + (static_cast<float>(i) / static_cast<float>(count - 1)) * width;
            float sy = cy - samples[i] * half_h;
            stroke_line(prev_x, prev_y, sx, sy);
            prev_x = sx;
            prev_y = sy;
        }
    }

    // ── Custom SkSL Shader Rendering (GPU) ──────────────────────────────
    /// Uniforms passed to custom widget shaders.
    struct ShaderUniforms {
        float value = 0;           ///< Widget value (0-1)
        float time = 0;            ///< Animation time (seconds)
        Color accent_color{};
        Color bg_color{};
        Color track_color{};
        Color fill_color{};
        Color thumb_color{};
    };

    /// Validate and compile an SkSL shader without drawing. Returns error string (empty = success).
    /// Static so it can be called without a Canvas instance.
    static std::string compile_sksl(const std::string& sksl);

    /// Draw a rectangle filled by a custom SkSL shader.
    /// Only works on GPU backends (SkiaCanvas). CPU backends draw a fallback rect.
    /// The shader receives: uniform float2 resolution, float value, float time,
    /// layout(color) float4 accentColor/bgColor/trackColor/fillColor/thumbColor.
    virtual bool draw_with_sksl(const std::string& sksl,
                                float x, float y, float w, float h,
                                const ShaderUniforms& uniforms) {
        // CPU fallback: draw a colored placeholder rect
        set_fill_color(uniforms.fill_color.a > 0.0f ? uniforms.fill_color : Color::rgba(0.314f, 0.314f, 0.392f, 0.784f));
        fill_rect(x, y, w, h);
        return false; // shader not rendered
    }

protected:
    float font_size_ = 14.0f;  ///< Current font size (set by set_font)
};

// ── Recording canvas ─────────────────────────────────────────────────────────
// Captures draw commands for inspection/testing
// Does not actually render anything

struct DrawCommand {
    enum class Type {
        save, restore,
        translate, scale, rotate, clip_rect,
        set_transform, clip, set_blend_mode,    // issue-896
        concat_transform,                       // issue-930
        set_fill_color, set_stroke_color, set_line_width,
        set_line_cap, set_line_join,
        fill_rect, stroke_rect, fill_rounded_rect, stroke_rounded_rect,
        fill_circle, stroke_circle, stroke_arc, stroke_line,
        set_font, set_text_align, fill_text,
        // pulp #1525 — Canvas2D `strokeText(text, x, y, maxWidth)` records
        // a distinct cmd so tests can assert that the bridge routed the
        // call through the dedicated stroke_text path (rather than the
        // pre-#1525 fillText-with-stroke-color approximation). Layout
        // matches fill_text: text in `text`, (x,y) in f[0..1], optional
        // maxWidth in f[2] (<=0 = no limit).
        stroke_text,
        // pulp #927 — full font setter: family in `text`, size/weight/slant/
        // letter_spacing in f[0..3]. Emitted alongside (in addition to) the
        // legacy set_font command so existing tests that count set_font
        // continue to pass.
        set_font_full,
        // ── issue-916: Canvas2D API gaps ──────────────────────────────
        set_line_dash,      ///< intervals stored in `floats`, phase in f[0]
        draw_image,         ///< source path/url in `text`, dst rect in f[0..3]
        write_pixels,       ///< RGBA bytes in `text` (binary), w/h in f[0..1], dst in f[2..3]
        // ── issue-925: setBoxShadow / draw_box_shadow ─────────────────
        draw_box_shadow,    ///< x/y/w/h in f[0..3], blur in f[4], spread/offsets via floats payload
        // ── issue-1434 batch 7: Canvas2D shadow* state setters ────────
        // Sticky state changes; the recording target captures one cmd
        // per setter so tests can assert on the bridge's flush order.
        set_shadow_color,    ///< color in `color`
        set_shadow_blur,     ///< blur (px) in f[0]
        set_shadow_offset_x, ///< dx (px) in f[0]
        set_shadow_offset_y, ///< dy (px) in f[0]
        // ── issue-1434 bridge-thin gap-fill: Canvas2D state setters ───
        // Sticky stroke / image state. Captured one cmd per setter so
        // the canvas2d bridge harness can assert flush order.
        set_miter_limit,     ///< limit in f[0]
        set_image_smoothing, ///< enabled in f[0] (0/1), quality in f[1] (0=low,1=med,2=high)
        // pulp #1520 — Canvas2D ctx.direction / ctx.filter sticky setters.
        // Direction enum (0=ltr, 1=rtl, 2=inherit) packed into f[0];
        // filter raw CSS <filter-function-list> string (e.g.
        // "blur(5px) sepia(80%)") in `text`. RecordingCanvas captures
        // each setter so tests can assert the JS shim flushed the
        // sticky state before the next text/image/fill draw.
        set_direction,
        set_filter,
        // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createPattern.
        // image source path / data URI in `text`, tile modes packed into
        // f[0] (x) and f[1] (y) — 0 = repeat, 1 = no_repeat.
        set_fill_pattern,
        set_stroke_pattern,
        // pulp Wave 3 c2d.7 — Canvas2D `ctx.strokeStyle =
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
        // ── issue-926: save_backdrop_filter for frosted-glass overlays ─
        save_backdrop_filter, ///< x/y/w/h in f[0..3], blur_radius in f[4]
        // ── pulp #1515: CSS clip-path: path("...") ────────────────────
        // Recording target captures the SVG-path-d string in `text` so
        // tests can assert that View::paint_all installs the clip
        // before painting children. Skia parses via SkParsePath /
        // SkPath::FromSVGString; backends without a path parser
        // degrade to a no-op (no clip applied).
        clip_path_svg,         ///< SVG path data in `text`
        // ── issue-929: real clearRect that replaces pixels ────────────
        clear_rect,          ///< clear rect, x/y/w/h in f[0..3]
        // ── issue-965: Canvas2D path API recording ────────────────────
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
        // ── pulp #1521: native arc subpaths (replace JS bezier approx) ─
        // Captured so widgets that emit native arc commands can be
        // asserted at the command-stream level without a Skia raster
        // surface. Maps 1:1 to the new Canvas::arc / arc_to / ellipse /
        // round_rect virtual methods.
        arc,                  ///< (cx, cy, r, start, end, anticlockwise as 0/1) in f[0..5]
        arc_to,               ///< (x1, y1, x2, y2, radius) in f[0..4]
        ellipse,              ///< (cx, cy, rx, ry, rotation, start) in f[0..5]; (end, anticlockwise as 0/1) extras tracked in `floats[0..1]`
        round_rect            ///< (x, y, w, h, tl_x, tl_y) in f[0..5]; tr/br/bl x/y in `floats[0..5]`
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
    // exactly once at entry (issue-897).
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
    // pulp #1525 — Canvas2D fillText(text,x,y,maxWidth) + strokeText.
    // The recording target captures `max_width` in `f[2]` so harness
    // tests can assert the bridge plumbed the optional fourth arg
    // through. `<=0` is the spec sentinel for "no constraint".
    void fill_text_with_max_width(const std::string& text,
                                  float x, float y, float max_width) override;
    void stroke_text(const std::string& text, float x, float y,
                     float max_width = 0.0f) override;
    float measure_text(const std::string& text) override;

    // issue-916 — capture the new commands so JS-driven tests can assert
    // on them via DrawCommand::Type::set_line_dash / draw_image /
    // write_pixels. Source path is stored in DrawCommand::text.
    void set_line_dash(const float* intervals, int count, float phase) override;
    bool draw_image_from_data(const uint8_t* data, size_t size,
                              float x, float y, float w, float h) override;
    bool draw_image_from_file(const std::string& path,
                              float x, float y, float w, float h) override;
    // pulp #1737 — record the source-rect overload so widget-bridge tests
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

    // issue-925 — capture a single box-shadow command so JS-driven tests
    // can assert on inset / color / offsets without having to walk the
    // CPU-fallback rectangle stack.
    void draw_box_shadow(float x, float y, float w, float h,
                         float dx, float dy, float blur, float spread,
                         Color color, bool inset, float corner_radius) override;

    // issue-1434 batch 7 — capture sticky Canvas2D shadow state setters
    // so tests can assert that JS `ctx.shadowColor = ...; ctx.shadowBlur =
    // ...; ctx.fillRect(...)` flushes the shadow state through to the
    // canvas before the geometry is recorded.
    void set_shadow_color(Color color) override;
    void set_shadow_blur(float blur) override;
    void set_shadow_offset_x(float dx) override;
    void set_shadow_offset_y(float dy) override;

    // issue-1434 bridge-thin gap-fill — capture sticky stroke / image
    // state so tests can assert that JS `ctx.miterLimit = ...` and
    // `ctx.imageSmoothingEnabled = ...` flush through to the canvas
    // before the next geometry is recorded.
    void set_miter_limit(float limit) override;
    void set_image_smoothing(bool enabled,
                             ImageSmoothingQuality quality) override;

    // pulp #1520 — Canvas2D ctx.direction / ctx.filter capture.
    void set_direction(TextDirection direction) override;
    void set_filter(const std::string& filter) override;

    // pulp #1434 bridge-thin gap-fill — capture pattern setter intents
    // so canvas2d harness tests can assert flush order without needing
    // a real raster surface or decoded image.
    void set_fill_pattern(const std::string& image_src,
                          PatternTileMode tile_x,
                          PatternTileMode tile_y) override;
    void set_stroke_pattern(const std::string& image_src,
                            PatternTileMode tile_x,
                            PatternTileMode tile_y) override;

    // pulp Wave 3 c2d.7 — capture stroke-gradient setter intents so the
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

    // issue-965 — Canvas2D path API recording. Each call appends one
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

    // pulp #1521 — native arc subpaths (recorded as DrawCommands so
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
    // pulp #1368 — track save/restore depth so RecordingCanvas can model
    // the same save_count() / restore_to_count() contract as the live
    // backends. This lets CanvasWidget::paint() unit tests assert that the
    // outer save/restore wrapper drops any leftover saves emitted by an
    // unbalanced JS draw script.
    int save_depth_ = 0;
    // pulp #1368 round 2 — track the current device matrix so
    // current_transform() returns a faithful CTM in unit tests. The matrix
    // is saved/restored alongside save_depth_, and translate/scale/rotate/
    // set_transform/concat_transform mutate it. Layout in column-major
    // CanvasRenderingContext2D order: [a, b, c, d, e, f].
    AffineTransform2x3 ctm_{};
    std::vector<AffineTransform2x3> ctm_stack_;
};

} // namespace pulp::canvas
