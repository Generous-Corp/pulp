#pragma once

/// @file svg_line.hpp
/// A View that renders an inline SVG `<line>` element. Targets HTML
/// patterns of the form `<svg><line x1="0" y1="10" x2="100" y2="10"
/// stroke="#000" stroke-width="1" /></svg>` — common for separators,
/// chart axes, and threshold markers in React/HTML bundles.
///
/// Mirrors the SvgPathWidget / SvgRectWidget API (pulp #965 / #1291 /
/// #1416): the same fill / stroke / stroke-width plumbing across all
/// SVG-primitive intrinsics.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

/// Renders an inline SVG `<line x1 y1 x2 y2 stroke stroke-width />`.
/// Endpoints are local to the widget origin (NOT bounds()-translated).
///
/// Default state: zero-length line at (0,0)→(0,0), opaque-black stroke,
/// stroke-width 1. SVG `<line>` has no fill (it's a 1-D primitive); for
/// API consistency with SvgRect / SvgPath we keep the fill setters as
/// no-op-friendly stubs but paint() never fills a line.
class SvgLineWidget : public View {
public:
    SvgLineWidget() = default;

    /// Set the line endpoints. All coords are local to the widget
    /// origin.
    void set_line(float x1, float y1, float x2, float y2);

    /// Solid-stroke paint. Pass an alpha-zero color or call
    /// clear_stroke() to disable stroking. Default: opaque black,
    /// stroke enabled.
    void set_stroke_color(canvas::Color c);
    void clear_stroke();

    /// Stroke width in widget-local units. Default 1.
    void set_stroke_width(float w);

    // Accessors used by tests.
    float x1() const { return x1_; }
    float y1() const { return y1_; }
    float x2() const { return x2_; }
    float y2() const { return y2_; }
    bool has_stroke() const { return has_stroke_; }
    canvas::Color stroke_color() const { return stroke_color_; }
    float stroke_width() const { return stroke_width_; }

    void paint(canvas::Canvas& canvas) override;

private:
    float x1_ = 0.0f;
    float y1_ = 0.0f;
    float x2_ = 0.0f;
    float y2_ = 0.0f;
    canvas::Color stroke_color_{0.0f, 0.0f, 0.0f, 1.0f};
    float stroke_width_ = 1.0f;
    bool has_stroke_ = true;
};

} // namespace pulp::view
