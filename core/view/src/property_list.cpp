#include <pulp/view/property_list.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulp::view {

PropertyList::PropertyList() {
    set_access_role(AccessRole::group);
}

void PropertyList::set_properties(std::vector<Property> props) {
    properties_ = std::move(props);
}

void PropertyList::set_value(const std::string& key, PropertyValue value) {
    for (auto& p : properties_) {
        if (p.key == key) {
            p.value = std::move(value);
            return;
        }
    }
}

const PropertyList::Property* PropertyList::find_property(const std::string& key) const {
    for (auto& p : properties_) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

float PropertyList::intrinsic_height() const {
    float height = 0;
    std::string last_category;
    for (auto& p : properties_) {
        if (show_categories_ && !p.category.empty() && p.category != last_category) {
            height += row_height_ * 0.8f;
            last_category = p.category;
        }
        height += row_height_;
    }
    return height;
}

void PropertyList::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color("bg.surface", Color::rgba8(30, 30, 40));
    auto text_color = resolve_color("text.primary", Color::rgba8(200, 200, 210));
    auto label_color = resolve_color("text.secondary", Color::rgba8(140, 140, 160));
    auto cat_color = resolve_color("accent.primary", Color::rgba8(100, 160, 255));
    auto border_c = resolve_color("control.border", Color::rgba8(60, 60, 70));
    auto hover_bg = resolve_color("bg.elevated", Color::rgba8(50, 50, 60));

    canvas.set_fill_color(bg);
    canvas.fill_rect(b.x, b.y, b.width, b.height);

    float y = b.y;
    float label_w = b.width * label_width_;
    float value_x = b.x + label_w + 8;
    std::string last_category;

    for (size_t i = 0; i < properties_.size(); ++i) {
        auto& prop = properties_[i];

        // Category header
        if (show_categories_ && !prop.category.empty() && prop.category != last_category) {
            last_category = prop.category;
            float cat_h = row_height_ * 0.8f;
            canvas.set_fill_color(cat_color);
            canvas.set_font("", 11.0f);
            canvas.fill_text(prop.category, b.x + 8, y + cat_h * 0.65f);
            y += cat_h;
        }

        // Row background
        if (static_cast<int>(i) == editing_index_) {
            canvas.set_fill_color(hover_bg);
            canvas.fill_rect(b.x, y, b.width, row_height_);
        }

        // Separator line
        canvas.set_stroke_color(border_c);
        canvas.set_line_width(0.5f);
        canvas.stroke_line(b.x, y + row_height_, b.x + b.width, y + row_height_);

        // Label
        canvas.set_fill_color(label_color);
        canvas.set_font("", 12.0f);
        canvas.fill_text(prop.label.empty() ? prop.key : prop.label,
                         b.x + 8, y + row_height_ * 0.65f);

        // Value
        std::string value_str;
        std::visit([&](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::string>)
                value_str = val;
            else if constexpr (std::is_same_v<T, float>) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.2f", val);
                value_str = buf;
            } else if constexpr (std::is_same_v<T, int>)
                value_str = std::to_string(val);
            else if constexpr (std::is_same_v<T, bool>)
                value_str = val ? "true" : "false";
            else if constexpr (std::is_same_v<T, Color>) {
                auto to_byte = [](float channel) {
                    return static_cast<unsigned>(
                        std::clamp(static_cast<int>(std::lround(channel * 255.0f)),
                                   0,
                                   255));
                };
                char buf[10];
                snprintf(buf,
                         sizeof(buf),
                         "#%02x%02x%02x",
                         to_byte(val.r),
                         to_byte(val.g),
                         to_byte(val.b));
                value_str = buf;
            }
        }, prop.value);

        auto val_color = prop.read_only ? label_color : text_color;
        canvas.set_fill_color(val_color);
        canvas.fill_text(value_str, value_x, y + row_height_ * 0.65f);

        // Color swatch for Color values
        if (std::holds_alternative<Color>(prop.value)) {
            auto c = std::get<Color>(prop.value);
            canvas.set_fill_color(c);
            canvas.fill_rounded_rect(b.x + b.width - 28, y + 4, 20, row_height_ - 8, 3.0f);
        }

        y += row_height_;
    }
}

void PropertyList::on_mouse_down(Point pos) {
    auto b = local_bounds();
    float y = b.y;
    std::string last_category;

    for (size_t i = 0; i < properties_.size(); ++i) {
        auto& prop = properties_[i];

        if (show_categories_ && !prop.category.empty() && prop.category != last_category) {
            last_category = prop.category;
            y += row_height_ * 0.8f;
        }

        if (pos.y >= y && pos.y < y + row_height_) {
            editing_index_ = static_cast<int>(i);
            if (!prop.read_only && std::holds_alternative<bool>(prop.value)) {
                bool v = !std::get<bool>(prop.value);
                prop.value = v;
                if (on_change) on_change(prop.key, v);
            }
            return;
        }

        y += row_height_;
    }
    editing_index_ = -1;
}

} // namespace pulp::view
