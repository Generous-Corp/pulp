#include <pulp/view/concertina_panel.hpp>

namespace pulp::view {

void ConcertinaPanel::add_section(std::string title, std::unique_ptr<View> content,
                                   bool initially_expanded) {
    sections_.push_back({std::move(title), std::move(content), initially_expanded});
    if (sections_.back().content)
        add_child(std::move(sections_.back().content));
    // Keep a raw pointer so we can show/hide during layout
    // (the child is now owned by the View tree)
    layout_sections();
}

void ConcertinaPanel::expand(int index) {
    if (index < 0 || index >= section_count()) return;
    if (exclusive_) {
        for (auto& s : sections_) s.expanded = false;
    }
    sections_[static_cast<size_t>(index)].expanded = true;
    layout_sections();
}

void ConcertinaPanel::collapse(int index) {
    if (index < 0 || index >= section_count()) return;
    sections_[static_cast<size_t>(index)].expanded = false;
    layout_sections();
}

void ConcertinaPanel::toggle(int index) {
    if (index < 0 || index >= section_count()) return;
    if (sections_[static_cast<size_t>(index)].expanded)
        collapse(index);
    else
        expand(index);
}

bool ConcertinaPanel::is_expanded(int index) const {
    if (index < 0 || index >= section_count()) return false;
    return sections_[static_cast<size_t>(index)].expanded;
}

void ConcertinaPanel::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float y = b.y;

    auto header_bg = resolve_color("control.background", Color::rgba(50, 50, 60));
    auto header_text = resolve_color("text.primary", Color::rgba(220, 220, 230));
    auto border = resolve_color("control.border", Color::rgba(80, 80, 90));

    for (int i = 0; i < section_count(); ++i) {
        // Draw header background
        canvas.set_fill_color(header_bg);
        canvas.fill_rect(b.x, y, b.width, header_height_);

        // Draw header text
        canvas.set_fill_color(header_text);
        canvas.set_font("system", 13.0f);
        canvas.fill_text(sections_[static_cast<size_t>(i)].title, b.x + 8.0f, y + header_height_ * 0.7f);

        // Draw header border
        canvas.set_stroke_color(border);
        canvas.set_line_width(1.0f);
        canvas.stroke_rect(b.x, y, b.width, header_height_);

        y += header_height_;

        // Skip content area if expanded (content view paints itself)
        if (sections_[static_cast<size_t>(i)].expanded) {
            auto* child = child_at(static_cast<size_t>(i));
            if (child) y += child->bounds().height;
        }
    }
}

void ConcertinaPanel::on_mouse_down(Point pos) {
    // Determine which header was clicked
    float y = 0;
    for (int i = 0; i < section_count(); ++i) {
        if (pos.y >= y && pos.y < y + header_height_) {
            toggle(i);
            return;
        }
        y += header_height_;
        if (sections_[static_cast<size_t>(i)].expanded) {
            auto* child = child_at(static_cast<size_t>(i));
            if (child) y += child->bounds().height;
        }
    }
}

void ConcertinaPanel::layout_sections() {
    auto b = local_bounds();
    float y = b.y;

    for (size_t i = 0; i < sections_.size(); ++i) {
        y += header_height_;
        auto* child = (i < child_count()) ? child_at(i) : nullptr;
        if (child) {
            if (sections_[i].expanded) {
                float content_h = child->intrinsic_height();
                if (content_h <= 0) content_h = 100.0f; // default height
                child->set_bounds({b.x, y, b.width, content_h});
                child->set_visible(true);
                y += content_h;
            } else {
                child->set_bounds({b.x, y, b.width, 0});
                child->set_visible(false);
            }
        }
    }
}

} // namespace pulp::view
