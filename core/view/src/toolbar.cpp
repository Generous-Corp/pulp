#include <pulp/view/toolbar.hpp>
#include <algorithm>

namespace pulp::view {

void ToolStrip::add_button(std::string id, std::string label,
                            std::function<void()> on_click) {
    ToolbarItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.type = ToolStripItemType::Button;
    item.on_click = std::move(on_click);
    items_.push_back(std::move(item));
}

void ToolStrip::add_toggle(std::string id, std::string label,
                             std::function<void(bool)> on_toggle) {
    ToolbarItem item;
    item.id = std::move(id);
    item.label = std::move(label);
    item.type = ToolStripItemType::Toggle;
    item.on_toggle = std::move(on_toggle);
    items_.push_back(std::move(item));
}

void ToolStrip::add_separator() {
    ToolbarItem item;
    item.type = ToolStripItemType::Separator;
    items_.push_back(std::move(item));
}

void ToolStrip::add_spacer() {
    ToolbarItem item;
    item.type = ToolStripItemType::Spacer;
    items_.push_back(std::move(item));
}

void ToolStrip::add_custom(std::string id, std::unique_ptr<View> view) {
    ToolbarItem item;
    item.id = std::move(id);
    item.type = ToolStripItemType::Custom;
    item.custom_view = std::move(view);
    items_.push_back(std::move(item));
}

void ToolStrip::remove_item(std::string_view id) {
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
                        [&](const ToolbarItem& item) { return item.id == id; }),
        items_.end());
}

bool ToolStrip::is_toggled(std::string_view id) const {
    for (auto& item : items_)
        if (item.id == id) return item.toggled;
    return false;
}

void ToolStrip::set_toggled(std::string_view id, bool state) {
    for (auto& item : items_) {
        if (item.id == id) {
            item.toggled = state;
            return;
        }
    }
}

void ToolStrip::set_enabled(std::string_view id, bool enabled) {
    for (auto& item : items_) {
        if (item.id == id) {
            item.enabled = enabled;
            return;
        }
    }
}

void ToolStrip::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    auto bg = resolve_color("toolbar.background", Color::rgba(40, 40, 50));
    canvas.set_fill_color(bg);
    canvas.fill_rect(b.x, b.y, b.width, b.height);

    auto btn_bg = resolve_color("control.background", Color::rgba(55, 55, 65));
    auto btn_text = resolve_color("text.primary", Color::rgba(220, 220, 230));
    auto toggled_bg = resolve_color("accent.background", Color::rgba(60, 100, 180));
    auto disabled_text = resolve_color("text.disabled", Color::rgba(100, 100, 110));
    auto sep_color = resolve_color("control.border", Color::rgba(70, 70, 80));

    bool horiz = (orientation_ == Orientation::horizontal);
    float pos = horiz ? b.x + spacing_ : b.y + spacing_;

    for (auto& item : items_) {
        switch (item.type) {
        case ToolStripItemType::Separator: {
            canvas.set_stroke_color(sep_color);
            canvas.set_line_width(1.0f);
            if (horiz) {
                canvas.stroke_rect(pos, b.y + 4.0f, 1.0f, b.height - 8.0f);
                pos += spacing_ + 1.0f;
            } else {
                canvas.stroke_rect(b.x + 4.0f, pos, b.width - 8.0f, 1.0f);
                pos += spacing_ + 1.0f;
            }
            break;
        }
        case ToolStripItemType::Spacer:
            pos += item_size_;
            break;
        case ToolStripItemType::Button:
        case ToolStripItemType::Toggle: {
            canvas.set_fill_color(item.toggled ? toggled_bg : btn_bg);
            if (horiz) {
                canvas.fill_rounded_rect(pos, b.y + 4.0f, item_size_, item_size_, 4.0f);
                canvas.set_fill_color(item.enabled ? btn_text : disabled_text);
                canvas.set_font("system", 11.0f);
                canvas.fill_text(item.label, pos + 4.0f, b.y + 4.0f + item_size_ * 0.65f);
                pos += item_size_ + spacing_;
            } else {
                canvas.fill_rounded_rect(b.x + 4.0f, pos, item_size_, item_size_, 4.0f);
                canvas.set_fill_color(item.enabled ? btn_text : disabled_text);
                canvas.set_font("system", 11.0f);
                canvas.fill_text(item.label, b.x + 8.0f, pos + item_size_ * 0.65f);
                pos += item_size_ + spacing_;
            }
            break;
        }
        case ToolStripItemType::Custom:
            pos += item_size_ + spacing_;
            break;
        }
    }
}

void ToolStrip::on_mouse_down(Point pos) {
    int idx = hit_test_item(pos);
    if (idx < 0) return;

    auto& item = items_[static_cast<size_t>(idx)];
    if (!item.enabled) return;

    if (item.type == ToolStripItemType::Toggle) {
        item.toggled = !item.toggled;
        if (item.on_toggle) item.on_toggle(item.toggled);
    } else if (item.type == ToolStripItemType::Button) {
        if (item.on_click) item.on_click();
    }
}

int ToolStrip::hit_test_item(Point p) const {
    auto b = local_bounds();
    bool horiz = (orientation_ == Orientation::horizontal);
    float pos = horiz ? b.x + spacing_ : b.y + spacing_;

    for (size_t i = 0; i < items_.size(); ++i) {
        auto& item = items_[i];
        switch (item.type) {
        case ToolStripItemType::Separator:
            pos += spacing_ + 1.0f;
            break;
        case ToolStripItemType::Spacer:
            pos += item_size_;
            break;
        case ToolStripItemType::Button:
        case ToolStripItemType::Toggle:
        case ToolStripItemType::Custom: {
            float ix = horiz ? pos : b.x + 4.0f;
            float iy = horiz ? b.y + 4.0f : pos;
            if (p.x >= ix && p.x < ix + item_size_ &&
                p.y >= iy && p.y < iy + item_size_)
                return static_cast<int>(i);
            pos += item_size_ + spacing_;
            break;
        }
        }
    }
    return -1;
}

} // namespace pulp::view
