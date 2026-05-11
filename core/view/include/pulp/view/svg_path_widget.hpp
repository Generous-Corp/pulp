#pragma once

/// @file svg_path_widget.hpp
/// A View that renders an inline SVG path-data string. Targets HTML
/// patterns of the form `<svg viewBox="0 0 24 24"><path d="M..."/></svg>`
/// — common for icon glyphs in React/HTML bundles. The widget parses
/// the path data once on set_path() into a flat list of Canvas2D path
/// segments, then replays them in paint() at the widget's bounds, scaled
/// from the source viewBox.

#include <pulp/view/view.hpp>
#include <pulp/canvas/canvas.hpp>
#include <string>
#include <vector>

namespace pulp::view {

/// Parsed path segment — flat, allocation-light, ready to replay onto
/// a Canvas in a single pass during paint().
struct SvgPathSegment {
    enum class Op {
        move_to,    ///< (x, y)
        line_to,    ///< (x, y)
        quad_to,    ///< (cpx, cpy, x, y)
        cubic_to,   ///< (cp1x, cp1y, cp2x, cp2y, x, y)
        close_path
    };
    Op op = Op::move_to;
    float p[6] = {};
};

/// Renders an inline SVG `<path d="...">` icon. Supports the path-data
/// commands that occur in icon glyphs: M/m, L/l, H/h, V/v, C/c, S/s,
/// Q/q, T/t, Z/z. Elliptical arcs (A/a) are converted to a chain of
/// cubic-bezier approximations. Default fill is black, no stroke.
class SvgPathWidget : public View {
public:
    SvgPathWidget() = default;

    /// Set the path-data string. Re-parses immediately. Empty string
    /// clears the widget.
    void set_path(std::string data);

    /// Set the source coordinate space the path was authored in. The
    /// widget scales path coords from this box to its own bounds with
    /// preserved aspect ratio (xMidYMid meet — the SVG default). When
    /// width or height is <= 0, the widget falls back to its bounds
    /// 1:1, which matches HTML default for path data with no viewBox.
    void set_viewbox(float width, float height);

    /// Solid-fill paint. Pass an alpha-zero color or call clear_fill()
    /// to disable filling. Default: opaque black, fill enabled.
    void set_fill_color(canvas::Color c);
    void clear_fill();

    /// pulp #932 / #1737 PR-4 — gradient fill as a CSS linear-gradient
    /// string. When set (non-empty), `paint()` overrides the solid
    /// fill_color_ with the parsed gradient — calls
    /// `canvas.set_fill_gradient_linear(...)` before
    /// `fill_current_path()`. Wins over `fill_color_` while non-empty;
    /// `clear_fill_gradient()` returns to solid fill semantics.
    ///
    /// The string MUST be a CSS linear-gradient — e.g.
    /// `"linear-gradient(to bottom, #ff0000, #0000ff)"`. Other
    /// gradient kinds (radial, conic) are followups; unparseable input
    /// silently falls back to `fill_color_` so authors don't lose
    /// their content to a parser error.
    ///
    /// Wired via the `setSvgFillGradient` bridge fn. The intended
    /// JSX consumer is `<SvgLinearGradient id="g1" stops={...}>` +
    /// `fill="url(#g1)"`; the prop-applier resolves the JSX subtree's
    /// gradient definitions into linear-gradient strings before calling
    /// the bridge. Direct C++ consumers can also call this with a
    /// hand-built gradient string.
    void set_fill_gradient(std::string css_linear_gradient);
    void clear_fill_gradient();
    const std::string& fill_gradient() const { return fill_gradient_; }

    /// Solid-stroke paint. Pass an alpha-zero color or call
    /// clear_stroke() to disable stroking. Default: no stroke.
    void set_stroke_color(canvas::Color c);
    void clear_stroke();

    /// Stroke width in SVG path-coords (i.e. viewBox space). Default 1.
    void set_stroke_width(float w);

    // Accessors used by tests.
    const std::string& path_data() const { return path_data_; }
    const std::vector<SvgPathSegment>& segments() const { return segments_; }
    bool has_fill() const { return has_fill_; }
    bool has_stroke() const { return has_stroke_; }
    canvas::Color fill_color() const { return fill_color_; }
    canvas::Color stroke_color() const { return stroke_color_; }
    float stroke_width() const { return stroke_width_; }
    float viewbox_width() const { return viewbox_w_; }
    float viewbox_height() const { return viewbox_h_; }

    void paint(canvas::Canvas& canvas) override;

private:
    void reparse();

    std::string path_data_;
    std::vector<SvgPathSegment> segments_;
    canvas::Color fill_color_{0.0f, 0.0f, 0.0f, 1.0f};
    canvas::Color stroke_color_{0.0f, 0.0f, 0.0f, 1.0f};
    std::string   fill_gradient_;  // pulp #932 — non-empty overrides solid fill
    float stroke_width_ = 1.0f;
    float viewbox_w_ = 0.0f;   // 0 means "use widget bounds 1:1"
    float viewbox_h_ = 0.0f;
    bool has_fill_ = true;
    bool has_stroke_ = false;
};

} // namespace pulp::view
