#include <pulp/view/widgets/svg_line.hpp>

#include <algorithm>

namespace pulp::view {

void SvgLineWidget::set_line(float x1, float y1, float x2, float y2) {
    x1_ = x1;
    y1_ = y1;
    x2_ = x2;
    y2_ = y2;
}

void SvgLineWidget::set_stroke_color(canvas::Color c) {
    stroke_color_ = c;
    has_stroke_ = true;
}

void SvgLineWidget::clear_stroke() {
    has_stroke_ = false;
}

void SvgLineWidget::set_stroke_width(float w) {
    stroke_width_ = std::max(0.0f, w);
}

void SvgLineWidget::paint(canvas::Canvas& canvas) {
    if (!has_stroke_ || stroke_width_ <= 0.0f) return;
    canvas.set_stroke_color(stroke_color_);
    canvas.set_line_width(stroke_width_);
    canvas.stroke_line(x1_, y1_, x2_, y2_);
}

} // namespace pulp::view
