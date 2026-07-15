#include <pulp/view/table.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>

namespace pulp::view {

void TableListBox::paint(canvas::Canvas& canvas) {
    if (!model_ || columns_.empty()) return;

    float w = bounds().width;
    float total_w = 0;
    for (auto& col : columns_) total_w += col.width;

    // Scale columns to fit if they exceed the view width
    float scale = (total_w > w && total_w > 0) ? w / total_w : 1.0f;

    // Theme tokens (with dark-mode fallbacks). Resolved once so a token/theme
    // swap restyles the whole table — and, critically, so the fills use rgba8
    // (0–255) not rgba (0–1), which the original code clamped to solid white.
    const auto c_header   = resolve_color("bg.elevated",   canvas::Color::rgba8(45, 45, 55));
    const auto c_headtext = resolve_color("text.secondary", canvas::Color::rgba8(180, 180, 195));
    const auto c_accent   = resolve_color("accent.primary", canvas::Color::rgba8(120, 150, 255));
    const auto c_border   = resolve_color("control.border", canvas::Color::rgba8(70, 70, 80));
    const auto c_sel      = resolve_color("selection.bg",   canvas::Color::rgba8(60, 80, 140));
    const auto c_row_even = resolve_color("bg.primary",     canvas::Color::rgba8(35, 35, 42));
    const auto c_row_odd  = resolve_color("bg.surface",     canvas::Color::rgba8(40, 40, 48));
    const auto c_celltext = resolve_color("text.primary",   canvas::Color::rgba8(210, 210, 220));

    // ── Draw header ─────────────────────────────────────────────────
    canvas.set_fill_color(c_header);
    canvas.fill_rect(0, 0, w, header_height_);

    float col_x = 0;
    for (int c = 0; c < column_count(); ++c) {
        float col_w = columns_[c].width * scale;

        // Header text
        canvas.set_fill_color(c_headtext);
        canvas.set_font("system", 12.0f);

        float text_x = col_x + 6.0f;
        if (columns_[c].align == TableColumn::Align::center)
            text_x = col_x + (col_w - canvas.measure_text(columns_[c].header)) / 2.0f;
        else if (columns_[c].align == TableColumn::Align::right)
            text_x = col_x + col_w - canvas.measure_text(columns_[c].header) - 6.0f;

        canvas.fill_text(columns_[c].header, text_x, header_height_ * 0.7f);

        // Sort indicator — anchored on the header's vertical midline via
        // GlyphCenter so the arrow lines up with the header label instead of
        // sitting high (the old baseline+4 offset placed the glyph above
        // center).
        if (c == sort_column_ && columns_[c].sortable) {
            float arrow_x = col_x + col_w - 14.0f;
            canvas.set_fill_color(c_accent);
            const char* glyph = sort_ascending_ ? "\xe2\x96\xb2"   // ▲
                                                : "\xe2\x96\xbc";  // ▼
            canvas.fill_text_anchored(glyph, arrow_x, header_height_ * 0.5f,
                                      canvas::Canvas::TextAnchor::GlyphCenter);
        }

        // Column separator
        canvas.set_stroke_color(c_border);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(col_x + col_w, 0, col_x + col_w, header_height_);

        col_x += col_w;
    }

    // Header bottom border
    canvas.set_stroke_color(c_border);
    canvas.stroke_line(0, header_height_, w, header_height_);

    // ── Draw rows ───────────────────────────────────────────────────
    int row_count = model_->row_count();
    int first_visible = static_cast<int>(scroll_offset_ / row_height_);
    float visible_height = bounds().height - header_height_;
    int visible_rows = static_cast<int>(visible_height / row_height_) + 2;

    for (int r = first_visible; r < std::min(row_count, first_visible + visible_rows); ++r) {
        float row_y = header_height_ + (r - first_visible) * row_height_
                      - std::fmod(scroll_offset_, row_height_);

        if (row_y + row_height_ < header_height_ || row_y > bounds().height)
            continue;

        // Row background
        if (r == selected_row_) {
            canvas.set_fill_color(c_sel);
        } else if (r % 2 == 0) {
            canvas.set_fill_color(c_row_even);
        } else {
            canvas.set_fill_color(c_row_odd);
        }
        canvas.fill_rect(0, row_y, w, row_height_);

        // Cell text
        col_x = 0;
        canvas.set_fill_color(c_celltext);
        canvas.set_font("system", 13.0f);

        for (int c = 0; c < column_count(); ++c) {
            float col_w = columns_[c].width * scale;
            std::string text = model_->cell_text(r, c);

            float text_x = col_x + 6.0f;
            if (columns_[c].align == TableColumn::Align::center)
                text_x = col_x + (col_w - canvas.measure_text(text)) / 2.0f;
            else if (columns_[c].align == TableColumn::Align::right)
                text_x = col_x + col_w - canvas.measure_text(text) - 6.0f;

            canvas.fill_text(text, text_x, row_y + row_height_ * 0.7f);
            col_x += col_w;
        }
    }
}

void TableListBox::on_mouse_down(Point pos) {
    float header_h = header_height_;

    if (pos.x < 0.0f || pos.y < 0.0f || pos.x >= bounds().width ||
        pos.y >= bounds().height) {
        return;
    }

    if (pos.y < header_h) {
        // Click on header — sort by column
        float col_x = 0;
        float w = bounds().width;
        float total_w = 0;
        for (auto& col : columns_) total_w += col.width;
        float scale = (total_w > w && total_w > 0) ? w / total_w : 1.0f;

        for (int c = 0; c < column_count(); ++c) {
            float col_w = columns_[c].width * scale;
            if (pos.x >= col_x && pos.x < col_x + col_w && columns_[c].sortable) {
                if (c == sort_column_)
                    sort_ascending_ = !sort_ascending_;
                else {
                    sort_column_ = c;
                    sort_ascending_ = true;
                }
                if (model_) model_->sort(sort_column_, sort_ascending_);
                return;
            }
            col_x += col_w;
        }
        return;
    }

    // Click on row — select
    int row = static_cast<int>((pos.y - header_h + scroll_offset_) / row_height_);
    if (model_ && row >= 0 && row < model_->row_count()) {
        selected_row_ = row;
        if (on_selection_changed) on_selection_changed(row);
        if (model_) model_->row_selected(row);
    }
}

}  // namespace pulp::view
