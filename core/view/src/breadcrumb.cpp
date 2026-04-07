#include <pulp/view/breadcrumb.hpp>

namespace pulp::view {

Breadcrumb::Breadcrumb() {
    set_access_role(AccessRole::group);
}

void Breadcrumb::set_items(std::vector<Item> items) {
    items_ = std::move(items);
}

void Breadcrumb::push(Item item) {
    items_.push_back(std::move(item));
}

Breadcrumb::Item Breadcrumb::pop() {
    if (items_.empty()) return {};
    auto item = std::move(items_.back());
    items_.pop_back();
    return item;
}

void Breadcrumb::pop_to(size_t index) {
    if (index < items_.size())
        items_.resize(index + 1);
}

int Breadcrumb::item_at_position(Point pos) const {
    float x = 8.0f;
    float font_size = 13.0f;
    float char_width = font_size * 0.6f;

    for (size_t i = 0; i < items_.size(); ++i) {
        float item_width = static_cast<float>(items_[i].label.size()) * char_width;
        if (pos.x >= x && pos.x < x + item_width)
            return static_cast<int>(i);
        x += item_width;
        x += static_cast<float>(separator_.size()) * char_width + 8;
    }
    return -1;
}

void Breadcrumb::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color("bg.surface", Color::rgba8(40, 40, 50));
    auto text_color = resolve_color("text.secondary", Color::rgba8(140, 140, 160));
    auto active_color = resolve_color("text.primary", Color::rgba8(220, 220, 230));
    auto sep_color = resolve_color("text.disabled", Color::rgba8(80, 80, 100));

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 4.0f);

    float x = b.x + 8;
    float y = b.y + b.height * 0.6f;
    float font_size = 13.0f;
    float char_width = font_size * 0.6f;

    canvas.set_font("", font_size);
    canvas.set_text_align(canvas::TextAlign::left);

    for (size_t i = 0; i < items_.size(); ++i) {
        bool is_last = (i == items_.size() - 1);
        canvas.set_fill_color(is_last ? active_color : text_color);
        canvas.fill_text(items_[i].label, x, y);
        x += static_cast<float>(items_[i].label.size()) * char_width;

        if (!is_last) {
            x += 4;
            canvas.set_fill_color(sep_color);
            canvas.fill_text(separator_, x, y);
            x += static_cast<float>(separator_.size()) * char_width + 4;
        }
    }
}

void Breadcrumb::on_mouse_down(Point pos) {
    int idx = item_at_position(pos);
    if (idx >= 0 && idx < static_cast<int>(items_.size())) {
        if (on_navigate) on_navigate(static_cast<size_t>(idx), items_[static_cast<size_t>(idx)]);
    }
}

} // namespace pulp::view
