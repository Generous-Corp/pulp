#include <pulp/view/color_picker.hpp>
#include <cstdio>
#include <cmath>

namespace pulp::view {

ColorPicker::ColorPicker() {
    set_focusable(true);
}

void ColorPicker::set_color(Color c) {
    color_ = c;
    update_from_color();
}

void ColorPicker::set_hsl(HSL hsl) {
    hsl_ = hsl;
    update_from_hsl();
}

void ColorPicker::set_hex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return;
    auto val = std::stoul(hex.substr(1), nullptr, 16);
    if (hex.size() == 7)
        color_ = color_from_hex(static_cast<uint32_t>(val));
    else if (hex.size() == 9)
        color_ = color_from_hex_alpha(static_cast<uint32_t>(val));
    update_from_color();
}

std::string ColorPicker::hex() const {
    char buf[10];
    if (color_.a8() == 255)
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", color_.r8(), color_.g8(), color_.b8());
    else
        snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color_.r8(), color_.g8(), color_.b8(), color_.a8());
    return buf;
}

void ColorPicker::set_swatches(std::vector<Color> swatches) {
    swatches_ = std::move(swatches);
}

void ColorPicker::update_from_hsl() {
    color_ = hsl_to_rgb(hsl_, color_.a);
}

void ColorPicker::update_from_color() {
    hsl_ = rgb_to_hsl(color_);
}

Rect ColorPicker::sl_area() const {
    auto b = local_bounds();
    float size = std::min(b.width, 180.0f);
    return {b.x + 4, b.y + 4, size - 8, size - 8};
}

Rect ColorPicker::hue_bar() const {
    auto sl = sl_area();
    return {sl.x, sl.y + sl.height + 8, sl.width, 20.0f};
}

Rect ColorPicker::alpha_bar() const {
    if (!show_alpha_) return {};
    auto hb = hue_bar();
    return {hb.x, hb.y + hb.height + 8, hb.width, 20.0f};
}

Rect ColorPicker::swatch_area() const {
    auto last = show_alpha_ ? alpha_bar() : hue_bar();
    return {last.x, last.y + last.height + 8, last.width, 40.0f};
}

void ColorPicker::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color("bg.surface", Color::rgba8(40, 40, 50));
    auto border_color = resolve_color("control.border", Color::rgba8(80, 80, 90));

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 8.0f);

    // SL area — draw a gradient grid approximation
    auto sl = sl_area();
    constexpr int steps = 16;
    float cell_w = sl.width / steps;
    float cell_h = sl.height / steps;

    for (int sx = 0; sx < steps; ++sx) {
        for (int sy = 0; sy < steps; ++sy) {
            float s = static_cast<float>(sx) / (steps - 1);
            float l = 1.0f - static_cast<float>(sy) / (steps - 1);
            HSL sample{hsl_.h, s, l};
            auto c = hsl_to_rgb(sample);
            canvas.set_fill_color(c);
            canvas.fill_rect(sl.x + sx * cell_w, sl.y + sy * cell_h, cell_w + 0.5f, cell_h + 0.5f);
        }
    }

    // SL cursor
    float cx = sl.x + hsl_.s * sl.width;
    float cy = sl.y + (1.0f - hsl_.l) * sl.height;
    canvas.set_stroke_color(Color::rgba8(255, 255, 255));
    canvas.set_line_width(2.0f);
    canvas.stroke_circle(cx, cy, 6.0f);
    canvas.set_fill_color(color_);
    canvas.fill_circle(cx, cy, 4.0f);

    // Hue bar
    auto hb = hue_bar();
    float hue_step = hb.width / 36.0f;
    for (int i = 0; i < 36; ++i) {
        float h = static_cast<float>(i) * 10.0f;
        auto c = hsl_to_rgb({h, 1.0f, 0.5f});
        canvas.set_fill_color(c);
        canvas.fill_rect(hb.x + i * hue_step, hb.y, hue_step + 0.5f, hb.height);
    }
    // Hue cursor
    float hue_x = hb.x + (hsl_.h / 360.0f) * hb.width;
    canvas.set_fill_color(Color::rgba8(255, 255, 255));
    canvas.fill_rect(hue_x - 2, hb.y - 2, 4, hb.height + 4);

    // Alpha bar
    if (show_alpha_) {
        auto ab = alpha_bar();
        for (int i = 0; i <= 20; ++i) {
            float t = static_cast<float>(i) / 20.0f;
            auto c = Color::rgba(color_.r, color_.g, color_.b, t);
            float w = ab.width / 20.0f;
            canvas.set_fill_color(c);
            canvas.fill_rect(ab.x + i * w, ab.y, w + 0.5f, ab.height);
        }
        float alpha_x = ab.x + color_.a * ab.width;
        canvas.set_fill_color(Color::rgba8(255, 255, 255));
        canvas.fill_rect(alpha_x - 2, ab.y - 2, 4, ab.height + 4);
    }

    // Swatches
    if (!swatches_.empty()) {
        auto sa = swatch_area();
        float sw = std::min(24.0f, sa.width / static_cast<float>(swatches_.size()));
        for (size_t i = 0; i < swatches_.size(); ++i) {
            float sx = sa.x + static_cast<float>(i) * (sw + 4);
            canvas.set_fill_color(swatches_[i]);
            canvas.fill_rounded_rect(sx, sa.y, sw, sw, 4.0f);
            canvas.set_stroke_color(border_color);
            canvas.set_line_width(0.5f);
            canvas.stroke_rounded_rect(sx, sa.y, sw, sw, 4.0f);
        }
    }

    // Current color preview
    float px = b.x + b.width - 44, py = b.y + 8;
    canvas.set_fill_color(color_);
    canvas.fill_rounded_rect(px, py, 36, 36, 6.0f);
    canvas.set_stroke_color(border_color);
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(px, py, 36, 36, 6.0f);
}

void ColorPicker::on_mouse_down(Point pos) {
    auto sl = sl_area();
    auto hb = hue_bar();
    auto ab = alpha_bar();
    auto sa = swatch_area();

    if (pos.x >= sl.x && pos.x <= sl.x + sl.width && pos.y >= sl.y && pos.y <= sl.y + sl.height) {
        drag_target_ = DragTarget::saturation_lightness;
    } else if (pos.x >= hb.x && pos.x <= hb.x + hb.width && pos.y >= hb.y && pos.y <= hb.y + hb.height) {
        drag_target_ = DragTarget::hue;
    } else if (show_alpha_ && pos.x >= ab.x && pos.x <= ab.x + ab.width && pos.y >= ab.y && pos.y <= ab.y + ab.height) {
        drag_target_ = DragTarget::alpha;
    } else if (pos.y >= sa.y && !swatches_.empty()) {
        float sw = std::min(24.0f, sa.width / static_cast<float>(swatches_.size()));
        int idx = static_cast<int>((pos.x - sa.x) / (sw + 4));
        if (idx >= 0 && idx < static_cast<int>(swatches_.size())) {
            set_color(swatches_[static_cast<size_t>(idx)]);
            if (on_change) on_change(color_);
        }
        return;
    }

    on_mouse_drag(pos);
}

void ColorPicker::on_mouse_drag(Point pos) {
    if (drag_target_ == DragTarget::saturation_lightness) {
        auto sl = sl_area();
        hsl_.s = std::clamp((pos.x - sl.x) / sl.width, 0.0f, 1.0f);
        hsl_.l = std::clamp(1.0f - (pos.y - sl.y) / sl.height, 0.0f, 1.0f);
        update_from_hsl();
        if (on_change) on_change(color_);
    } else if (drag_target_ == DragTarget::hue) {
        auto hb = hue_bar();
        hsl_.h = std::clamp((pos.x - hb.x) / hb.width, 0.0f, 1.0f) * 360.0f;
        update_from_hsl();
        if (on_change) on_change(color_);
    } else if (drag_target_ == DragTarget::alpha && show_alpha_) {
        auto ab = alpha_bar();
        float t = std::clamp((pos.x - ab.x) / ab.width, 0.0f, 1.0f);
        color_.a = t;
        if (on_change) on_change(color_);
    }
}

void ColorPicker::on_mouse_up(Point pos) {
    (void)pos;
    drag_target_ = DragTarget::none;
}

void ColorPicker::on_mouse_event(const MouseEvent& event) {
    View::on_mouse_event(event);
}

} // namespace pulp::view
