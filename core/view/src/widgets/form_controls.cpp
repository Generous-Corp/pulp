// HTML form-control widgets: RangeSlider and Checkbox painting, including the
// Chromium form-control skin a design-imported `appearance: auto` control wears.
#include <pulp/view/theme_contrast.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace pulp::view {

// ── RangeSlider paint ───────────────────────────────────────────────────────

namespace {

// Chromium's cross-platform form-control theme: an 8px track whose ends are
// pulled in one device pixel so the thumb can cover them, a 1px border, and a
// 16px thumb part painted as a circle inset by half a device pixel.
constexpr float kRefreshTrackThickness = 8.0f;
constexpr float kRefreshThumbPart = 16.0f;
constexpr float kRefreshBorderWidth = 1.0f;
// Lightness shift Chromium applies to an author accent for hover / press.
constexpr float kRefreshAccentLightnessShift = 0.11f;

struct RefreshPalette {
    canvas::Color fill, value, border;
};

constexpr canvas::Color argb8(uint32_t argb) {
    const auto a = static_cast<uint8_t>((argb >> 24) & 0xFF);
    const auto r = static_cast<uint8_t>((argb >> 16) & 0xFF);
    const auto g = static_cast<uint8_t>((argb >> 8) & 0xFF);
    const auto b = static_cast<uint8_t>(argb & 0xFF);
    return canvas::Color::rgba8(r, g, b, a); // token-lint:allow -- UA theme palette
}

// An author accent for `state`, as Chromium resolves it: the accent itself at
// rest, shifted 0.11 in HSL lightness for hover (darker in light schemes) and
// press (lighter), the direction reversed in dark schemes.
canvas::Color refresh_accent_for_state(canvas::Color accent, UaControlState state, bool dark) {
    if (state == UaControlState::normal || state == UaControlState::disabled)
        return accent;
    float shift = state == UaControlState::pressed ? kRefreshAccentLightnessShift
                                                   : -kRefreshAccentLightnessShift;
    if (dark)
        shift = -shift;
    auto hsl = accent.to_hsl();
    hsl.l = std::clamp(hsl.l + shift, 0.0f, 1.0f);
    return canvas::Color::from_hsl(hsl, accent.a);
}

// Chromium's default web-native control colours per state; the slider thumb
// shares the value-bar colour. These reproduce the browser's UA theme, so they
// are deliberately not theme tokens.
RefreshPalette refresh_palette(UaControlState state, bool dark) {
    using S = UaControlState;
    if (dark) {
        switch (state) {
        case S::disabled:
            return {argb8(0xFF363636), argb8(0xFF757575), argb8(0xFF626262)};
        case S::hovered:
            return {argb8(0xFF3B3B3B), argb8(0xFFD1E6FF), argb8(0xFFACACAC)};
        case S::pressed:
            return {argb8(0xFF3B3B3B), argb8(0xFF61A9FF), argb8(0xFF6E6E6E)};
        case S::normal:
            break;
        }
        return {argb8(0xFF3B3B3B), argb8(0xFF99C8FF), argb8(0xFF858585)};
    }
    switch (state) {
    case S::disabled:
        return {argb8(0x4DEFEFEF), argb8(0xFFCBCBCB), argb8(0x4D767676)};
    case S::hovered:
        return {argb8(0xFFE5E5E5), argb8(0xFF005CC8), argb8(0x804F4F4F)};
    case S::pressed:
        return {argb8(0xFFF5F5F5), argb8(0xFF3793FF), argb8(0x808D8D8D)};
    case S::normal:
        break;
    }
    return {argb8(0xFFEFEFEF), argb8(0xFF0075FF), argb8(0x80767676)};
}

} // namespace

UaColorScheme ua_scheme_for_accent(canvas::Color accent, UaColorScheme scheme,
                                   canvas::Color contrast_light, canvas::Color contrast_dark) {
    // Chromium's minimum visible contrast ratio.
    constexpr float kMinimumVisibleContrast = 3.0f;
    const float with_light = contrast_ratio(accent, contrast_light);
    const float with_dark = contrast_ratio(accent, contrast_dark);
    if (scheme == UaColorScheme::dark) {
        if (with_dark < kMinimumVisibleContrast && with_dark < with_light)
            return UaColorScheme::light;
    } else if (with_light < kMinimumVisibleContrast && with_light < with_dark) {
        return UaColorScheme::dark;
    }
    return scheme;
}

RangeSlider::ChromiumRefreshParts
RangeSlider::chromium_refresh_parts(Rect bounds, float position, bool vertical, ControlState state,
                                    ColorScheme scheme, std::optional<canvas::Color> accent,
                                    float device_scale) {
    ChromiumRefreshParts p;
    const float px =
        1.0f / (std::isfinite(device_scale) && device_scale > 0.0f ? device_scale : 1.0f);
    const float t = std::isfinite(position) ? std::clamp(position, 0.0f, 1.0f) : 0.0f;
    const float half_track = kRefreshTrackThickness * 0.5f;
    const float cx = bounds.center_x();
    const float cy = bounds.center_y();

    // Track: the full major axis, centred and clamped on the minor axis.
    Rect full;
    if (vertical) {
        const float l = std::max(bounds.x, cx - half_track);
        const float r = std::min(bounds.right(), cx + half_track);
        full = {l, bounds.y, std::max(0.0f, r - l), bounds.height};
    } else {
        const float top = std::max(bounds.y, cy - half_track);
        const float bot = std::min(bounds.bottom(), cy + half_track);
        full = {bounds.x, top, bounds.width, std::max(0.0f, bot - top)};
    }
    // Chromium's radius is "fully rounded" (40px, clamped to the rect).
    p.track_radius = 0.5f * std::min(full.width, full.height);
    p.track = full;
    if (vertical) {
        p.track.y += px;
        p.track.height = std::max(0.0f, p.track.height - 2.0f * px);
    } else {
        p.track.x += px;
        p.track.width = std::max(0.0f, p.track.width - 2.0f * px);
    }
    p.track_radius = std::min(p.track_radius, 0.5f * std::min(p.track.width, p.track.height));

    p.border_width = kRefreshBorderWidth;
    const float inset = p.border_width * 0.5f;
    p.border = {p.track.x + inset, p.track.y + inset,
                std::max(0.0f, p.track.width - p.border_width),
                std::max(0.0f, p.track.height - p.border_width)};
    p.border_radius = 0.5f * std::min(p.border.width, p.border.height);

    // Thumb part: 16px square travelling the major axis, centred on the minor.
    if (vertical) {
        const float travel = std::max(0.0f, bounds.height - kRefreshThumbPart);
        const float top = bounds.y + (1.0f - t) * travel;
        p.thumb = {cx - kRefreshThumbPart * 0.5f + 0.5f * px, top + 0.5f * px,
                   kRefreshThumbPart - px, kRefreshThumbPart - px};
        // Minimum at the bottom: the value bar runs from the thumb down.
        const float thumb_cy = top + kRefreshThumbPart * 0.5f;
        const float clip_top = thumb_cy - half_track;
        p.value_clip = {full.x, clip_top, full.width, std::max(0.0f, full.bottom() - clip_top)};
    } else {
        const float travel = std::max(0.0f, bounds.width - kRefreshThumbPart);
        const float left = bounds.x + t * travel;
        p.thumb = {left + 0.5f * px, cy - kRefreshThumbPart * 0.5f + 0.5f * px,
                   kRefreshThumbPart - px, kRefreshThumbPart - px};
        const float thumb_cx = left + kRefreshThumbPart * 0.5f;
        p.value_clip = {full.x, full.y, std::max(0.0f, thumb_cx + half_track - full.x),
                        full.height};
    }

    // An accent next to a track it barely contrasts with flips the palette,
    // judged against the resting track colour so hover cannot flicker it.
    if (accent && state != ControlState::disabled)
        scheme = ua_scheme_for_accent(*accent, scheme, argb8(0xFFEFEFEF), argb8(0xFF3B3B3B));
    p.scheme = scheme;
    const bool dark = scheme == ColorScheme::dark;
    const auto palette = refresh_palette(state, dark);
    p.track_fill = palette.fill;
    p.border_color = palette.border;
    p.value_fill = palette.value;
    if (accent && state != ControlState::disabled)
        p.value_fill = refresh_accent_for_state(*accent, state, dark);
    p.thumb_fill = p.value_fill;
    return p;
}

RangeSlider::ControlState RangeSlider::control_state() const {
    if (!enabled())
        return ControlState::disabled;
    if (dragging_)
        return ControlState::pressed;
    if (is_hovered())
        return ControlState::hovered;
    return ControlState::normal;
}

RangeSlider::ChromiumRefreshParts RangeSlider::chromium_refresh_parts(float device_scale) const {
    std::optional<canvas::Color> accent;
    if (has_accent_color_)
        accent = accent_color_;
    // A modulator moves the thumb and value bar to the played value, in the
    // modulation colour, as the platform skin does.
    const float position = has_modulated_
                               ? (skew_ == 1.0f ? modulated_ : std::pow(modulated_, skew_))
                               : value_to_position_();
    // Chromium paints a theme part in its pixel-snapped CSS box, so an imported
    // control paints in the snapped rect its capture recorded.
    auto parts =
        chromium_refresh_parts(self_paint_bounds(), position, orientation_ == Orientation::vertical,
                               control_state(), color_scheme_, accent, device_scale);
    if (has_modulated_) {
        parts.value_fill = resolve_color("knob.modulation", canvas::Color::rgba8(190, 150, 255));
        parts.thumb_fill = parts.value_fill;
    }
    return parts;
}

void RangeSlider::paint_chromium_refresh_(canvas::Canvas& canvas) {
    const auto p = chromium_refresh_parts(canvas.backing_scale());

    canvas.set_fill_color(p.track_fill);
    canvas.fill_rounded_rect(p.track.x, p.track.y, p.track.width, p.track.height, p.track_radius);

    // The value bar is the same rounded track clipped at the thumb, so its
    // start keeps the track's rounded end and its open end hides under the thumb.
    if (p.value_clip.width > 0.0f && p.value_clip.height > 0.0f) {
        canvas.save();
        canvas.clip_rect(p.value_clip.x, p.value_clip.y, p.value_clip.width, p.value_clip.height);
        canvas.set_fill_color(p.value_fill);
        canvas.fill_rounded_rect(p.track.x, p.track.y, p.track.width, p.track.height,
                                 p.track_radius);
        canvas.restore();
    }

    canvas.set_stroke_color(p.border_color);
    canvas.set_line_width(p.border_width);
    canvas.stroke_rounded_rect(p.border.x, p.border.y, p.border.width, p.border.height,
                               p.border_radius);

    canvas.set_fill_color(p.thumb_fill);
    canvas.fill_circle(p.thumb.center_x(), p.thumb.center_y(),
                       0.5f * std::min(p.thumb.width, p.thumb.height));
}

void RangeSlider::paint(canvas::Canvas& canvas) {
    if (skin_ == Skin::chromium_refresh) {
        paint_chromium_refresh_(canvas);
        return;
    }
    auto b = local_bounds();
    bool horiz = orientation_ == Orientation::horizontal;

    // CSS accent-color affects the active portion, not the handle. Keeping
    // the thumb independently opaque is important: painting an accent with
    // alpha over the already-painted track makes the track appear to show
    // through the handle.
    auto track_color = resolve_color("control.track", canvas::Color::rgba8(60, 60, 60));
    auto theme_fill = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
    auto theme_thumb = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
    const bool active = enabled();
    auto fill_color = active && has_accent_color_ ? accent_color_ : theme_fill;
    // clang-format off
    auto thumb_color = active && has_accent_color_
        ? canvas::Color::rgba8(248, 248, 248) // token-lint:allow -- Chromium accented-range keyline material
        : theme_thumb;
    // clang-format on
    // One thumb: while modulated, the fill and thumb sit at the value
    // playing in the modulation colour; the base is a tick (below).
    auto keyline_color = thumb_color;
    if (has_modulated_) {
        const auto modulation =
            resolve_color("knob.modulation", canvas::Color::rgba8(190, 150, 255));
        fill_color = modulation;
        thumb_color = modulation;
    }

    // Track thickness — typical HTML range visuals sit in 4..6px.
    float track_thick =
        std::min(track_thickness_, horiz ? std::max(b.height, 1.0f) : std::max(b.width, 1.0f));

    // Normalized position along the track, taking the (possibly-collapsed)
    // [min,max] range and the skew curve into account.
    const float base_t = value_to_position_();
    float t = has_modulated_ ? (skew_ == 1.0f ? modulated_ : std::pow(modulated_, skew_)) : base_t;

    // Background track.
    canvas.set_fill_color(track_color);
    if (horiz) {
        float ty = (b.height - track_thick) * 0.5f;
        canvas.fill_rounded_rect(0, ty, b.width, track_thick, track_thick * 0.5f);
    } else {
        float tx = (b.width - track_thick) * 0.5f;
        canvas.fill_rounded_rect(tx, 0, track_thick, b.height, track_thick * 0.5f);
    }

    // Active fill — only the portion between min and current value.
    canvas.set_fill_color(active ? fill_color : track_color);
    if (horiz) {
        float fill_w = t * b.width;
        float ty = (b.height - track_thick) * 0.5f;
        if (fill_w > 0)
            canvas.fill_rounded_rect(0, ty, fill_w, track_thick, track_thick * 0.5f);
    } else {
        float fill_h = t * b.height;
        float tx = (b.width - track_thick) * 0.5f;
        // Vertical: fill the lower portion (closer to min at the bottom).
        if (fill_h > 0)
            canvas.fill_rounded_rect(tx, b.height - fill_h, track_thick, fill_h,
                                     track_thick * 0.5f);
    }

    // Chromium's accented macOS range has a one-pixel light keyline around
    // the track. It is part of the control skin (not the element's CSS box
    // border), so draw it only for an enabled accented range. A disabled
    // range remains the neutral unoutlined home-screen control.
    if (active && has_accent_color_) {
        canvas.set_stroke_color(keyline_color);
        canvas.set_line_width(1.0f);
        if (horiz) {
            float ty = (b.height - track_thick) * 0.5f;
            canvas.stroke_rounded_rect(0.5f, ty + 0.5f, std::max(0.0f, b.width - 1.0f),
                                       std::max(0.0f, track_thick - 1.0f),
                                       std::max(0.0f, (track_thick - 1.0f) * 0.5f));
        } else {
            float tx = (b.width - track_thick) * 0.5f;
            canvas.stroke_rounded_rect(tx + 0.5f, 0.5f, std::max(0.0f, track_thick - 1.0f),
                                       std::max(0.0f, b.height - 1.0f),
                                       std::max(0.0f, (track_thick - 1.0f) * 0.5f));
        }
    }

    // Chrome/macOS uses a 24x16 pill rather than a circular thumb. Inset by
    // half its major-axis extent so it never overshoots the element bounds.
    canvas.set_fill_color(thumb_color);
    const float thumb_minor =
        (horiz ? std::min(b.height, 16.0f) : std::min(b.width, 16.0f)) * hover_scale_.value();
    const float thumb_major = std::min(horiz ? b.width : b.height, 24.0f) * hover_scale_.value();
    if (horiz) {
        float usable = std::max(0.0f, b.width - thumb_major);
        float hx = thumb_major * 0.5f + t * usable;
        canvas.fill_rounded_rect(hx - thumb_major * 0.5f, (b.height - thumb_minor) * 0.5f,
                                 thumb_major, thumb_minor, thumb_minor * 0.5f);
    } else {
        float usable = std::max(0.0f, b.height - thumb_major);
        // Vertical: t=0 → bottom (handle near max-y), t=1 → top.
        float hy = thumb_major * 0.5f + (1.0f - t) * usable;
        canvas.fill_rounded_rect((b.width - thumb_minor) * 0.5f, hy - thumb_major * 0.5f,
                                 thumb_minor, thumb_major, thumb_minor * 0.5f);
    }

    // The base a modulator moves this slider around: a quiet tick across the
    // track, on top of the thumb so it stays visible under it.
    if (has_modulated_) {
        canvas.set_stroke_color(theme_thumb);
        canvas.set_line_cap(canvas::LineCap::round);
        canvas.set_line_width(1.5f);
        const float half = track_thick * 0.5f + 3.0f;
        if (horiz) {
            const float usable = std::max(0.0f, b.width - thumb_major);
            const float bx = thumb_major * 0.5f + base_t * usable;
            canvas.stroke_line(bx, b.height * 0.5f - half, bx, b.height * 0.5f + half);
        } else {
            const float usable = std::max(0.0f, b.height - thumb_major);
            const float by = thumb_major * 0.5f + (1.0f - base_t) * usable;
            canvas.stroke_line(b.width * 0.5f - half, by, b.width * 0.5f + half, by);
        }
    }
}

// ── Checkbox ────────────────────────────────────────────────────────────────

namespace {

struct RefreshCheckPalette {
    canvas::Color border, accent, background, lighten;
};

// Chromium's checkbox / radio colours per state: border, default accent,
// control background (also the check and dash colour) and the disabled
// lighten layer.
RefreshCheckPalette refresh_check_palette(UaControlState state, bool dark) {
    using S = UaControlState;
    if (dark) {
        const auto bg = argb8(0xFF3B3B3B);
        switch (state) {
        case S::disabled:
            return {argb8(0xFF626262), argb8(0xFF757575), bg, bg};
        case S::hovered:
            return {argb8(0xFFACACAC), argb8(0xFFD1E6FF), bg, bg};
        case S::pressed:
            return {argb8(0xFF6E6E6E), argb8(0xFF61A9FF), bg, bg};
        case S::normal:
            break;
        }
        return {argb8(0xFF858585), argb8(0xFF99C8FF), bg, bg};
    }
    const auto lighten = argb8(0x33A9A9A9);
    switch (state) {
    case S::disabled:
        return {argb8(0x4D767676), argb8(0x4D767676), argb8(0x99FFFFFF), lighten};
    case S::hovered:
        return {argb8(0xFF4F4F4F), argb8(0xFF005CC8), argb8(0xFFFFFFFF), lighten};
    case S::pressed:
        return {argb8(0xFF8D8D8D), argb8(0xFF3793FF), argb8(0xFFFFFFFF), lighten};
    case S::normal:
        break;
    }
    return {argb8(0xFF767676), argb8(0xFF0075FF), argb8(0xFFFFFFFF), lighten};
}

} // namespace

Checkbox::ChromiumRefreshParts
Checkbox::chromium_refresh_parts(Rect bounds, Kind kind, bool checked, bool indeterminate,
                                 ControlState state, ColorScheme scheme,
                                 std::optional<canvas::Color> accent) {
    ChromiumRefreshParts p;
    const bool is_checkbox = kind == Kind::checkbox;
    if (!is_checkbox)
        indeterminate = false;
    const bool marked = checked || indeterminate;

    const float side = std::max(0.0f, std::min(bounds.width, bounds.height));
    p.box = {bounds.center_x() - side * 0.5f, bounds.center_y() - side * 0.5f, side, side};
    p.radius = is_checkbox ? std::min(2.0f, side * 0.5f) : side * 0.5f;

    // A checked, enabled control flips its palette when the accent would not
    // stand out against the control background.
    if (accent && checked && state != ControlState::disabled)
        scheme = ua_scheme_for_accent(*accent, scheme, argb8(0xFFFFFFFF), argb8(0xFF3B3B3B));
    p.scheme = scheme;
    const bool dark = scheme == ColorScheme::dark;
    const auto palette = refresh_check_palette(state, dark);
    const canvas::Color accent_now = accent && state != ControlState::disabled
                                         ? refresh_accent_for_state(*accent, state, dark)
                                         : palette.accent;

    constexpr float kBorderWidth = 1.0f;
    p.background_rect = {p.box.x + 0.2f * kBorderWidth, p.box.y + 0.2f * kBorderWidth,
                         std::max(0.0f, side - 0.4f * kBorderWidth),
                         std::max(0.0f, side - 0.4f * kBorderWidth)};
    if (state == ControlState::disabled)
        p.lighten = palette.lighten;
    p.background = palette.background;
    p.border_rect = {p.box.x + 0.5f * kBorderWidth, p.box.y + 0.5f * kBorderWidth,
                     std::max(0.0f, side - kBorderWidth), std::max(0.0f, side - kBorderWidth)};
    if (!is_checkbox || !marked)
        p.border = checked ? accent_now : palette.border;

    if (is_checkbox) {
        p.mark = palette.background;
        if (marked)
            p.accent_fill = accent_now;
        if (indeterminate) {
            // An 8x2 dash centred in the 13px reference box, scaled with it.
            const float sx = side / 13.0f;
            p.dash =
                Rect{p.box.x + 2.5f * sx, p.box.y + 5.5f * sx, side - 5.0f * sx, side - 11.0f * sx};
        } else if (checked) {
            p.has_check = true;
            p.check = {Point{p.box.x + side * 0.2f, p.box.center_y()},
                       Point{p.box.x + side * 0.4f, p.box.center_y() + side * 0.2f},
                       Point{p.box.right() - side * 0.2f, p.box.y + side * 0.2f}};
            p.check_width = side * 0.16f;
        }
    } else if (checked) {
        p.mark = accent_now;
        p.dot = Rect{p.box.x + side * 0.2f, p.box.y + side * 0.2f, side * 0.6f, side * 0.6f};
    }
    return p;
}

Checkbox::ControlState Checkbox::control_state() const {
    if (!enabled())
        return ControlState::disabled;
    if (is_hovered())
        return ControlState::hovered;
    return ControlState::normal;
}

Checkbox::ChromiumRefreshParts Checkbox::chromium_refresh_parts() const {
    return chromium_refresh_parts(self_paint_bounds(), kind_, checked_, indeterminate_,
                                  control_state(), color_scheme_, accent_color_);
}

void Checkbox::paint_chromium_refresh_(canvas::Canvas& canvas) {
    const auto p = chromium_refresh_parts();
    if (p.box.width <= 0.0f)
        return;
    // A box of 2px or less is a plain square in the border colour.
    if (p.box.width <= 2.0f) {
        canvas.set_fill_color(p.border.value_or(p.accent_fill.value_or(p.mark)));
        canvas.fill_rect(p.box.x, p.box.y, p.box.width, p.box.height);
        return;
    }
    const auto& bg = p.background_rect;
    if (p.lighten) {
        canvas.set_fill_color(*p.lighten);
        canvas.fill_rounded_rect(bg.x, bg.y, bg.width, bg.height, p.radius);
    }
    canvas.set_fill_color(p.background);
    canvas.fill_rounded_rect(bg.x, bg.y, bg.width, bg.height, p.radius);
    if (p.border) {
        canvas.set_stroke_color(*p.border);
        canvas.set_line_width(1.0f);
        canvas.stroke_rounded_rect(p.border_rect.x, p.border_rect.y, p.border_rect.width,
                                   p.border_rect.height, p.radius);
    }
    if (p.accent_fill) {
        canvas.set_fill_color(*p.accent_fill);
        canvas.fill_rounded_rect(p.box.x, p.box.y, p.box.width, p.box.height, p.radius);
    }
    if (p.dash) {
        canvas.set_fill_color(p.mark);
        canvas.fill_rounded_rect(p.dash->x, p.dash->y, p.dash->width, p.dash->height,
                                 std::min(p.radius, 0.5f * p.dash->height));
    } else if (p.has_check) {
        const canvas::Canvas::Point2D pts[3] = {{p.check[0].x, p.check[0].y},
                                                {p.check[1].x, p.check[1].y},
                                                {p.check[2].x, p.check[2].y}};
        canvas.set_stroke_color(p.mark);
        canvas.set_line_width(p.check_width);
        canvas.set_line_cap(canvas::LineCap::butt);
        canvas.set_line_join(canvas::LineJoin::miter);
        canvas.stroke_path(pts, 3);
    } else if (p.dot) {
        canvas.set_fill_color(p.mark);
        canvas.fill_circle(p.dot->center_x(), p.dot->center_y(), 0.5f * p.dot->width);
    }
}

void Checkbox::paint(canvas::Canvas& canvas) {
    if (skin_ == Skin::chromium_refresh) {
        paint_chromium_refresh_(canvas);
        return;
    }
    auto b = local_bounds();
    float size = std::min(b.width, b.height);
    float cx = b.width * 0.5f;
    float cy = b.height * 0.5f;
    float r = size * 0.35f; // smaller radius to avoid clipping at bounds edge

    // Rounded SQUARE (matches the Figma design language — not a circle).
    float corner = r * 0.35f;
    if (checked_) {
        // Filled rounded square
        auto fill = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
        canvas.set_fill_color(fill);
        canvas.fill_rounded_rect(cx - r, cy - r, r * 2, r * 2, corner);
        // Check glyph (simple checkmark using lines)
        // clang-format off
        canvas.set_stroke_color(canvas::Color::rgba8(30, 30, 40)); // token-lint:allow -- check-glyph ink on the control.fill face; no theme token carries it
        // clang-format on
        canvas.set_line_width(2.0f);
        canvas.stroke_line(cx - r * 0.35f, cy, cx - r * 0.05f, cy + r * 0.3f);
        canvas.stroke_line(cx - r * 0.05f, cy + r * 0.3f, cx + r * 0.4f, cy - r * 0.3f);
    } else {
        // Stroked rounded square
        auto border = resolve_color("control.border", canvas::Color::rgba8(80, 80, 100));
        canvas.set_stroke_color(border);
        canvas.set_line_width(1.5f);
        canvas.stroke_rounded_rect(cx - r, cy - r, r * 2, r * 2, corner);
    }
}

void Checkbox::on_mouse_down(Point) {
    if (indeterminate_)
        set_indeterminate(false);
    if (kind_ == Kind::radio) {
        if (checked_)
            return; // clicking the selected radio keeps it selected
        set_checked(true);
        if (radio_group_ != 0) {
            if (auto* p = parent()) {
                for (size_t i = 0; i < p->child_count(); ++i) {
                    auto* sib = dynamic_cast<Checkbox*>(p->child_at(i));
                    if (sib && sib != this && sib->kind_ == Kind::radio &&
                        sib->radio_group_ == radio_group_ && sib->checked_) {
                        sib->set_checked(false);
                        if (sib->on_change)
                            sib->on_change(false);
                    }
                }
            }
        }
        if (on_change)
            on_change(true);
        return;
    }
    set_checked(!checked_);
    if (on_change)
        on_change(checked_);
}

} // namespace pulp::view
