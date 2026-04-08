#include <pulp/view/concertina_panel.hpp>
#include <pulp/canvas/canvas.hpp>

namespace pulp::view {

void ConcertinaPanel::add_section(std::string title, std::unique_ptr<View> content,
                                   bool initially_expanded) {
    Section section;
    section.title = std::move(title);
    section.expanded = initially_expanded;

    if (content) {
        content->set_visible(initially_expanded);
        section.content = std::move(content);
    }

    sections_.push_back(std::move(section));
}

void ConcertinaPanel::expand(int index) {
    if (index < 0 || index >= section_count()) return;
    if (exclusive_) {
        for (auto& s : sections_) {
            s.expanded = false;
            if (s.content) s.content->set_visible(false);
        }
    }
    sections_[index].expanded = true;
    if (sections_[index].content) sections_[index].content->set_visible(true);
}

void ConcertinaPanel::collapse(int index) {
    if (index < 0 || index >= section_count()) return;
    sections_[index].expanded = false;
    if (sections_[index].content) sections_[index].content->set_visible(false);
}

void ConcertinaPanel::toggle(int index) {
    if (index < 0 || index >= section_count()) return;
    if (sections_[index].expanded)
        collapse(index);
    else
        expand(index);
}

bool ConcertinaPanel::is_expanded(int index) const {
    if (index < 0 || index >= section_count()) return false;
    return sections_[index].expanded;
}

void ConcertinaPanel::paint(canvas::Canvas& canvas) {
    float w = bounds().width;
    float y = 0;

    for (int i = 0; i < section_count(); ++i) {
        auto& section = sections_[i];

        // Header background
        auto bg = canvas::Color::rgba(50, 50, 60);
        canvas.set_fill_color(bg);
        canvas.fill_rect(0, y, w, header_height_);

        // Expand/collapse indicator
        canvas.set_fill_color(canvas::Color::rgba(160, 160, 175));
        canvas.set_font("system", 12.0f);
        canvas.fill_text(section.expanded ? "\xe2\x96\xbc" : "\xe2\x96\xb6",
                        8.0f, y + header_height_ * 0.7f);  // ▼ or ▶

        // Title
        canvas.set_fill_color(canvas::Color::rgba(210, 210, 225));
        canvas.set_font("system", 13.0f);
        canvas.fill_text(section.title, 24.0f, y + header_height_ * 0.7f);

        // Header bottom border
        canvas.set_stroke_color(canvas::Color::rgba(65, 65, 75));
        canvas.set_line_width(1.0f);
        canvas.stroke_line(0, y + header_height_, w, y + header_height_);

        y += header_height_;

        // Content area (if expanded)
        if (section.expanded && section.content) {
            float content_h = section.content->intrinsic_height();
            if (content_h <= 0) content_h = 100.0f;  // Default content height

            // Content background
            canvas.set_fill_color(canvas::Color::rgba(35, 35, 42));
            canvas.fill_rect(0, y, w, content_h);

            // Paint the content view with translated origin
            section.content->set_bounds({0, y, w, content_h});
            canvas.save();
            canvas.translate(0, y);
            section.content->paint(canvas);
            canvas.restore();

            // The content view would be laid out here via set_bounds
            // In a real layout pass, we'd call section.content->set_bounds({0, y, w, content_h})

            y += content_h;
        }
    }
}

void ConcertinaPanel::on_mouse_down(Point pos) {
    float y = 0;
    for (int i = 0; i < section_count(); ++i) {
        if (pos.y >= y && pos.y < y + header_height_) {
            toggle(i);
            return;
        }
        y += header_height_;
        if (sections_[i].expanded) {
            float content_h = sections_[i].content ? sections_[i].content->intrinsic_height() : 100.0f;
            if (content_h <= 0) content_h = 100.0f;
            y += content_h;
        }
    }
}

void ConcertinaPanel::layout_sections() {
    float w = bounds().width;
    float y = 0;
    for (auto& section : sections_) {
        y += header_height_;
        if (section.expanded && section.content) {
            float content_h = section.content->intrinsic_height();
            if (content_h <= 0) content_h = 100.0f;
            section.content->set_bounds({0, y, w, content_h});
            y += content_h;
        }
    }
}

}  // namespace pulp::view
