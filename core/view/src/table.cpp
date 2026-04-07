#include <pulp/view/table.hpp>

namespace pulp::view {

void TableListBox::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    auto header_bg = resolve_color("control.background", Color::rgba(45, 45, 55));
    auto header_text = resolve_color("text.primary", Color::rgba(200, 200, 210));
    auto row_bg = resolve_color("surface.background", Color::rgba(30, 30, 40));
    auto row_alt = resolve_color("surface.alt", Color::rgba(35, 35, 45));
    auto sel_bg = resolve_color("accent.background", Color::rgba(60, 100, 180));
    auto cell_text = resolve_color("text.primary", Color::rgba(220, 220, 230));
    auto border = resolve_color("control.border", Color::rgba(60, 60, 70));

    // Draw header row
    canvas.set_fill_color(header_bg);
    canvas.fill_rect(b.x, b.y, b.width, header_height_);

    canvas.set_fill_color(header_text);
    canvas.set_font("system", 12.0f);
    float col_x = b.x;
    for (int c = 0; c < column_count(); ++c) {
        auto& col = columns_[static_cast<size_t>(c)];
        canvas.fill_text(col.header, col_x + 6.0f, b.y + header_height_ * 0.7f);
        col_x += col.width;
    }

    // Header border
    canvas.set_stroke_color(border);
    canvas.set_line_width(1.0f);
    canvas.stroke_rect(b.x, b.y, b.width, header_height_);

    if (!model_) return;

    // Draw rows
    int row_count = model_->row_count();
    float y = b.y + header_height_ - scroll_offset_;
    for (int r = 0; r < row_count; ++r) {
        if (y + row_height_ < b.y + header_height_) { y += row_height_; continue; }
        if (y > b.y + b.height) break;

        // Row background
        if (r == selected_row_)
            canvas.set_fill_color(sel_bg);
        else
            canvas.set_fill_color(r % 2 == 0 ? row_bg : row_alt);
        canvas.fill_rect(b.x, y, b.width, row_height_);

        // Cell text
        canvas.set_fill_color(cell_text);
        canvas.set_font("system", 12.0f);
        col_x = b.x;
        for (int c = 0; c < column_count(); ++c) {
            auto text = model_->cell_text(r, c);
            canvas.fill_text(text, col_x + 6.0f, y + row_height_ * 0.7f);
            col_x += columns_[static_cast<size_t>(c)].width;
        }

        y += row_height_;
    }
}

void TableListBox::on_mouse_down(Point pos) {
    auto b = local_bounds();

    // Click in header — sort
    if (pos.y >= b.y && pos.y < b.y + header_height_) {
        float col_x = b.x;
        for (int c = 0; c < column_count(); ++c) {
            float w = columns_[static_cast<size_t>(c)].width;
            if (pos.x >= col_x && pos.x < col_x + w) {
                if (columns_[static_cast<size_t>(c)].sortable && model_) {
                    if (sort_column_ == c)
                        sort_ascending_ = !sort_ascending_;
                    else {
                        sort_column_ = c;
                        sort_ascending_ = true;
                    }
                    model_->sort(sort_column_, sort_ascending_);
                }
                return;
            }
            col_x += w;
        }
        return;
    }

    // Click in rows — select
    if (!model_) return;
    float row_y = b.y + header_height_ - scroll_offset_;
    int row = static_cast<int>((pos.y - row_y) / row_height_);
    if (row >= 0 && row < model_->row_count()) {
        selected_row_ = row;
        model_->row_selected(row);
        if (on_selection_changed) on_selection_changed(row);
    }
}

} // namespace pulp::view
