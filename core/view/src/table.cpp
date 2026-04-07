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

    // ── Draw header ─────────────────────────────────────────────────
    canvas.set_fill_color(canvas::Color::rgba(45, 45, 55));
    canvas.fill_rect(0, 0, w, header_height_);

    float col_x = 0;
    for (int c = 0; c < column_count(); ++c) {
        float col_w = columns_[c].width * scale;

        // Header text
        canvas.set_fill_color(canvas::Color::rgba(180, 180, 195));
        canvas.set_font("system", 12.0f);

        float text_x = col_x + 6.0f;
        if (columns_[c].align == TableColumn::Align::center)
            text_x = col_x + (col_w - canvas.measure_text(columns_[c].header)) / 2.0f;
        else if (columns_[c].align == TableColumn::Align::right)
            text_x = col_x + col_w - canvas.measure_text(columns_[c].header) - 6.0f;

        canvas.fill_text(columns_[c].header, text_x, header_height_ * 0.7f);

        // Sort indicator
        if (c == sort_column_ && columns_[c].sortable) {
            float arrow_x = col_x + col_w - 14.0f;
            float arrow_y = header_height_ * 0.5f;
            canvas.set_fill_color(canvas::Color::rgba(120, 150, 255));
            if (sort_ascending_)
                canvas.fill_text("\xe2\x96\xb2", arrow_x, arrow_y + 4.0f);  // ▲
            else
                canvas.fill_text("\xe2\x96\xbc", arrow_x, arrow_y + 4.0f);  // ▼
        }

        // Column separator
        canvas.set_stroke_color(canvas::Color::rgba(60, 60, 70));
        canvas.set_line_width(1.0f);
        canvas.stroke_line(col_x + col_w, 0, col_x + col_w, header_height_);

        col_x += col_w;
    }

    // Header bottom border
    canvas.set_stroke_color(canvas::Color::rgba(70, 70, 80));
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
            canvas.set_fill_color(canvas::Color::rgba(60, 80, 140));
        } else if (r % 2 == 0) {
            canvas.set_fill_color(canvas::Color::rgba(35, 35, 42));
        } else {
            canvas.set_fill_color(canvas::Color::rgba(40, 40, 48));
        }
        canvas.fill_rect(0, row_y, w, row_height_);

        // Cell text
        col_x = 0;
        canvas.set_fill_color(canvas::Color::rgba(210, 210, 220));
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
