#include <pulp/view/toolbar.hpp>
#include <pulp/canvas/canvas.hpp>
#include <algorithm>

namespace pulp::view {

void Toolbar::add_button(std::string id, std::string label, std::function<void()> on_click) {
    ToolbarItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.type = ToolbarItemType::Button;
    item.on_click = std::move(on_click);
    items_.push_back(std::move(item));
}

void Toolbar::add_toggle(std::string id, std::string label, std::function<void(bool)> on_toggle) {
    ToolbarItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.type = ToolbarItemType::Toggle;
    item.on_toggle = std::move(on_toggle);
    items_.push_back(std::move(item));
}

// add_separator() is defined in app_framework.cpp

void Toolbar::add_spacer() {
    ToolbarItem item;
    item.type = ToolbarItemType::Spacer;
    items_.push_back(std::move(item));
}

void Toolbar::add_custom(std::string id, std::unique_ptr<View> view) {
    ToolbarItem item;
    item.id = std::move(id);
    item.type = ToolbarItemType::Custom;
    item.custom_view = std::move(view);
    items_.push_back(std::move(item));
}

void Toolbar::remove_item(std::string_view id) {
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
                      [id](const ToolbarItem& item) { return item.id == id; }),
        items_.end());
}

bool Toolbar::is_toggled(std::string_view id) const {
    for (auto& item : items_)
        if (item.id == id) return item.toggled;
    return false;
}

void Toolbar::set_toggled(std::string_view id, bool state) {
    for (auto& item : items_)
        if (item.id == id) { item.toggled = state; return; }
}

void Toolbar::set_enabled(std::string_view id, bool enabled) {
    for (auto& item : items_)
        if (item.id == id) { item.enabled = enabled; return; }
}

void Toolbar::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;

    // Background
    canvas.set_fill_color(canvas::Color::rgba(40, 40, 48));
    canvas.fill_rect(0, 0, w, h);

    bool horiz = (orientation_ == Orientation::horizontal);
    float pos = spacing_;

    for (auto& item : items_) {
        if (item.type == ToolbarItemType::Spacer) {
            // Spacer takes remaining space (simplified: fixed 20px)
            pos += 20.0f;
            continue;
        }

        if (item.type == ToolbarItemType::Separator) {
            canvas.set_stroke_color(canvas::Color::rgba(70, 70, 80));
            canvas.set_line_width(1.0f);
            if (horiz) {
                canvas.stroke_line(pos, 4, pos, h - 4);
                pos += spacing_ * 2;
            } else {
                canvas.stroke_line(4, pos, w - 4, pos);
                pos += spacing_ * 2;
            }
            continue;
        }

        float item_x = horiz ? pos : 0;
        float item_y = horiz ? 0 : pos;

        // Custom view — paint it directly
        if (item.type == ToolbarItemType::Custom && item.custom_view) {
            item.custom_view->set_bounds({item_x, item_y, item_size_, item_size_});
            canvas.save();
            canvas.translate(item_x, item_y);
            item.custom_view->paint(canvas);
            canvas.restore();
            pos += item_size_ + spacing_;
            continue;
        }

        // Button/toggle background
        auto bg_color = item.toggled
            ? canvas::Color::rgba(60, 80, 140)
            : canvas::Color::rgba(55, 55, 65);
        if (!item.enabled) bg_color = canvas::Color::rgba(40, 40, 45);

        if (horiz) {
            canvas.set_fill_color(bg_color);
            canvas.fill_rounded_rect(item_x, 4, item_size_, item_size_, 4.0f);
        } else {
            canvas.set_fill_color(bg_color);
            canvas.fill_rounded_rect(4, item_y, item_size_, item_size_, 4.0f);
        }

        // Label
        auto text_color = item.enabled
            ? canvas::Color::rgba(200, 200, 215)
            : canvas::Color::rgba(100, 100, 110);
        canvas.set_fill_color(text_color);
        canvas.set_font("system", 11.0f);

        // Center the first character of the label as an icon
        std::string display = item.label.empty() ? "" : item.label.substr(0, 2);
        float text_w = canvas.measure_text(display);
        if (horiz) {
            canvas.fill_text(display, item_x + (item_size_ - text_w) / 2.0f,
                           4 + item_size_ * 0.65f);
        } else {
            canvas.fill_text(display, 4 + (item_size_ - text_w) / 2.0f,
                           item_y + item_size_ * 0.65f);
        }

        pos += item_size_ + spacing_;
    }

    // Bottom border
    canvas.set_stroke_color(canvas::Color::rgba(60, 60, 70));
    canvas.set_line_width(1.0f);
    if (horiz)
        canvas.stroke_line(0, h - 1, w, h - 1);
    else
        canvas.stroke_line(w - 1, 0, w - 1, h);
}

int Toolbar::hit_test_item(Point pos) const {
    bool horiz = (orientation_ == Orientation::horizontal);
    float item_pos = spacing_;

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        auto& item = items_[i];
        if (item.type == ToolbarItemType::Spacer) { item_pos += 20.0f; continue; }
        if (item.type == ToolbarItemType::Separator) { item_pos += spacing_ * 2; continue; }

        float coord = horiz ? pos.x : pos.y;
        if (coord >= item_pos && coord < item_pos + item_size_)
            return i;

        item_pos += item_size_ + spacing_;
    }
    return -1;
}

void Toolbar::on_mouse_down(Point pos) {
    int idx = hit_test_item(pos);
    if (idx < 0 || idx >= static_cast<int>(items_.size())) return;

    auto& item = items_[idx];
    if (!item.enabled) return;

    if (item.type == ToolbarItemType::Custom && item.custom_view) {
        // Forward click to custom view
        bool horiz = (orientation_ == Orientation::horizontal);
        float item_start = spacing_;
        for (int i = 0; i < idx; ++i) {
            if (items_[i].type == ToolbarItemType::Spacer) item_start += 20.0f;
            else if (items_[i].type == ToolbarItemType::Separator) item_start += spacing_ * 2;
            else item_start += item_size_ + spacing_;
        }
        Point local = {pos.x - (horiz ? item_start : 0),
                       pos.y - (horiz ? 0 : item_start)};
        item.custom_view->on_mouse_down(local);
    } else if (item.type == ToolbarItemType::Button && item.on_click)
        item.on_click();
    else if (item.type == ToolbarItemType::Toggle) {
        item.toggled = !item.toggled;
        if (item.on_toggle) item.on_toggle(item.toggled);
    }
}

}  // namespace pulp::view
