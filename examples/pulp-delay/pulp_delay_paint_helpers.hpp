#pragma once

#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <string>

namespace pulp::examples::delay::ui::paint_detail {

inline void text(canvas::Canvas& canvas, const std::string& value, float x, float baseline,
                 float size, canvas::Color colour,
                 canvas::TextAlign align = canvas::TextAlign::left, int weight = 500,
                 float spacing = 0.0f) {
    canvas.set_font_full("Roboto Mono", size, weight, 0, spacing);
    canvas.set_text_align(align);
    canvas.set_fill_color(colour);
    canvas.fill_text(value, x, baseline);
}

inline canvas::Color with_alpha(canvas::Color value, float alpha) {
    value.a = std::clamp(alpha, 0.0f, 1.0f);
    return value;
}

} // namespace pulp::examples::delay::ui::paint_detail
