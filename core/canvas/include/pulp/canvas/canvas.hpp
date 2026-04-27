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
enum class TextAlign { left, center, right };
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

    // ── Clipping ─────────────────────────────────────────────────────────
    virtual void clip_rect(float x, float y, float w, float h) = 0;

    /// Intersect the current clip region with the current path.
    /// Mirrors CanvasRenderingContext2D.clip(). Default no-op so
    /// backends without a path builder remain unaffected.
    virtual void clip() {}

    // ── Fill and stroke style ────────────────────────────────────────────
    virtual void set_fill_color(Color c) = 0;
    virtual void set_stroke_color(Color c) = 0;
    virtual void set_line_width(float w) = 0;
    virtual void set_line_cap(LineCap cap) = 0;
    virtual void set_line_join(LineJoin join) = 0;

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

    /// Set a conic (sweep) gradient as the fill paint.
    virtual void set_fill_gradient_conic(float cx, float cy, float start_angle,
                                          const Color* colors, const float* positions,
                                          int count) {
        if (count > 0) set_fill_color(colors[0]); // fallback
    }

    /// Clear gradient, return to solid fill color.
    virtual void clear_fill_gradient() {}

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
    /// Close the current path subpath.
    virtual void close_path() {}
    /// Fill the current path.
    virtual void fill_current_path() {}
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

    // ── Text ─────────────────────────────────────────────────────────────
    virtual void set_font(const std::string& family, float size) = 0;
    virtual void set_text_align(TextAlign align) = 0;
    virtual void fill_text(const std::string& text, float x, float y) = 0;
    virtual float measure_text(const std::string& text) = 0;

    /// Full text metrics for layout and intrinsic sizing.
    struct TextMetrics {
        float width = 0;        ///< Advance width of the text
        float ascent = 0;       ///< Distance above baseline (positive value)
        float descent = 0;      ///< Distance below baseline (positive value)
        float line_height = 0;  ///< Recommended line spacing (ascent + descent + leading)
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

    /// Measure text with full font metrics using current font settings.
    virtual TextMetrics measure_text_full(const std::string& text) {
        TextMetrics m;
        m.width = measure_text(text);
        m.ascent = font_size_ * 0.75f;   // approximate
        m.descent = font_size_ * 0.25f;
        m.line_height = font_size_ * 1.2f;
        return m;
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
        set_fill_color, set_stroke_color, set_line_width,
        set_line_cap, set_line_join,
        fill_rect, stroke_rect, fill_rounded_rect, stroke_rounded_rect,
        fill_circle, stroke_circle, stroke_arc, stroke_line,
        set_font, set_text_align, fill_text
    };

    Type type;
    // Generic storage for command parameters
    float f[6] = {};
    Color color{};
    std::string text;
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
    void translate(float x, float y) override;
    void scale(float sx, float sy) override;
    void rotate(float radians) override;
    void set_transform(float a, float b, float c,
                       float d, float e, float f) override;
    void capture_paint_baseline_transform() override;
    void clip_rect(float x, float y, float w, float h) override;
    void clip() override;
    void set_blend_mode(BlendMode mode) override;
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
    void set_font(const std::string& family, float size) override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    float measure_text(const std::string& text) override;

private:
    std::vector<DrawCommand> commands_;
    size_t baseline_capture_count_ = 0;
};

} // namespace pulp::canvas
