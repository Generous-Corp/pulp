#pragma once

/// @file svg_rect.hpp
/// A View that renders an inline SVG `<rect>` element. Targets HTML
/// patterns of the form `<svg><rect x="0" y="0" width="100" height="20"
/// fill="#f00" /></svg>` — common for band-shape thumbnails and other
/// chart-style SVG primitives in React/HTML bundles.
///
/// Mirrors the SvgPathWidget API (pulp #965 / #994 / #1291): fill +
/// stroke + stroke-width plumbing kept consistent so JS/JSX consumers
/// see the same surface across the SVG primitives.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

/// Renders an inline SVG `<rect x y width height fill stroke
/// stroke-width />`. The geometry is local to the widget origin (the
/// widget's `bounds_` translation is applied by the View::paint_all
/// machinery; paint() draws in local space).
///
/// Default state: zero-sized rect, opaque-black fill, no stroke. Setting
/// fill / stroke independently lets consumers express the SVG defaults
/// `fill="black" stroke="none"` and the common Spectr pattern of
/// `fill="#xxx" stroke="none"` for solid bars.
class SvgRectWidget : public View {
public:
    SvgRectWidget() = default;

    /// Set the rect geometry. x/y are local to the widget origin (NOT
    /// bounds()-translated). width/height < 0 are clamped to 0.
    void set_rect(float x, float y, float width, float height);

    /// Solid-fill paint. Pass an alpha-zero color or call clear_fill()
    /// to disable filling. Default: opaque black, fill enabled.
    void set_fill_color(canvas::Color c);
    void clear_fill();

    /// Solid-stroke paint. Pass an alpha-zero color or call
    /// clear_stroke() to disable stroking. Default: no stroke.
    void set_stroke_color(canvas::Color c);
    void clear_stroke();

    /// Stroke width in widget-local units. Default 1.
    void set_stroke_width(float w);

    // Accessors used by tests.
    float rect_x() const { return x_; }
    float rect_y() const { return y_; }
    float rect_width() const { return w_; }
    float rect_height() const { return h_; }
    bool has_fill() const { return has_fill_; }
    bool has_stroke() const { return has_stroke_; }
    canvas::Color fill_color() const { return fill_color_; }
    canvas::Color stroke_color() const { return stroke_color_; }
    float stroke_width() const { return stroke_width_; }

    void paint(canvas::Canvas& canvas) override;

private:
    float x_ = 0.0f;
    float y_ = 0.0f;
    float w_ = 0.0f;
    float h_ = 0.0f;
    canvas::Color fill_color_{0.0f, 0.0f, 0.0f, 1.0f};
    canvas::Color stroke_color_{0.0f, 0.0f, 0.0f, 1.0f};
    float stroke_width_ = 1.0f;
    bool has_fill_ = true;
    bool has_stroke_ = false;
};

} // namespace pulp::view
