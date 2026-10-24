// ButtonSurface: Chromium's themed push-button surface for an imported
// `appearance: auto` <button>, whose content is the view's children.
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::view {

namespace {

constexpr canvas::Color argb8(uint32_t argb) {
    const auto a = static_cast<uint8_t>((argb >> 24) & 0xFF);
    const auto r = static_cast<uint8_t>((argb >> 16) & 0xFF);
    const auto g = static_cast<uint8_t>((argb >> 8) & 0xFF);
    const auto b = static_cast<uint8_t>(argb & 0xFF);
    return canvas::Color::rgba8(r, g, b, a); // token-lint:allow -- UA theme palette
}

struct ButtonPalette {
    canvas::Color fill, border;
};

// Chromium's web-native push-button colours per state. They reproduce the
// browser's UA theme, so they are deliberately not theme tokens. In the dark
// scheme the border is the fill colour.
ButtonPalette button_palette(UaControlState state, bool dark) {
    using S = UaControlState;
    if (dark) {
        switch (state) {
        case S::disabled:
            return {argb8(0xFF363636), argb8(0xFF363636)};
        case S::hovered:
            return {argb8(0xFF7B7B7B), argb8(0xFF7B7B7B)};
        case S::pressed:
            return {argb8(0xFF616161), argb8(0xFF616161)};
        case S::normal:
            break;
        }
        return {argb8(0xFF6B6B6B), argb8(0xFF6B6B6B)};
    }
    switch (state) {
    case S::disabled:
        return {argb8(0x4DEFEFEF), argb8(0x4D767676)};
    case S::hovered:
        return {argb8(0xFFE5E5E5), argb8(0xFF4F4F4F)};
    case S::pressed:
        return {argb8(0xFFF5F5F5), argb8(0xFF8D8D8D)};
    case S::normal:
        break;
    }
    return {argb8(0xFFEFEFEF), argb8(0xFF767676)};
}

// Chromium's zoomed border width: whole device pixels, never under one.
float device_width(float css_width, float device_scale) {
    return std::max(std::floor(css_width * device_scale), 1.0f);
}

Rect outset(Rect r, float h, float v) {
    return {r.x - h, r.y - v, std::max(0.0f, r.width + 2.0f * h),
            std::max(0.0f, r.height + 2.0f * v)};
}

} // namespace

ButtonSurface::ChromiumRefreshParts
ButtonSurface::chromium_refresh_parts(Rect bounds, ControlState state, ColorScheme scheme,
                                      bool focus_visible, float device_scale) {
    ChromiumRefreshParts p;
    const float dev = std::isfinite(device_scale) && device_scale > 0.0f ? device_scale : 1.0f;
    const bool dark = scheme == ColorScheme::dark;
    const auto palette = button_palette(state, dark);
    p.bounds = bounds;
    p.fill = palette.fill;
    p.border = palette.border;
    if (bounds.width * dev < 5.0f || bounds.height * dev < 5.0f) {
        p.solid = true;
        return p;
    }

    const float border_dev = device_width(1.0f, dev);
    p.border_width = border_dev / dev;
    p.radius = device_width(2.0f, dev) / dev;
    const float half = 0.5f * p.border_width;
    p.surface = outset(bounds, -half, -half);
    if (state == ControlState::disabled)
        p.lighten = dark ? argb8(0xFF3B3B3B) : argb8(0x33A9A9A9);

    if (focus_visible && state != ControlState::disabled) {
        // Blink's focus ring: a 3px stroke split 2:1 into an outer ring in the
        // panel-contrast colour and an inner ring in the outline colour, both
        // stroked at the wider width, the inner one pulled one border width
        // inside the box. Offsets are whole device pixels, and below scale 1
        // the stroke stays 3 device px so the ring never thins away.
        const float stroke_dev = dev >= 1.0f ? device_width(3.0f, dev) : 3.0f;
        const float ring_dev = stroke_dev / 3.0f * 2.0f;
        const float inner_offset_dev = -border_dev;
        const float outer_offset_dev = inner_offset_dev + std::ceil(stroke_dev / 3.0f);
        // A negative offset never shrinks the ring past half the box.
        auto ring = [&](float offset_dev) {
            const float h = std::max(offset_dev, -std::floor(bounds.width * dev / 2.0f));
            const float v = std::max(offset_dev, -std::floor(bounds.height * dev / 2.0f));
            return outset(bounds, h / dev, v / dev);
        };
        p.focus_ring = true;
        p.ring_width = ring_dev / dev;
        p.outer_ring = ring(outer_offset_dev);
        p.inner_ring = ring(inner_offset_dev);
        p.outer_ring_color = dark ? argb8(0xFF101010) : argb8(0xFFFFFFFF);
        p.inner_ring_color = dark ? argb8(0xFF99C8FF) : argb8(0xFF005FCC);
    }
    return p;
}

ButtonSurface::ControlState ButtonSurface::control_state() const {
    if (!enabled())
        return ControlState::disabled;
    if (pressed_)
        return ControlState::pressed;
    if (is_hovered())
        return ControlState::hovered;
    return ControlState::normal;
}

ButtonSurface::ChromiumRefreshParts
ButtonSurface::chromium_refresh_parts(float device_scale) const {
    return chromium_refresh_parts(self_paint_bounds(), control_state(), color_scheme_,
                                  focus_visible_, device_scale);
}

void ButtonSurface::paint(canvas::Canvas& canvas) {
    const auto p = chromium_refresh_parts(canvas.backing_scale());
    if (p.solid) {
        canvas.set_fill_color(p.fill);
        canvas.fill_rect(p.bounds.x, p.bounds.y, p.bounds.width, p.bounds.height);
        return;
    }
    const auto& s = p.surface;
    if (p.lighten) {
        canvas.set_fill_color(*p.lighten);
        canvas.fill_rounded_rect(s.x, s.y, s.width, s.height, p.radius);
    }
    canvas.set_fill_color(p.fill);
    canvas.fill_rounded_rect(s.x, s.y, s.width, s.height, p.radius);
    canvas.set_stroke_color(p.border);
    canvas.set_line_width(p.border_width);
    canvas.stroke_rounded_rect(s.x, s.y, s.width, s.height, p.radius);

    if (p.focus_ring) {
        canvas.set_line_width(p.ring_width);
        canvas.set_stroke_color(p.outer_ring_color);
        canvas.stroke_rounded_rect(p.outer_ring.x, p.outer_ring.y, p.outer_ring.width,
                                   p.outer_ring.height, p.radius);
        canvas.set_stroke_color(p.inner_ring_color);
        canvas.stroke_rounded_rect(p.inner_ring.x, p.inner_ring.y, p.inner_ring.width,
                                   p.inner_ring.height, p.radius);
    }
}

} // namespace pulp::view
