#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <functional>

namespace pulp::canvas {

// ── Color ────────────────────────────────────────────────────────────────────

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    static Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return {r, g, b, a};
    }
    static Color hex(uint32_t rgb) {
        return {static_cast<uint8_t>((rgb >> 16) & 0xFF),
                static_cast<uint8_t>((rgb >> 8) & 0xFF),
                static_cast<uint8_t>(rgb & 0xFF), 255};
    }

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
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
    std::vector<GradientStop> stops;
};

using Paint = std::variant<Color, LinearGradient, RadialGradient>;

// ── Drawing commands ─────────────────────────────────────────────────────────

enum class LineCap { butt, round, square };
enum class LineJoin { miter, round, bevel };
enum class TextAlign { left, center, right };
enum class TextBaseline { top, middle, bottom };

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

    // ── Clipping ─────────────────────────────────────────────────────────
    virtual void clip_rect(float x, float y, float w, float h) = 0;

    // ── Fill and stroke style ────────────────────────────────────────────
    virtual void set_fill_color(Color c) = 0;
    virtual void set_stroke_color(Color c) = 0;
    virtual void set_line_width(float w) = 0;
    virtual void set_line_cap(LineCap cap) = 0;
    virtual void set_line_join(LineJoin join) = 0;

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

    // ── Text ─────────────────────────────────────────────────────────────
    virtual void set_font(const std::string& family, float size) = 0;
    virtual void set_text_align(TextAlign align) = 0;
    virtual void fill_text(const std::string& text, float x, float y) = 0;
    virtual float measure_text(const std::string& text) = 0;
};

// ── Recording canvas ─────────────────────────────────────────────────────────
// Captures draw commands for inspection/testing
// Does not actually render anything

struct DrawCommand {
    enum class Type {
        save, restore,
        translate, scale, rotate, clip_rect,
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
    void set_font(const std::string& family, float size) override;
    void set_text_align(TextAlign align) override;
    void fill_text(const std::string& text, float x, float y) override;
    float measure_text(const std::string& text) override;

private:
    std::vector<DrawCommand> commands_;
};

} // namespace pulp::canvas
