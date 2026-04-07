#include <pulp/view/theme_editor.hpp>

namespace pulp::view {

ThemeEditor::ThemeEditor() {
    set_background_color(canvas::Color::rgba8(30, 30, 40));
}

void ThemeEditor::build_swatches() {
    // Swatches are painted directly — no child views needed for now
}

void ThemeEditor::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Title
    canvas.set_fill_color(canvas::Color::rgba(1.0f, 1.0f, 1.0f));
    canvas.set_font("Inter", 14.0f);
    canvas.set_text_align(canvas::TextAlign::left);
    canvas.fill_text("Theme Editor", 8.0f, 20.0f);

    // Draw color swatches in a grid
    float x = 8.0f;
    float y = 32.0f;
    float max_x = b.width - swatch_size_ - 8.0f;

    for (auto& [token, color] : editing_theme_.colors) {
        // Swatch rect
        canvas.set_fill_color(color);
        canvas.fill_rounded_rect(x, y, swatch_size_, swatch_size_, 4.0f);

        // Selected indicator
        if (token == selected_token_) {
            canvas.set_stroke_color(canvas::Color::rgba(1.0f, 1.0f, 1.0f));
            canvas.set_line_width(2.0f);
            canvas.stroke_rounded_rect(x - 1, y - 1, swatch_size_ + 2, swatch_size_ + 2, 5.0f);
        }

        // Token name below swatch
        canvas.set_fill_color(canvas::Color::rgba(0.7f, 0.7f, 0.7f));
        canvas.set_font("Inter", 9.0f);
        canvas.fill_text(token, x, y + swatch_size_ + 12.0f);

        x += swatch_size_ + swatch_gap_ + 60.0f;  // swatch + label space
        if (x > max_x) {
            x = 8.0f;
            y += swatch_size_ + 20.0f;
        }
    }
}

} // namespace pulp::view
