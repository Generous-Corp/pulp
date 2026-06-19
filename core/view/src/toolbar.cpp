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

void Toolbar::add_separator() {
    ToolbarItem item;
    item.type = ToolbarItemType::Separator;
    items_.push_back(std::move(item));
}

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

// Width a Custom item occupies along the bar: its view's flex preferred_width
// when set, else the square item_size_. Shared by paint and hit-testing so
// they always agree on layout.
static float toolbar_custom_width(const ToolbarItem& item, float item_size) {
    if (item.custom_view) {
        float pw = item.custom_view->flex().preferred_width;
        if (pw > 0.0f) return pw;
    }
    return item_size;
}

float Toolbar::custom_item_width(const ToolbarItem& item) const {
    return toolbar_custom_width(item, item_size_);
}

void Toolbar::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;

    // Background. NOTE: Color::rgba() takes 0–1 floats and clamps — passing
    // 0–255 ints (the prior bug) painted the whole toolbar solid white. Resolve
    // from theme tokens with rgba8 fallbacks.
    canvas.set_fill_color(resolve_color("bg.secondary", canvas::Color::rgba8(40, 40, 48)));
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
            canvas.set_stroke_color(resolve_color("divider", canvas::Color::rgba8(70, 70, 80)));
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

        // Custom view — paint it directly. A custom view sizes to its own
        // flex preferred_width when set (e.g. a wide "120 BPM" readout) so it
        // is not clamped to the square item_size_; falls back to item_size_.
        if (item.type == ToolbarItemType::Custom && item.custom_view) {
            float cw = custom_item_width(item);
            item.custom_view->set_bounds({item_x, item_y, cw, item_size_});
            canvas.save();
            canvas.translate(item_x, item_y);
            item.custom_view->paint(canvas);
            canvas.restore();
            pos += cw + spacing_;
            continue;
        }

        // Button/toggle background — toggled = accent fill, else elevated.
        auto bg_color = item.toggled
            ? resolve_color("accent.primary", canvas::Color::rgba8(60, 80, 140))
            : resolve_color("bg.elevated", canvas::Color::rgba8(55, 55, 65));
        if (!item.enabled) bg_color = resolve_color("bg.surface", canvas::Color::rgba8(40, 40, 45));

        if (horiz) {
            canvas.set_fill_color(bg_color);
            canvas.fill_rounded_rect(item_x, 4, item_size_, item_size_, 4.0f);
        } else {
            canvas.set_fill_color(bg_color);
            canvas.fill_rounded_rect(4, item_y, item_size_, item_size_, 4.0f);
        }

        // Label — on a toggled (accent-filled) item use on-accent ink.
        auto text_color = !item.enabled
            ? resolve_color("text.disabled", canvas::Color::rgba8(100, 100, 110))
            : item.toggled ? resolve_color("accent.text", canvas::Color::rgba8(20, 24, 30))
                           : resolve_color("text.primary", canvas::Color::rgba8(200, 200, 215));
        canvas.set_fill_color(text_color);
        canvas.set_font("system", 11.0f);

        // Center the leading glyph of the label as an icon. UTF-8 aware: a
        // naive substr(0, 2) splits a multibyte codepoint (e.g. a 3-byte
        // transport glyph ▶ / ↺) into invalid UTF-8, which aborts Skia text
        // shaping. Take the first whole codepoint when the label leads with a
        // multibyte sequence; otherwise keep the historic first-two-ASCII look.
        std::string display;
        if (!item.label.empty()) {
            auto lead = static_cast<unsigned char>(item.label[0]);
            if (lead < 0x80) {
                display = item.label.substr(0, 2);
            } else {
                size_t n = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : 2;
                display = item.label.substr(0, std::min(n, item.label.size()));
            }
        }
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
    canvas.set_stroke_color(resolve_color("divider", canvas::Color::rgba8(60, 60, 70)));
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

        float span = (item.type == ToolbarItemType::Custom)
                         ? custom_item_width(item) : item_size_;
        float coord = horiz ? pos.x : pos.y;
        if (coord >= item_pos && coord < item_pos + span)
            return i;

        item_pos += span + spacing_;
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
            else if (items_[i].type == ToolbarItemType::Custom)
                item_start += custom_item_width(items_[i]) + spacing_;
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
