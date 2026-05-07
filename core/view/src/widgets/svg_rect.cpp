#include <pulp/view/widgets/svg_rect.hpp>

#include <algorithm>

namespace pulp::view {

void SvgRectWidget::set_rect(float x, float y, float width, float height) {
    x_ = x;
    y_ = y;
    w_ = std::max(0.0f, width);
    h_ = std::max(0.0f, height);
}

void SvgRectWidget::set_fill_color(canvas::Color c) {
    fill_color_ = c;
    has_fill_ = true;
}

void SvgRectWidget::clear_fill() {
    has_fill_ = false;
}

void SvgRectWidget::set_stroke_color(canvas::Color c) {
    stroke_color_ = c;
    has_stroke_ = true;
}

void SvgRectWidget::clear_stroke() {
    has_stroke_ = false;
}

void SvgRectWidget::set_stroke_width(float w) {
    stroke_width_ = std::max(0.0f, w);
}

void SvgRectWidget::paint(canvas::Canvas& canvas) {
    if (w_ <= 0.0f || h_ <= 0.0f) return;
    if (!has_fill_ && !(has_stroke_ && stroke_width_ > 0.0f)) return;

    if (has_fill_) {
        canvas.set_fill_color(fill_color_);
        canvas.fill_rect(x_, y_, w_, h_);
    }
    if (has_stroke_ && stroke_width_ > 0.0f) {
        canvas.set_stroke_color(stroke_color_);
        canvas.set_line_width(stroke_width_);
        canvas.stroke_rect(x_, y_, w_, h_);
    }
}

} // namespace pulp::view
