#include "pulp_delay_controls.hpp"
#include "pulp_delay_paint_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace pulp::examples::delay::ui {

namespace {

using paint_detail::text;
using paint_detail::with_alpha;

std::string numeric_value(float raw, const std::string& unit) {
    char buffer[64]{};
    if (unit == "%") {
        std::snprintf(buffer, sizeof(buffer), "%.0f%%", raw);
    } else if (unit == "ms") {
        std::snprintf(buffer, sizeof(buffer), "%.0f ms", raw);
    } else if (unit == "Hz") {
        if (raw >= 1000.0f)
            std::snprintf(buffer, sizeof(buffer), "%.1f kHz", raw / 1000.0f);
        else if (raw >= 10.0f)
            std::snprintf(buffer, sizeof(buffer), "%.0f Hz", raw);
        else
            std::snprintf(buffer, sizeof(buffer), "%.2f Hz", raw);
    } else if (unit == "x") {
        std::snprintf(buffer, sizeof(buffer), "%.2fx", raw);
    } else if (unit == "Q") {
        std::snprintf(buffer, sizeof(buffer), "%.2f Q", raw);
    } else if (std::abs(raw) >= 100.0f) {
        std::snprintf(buffer, sizeof(buffer), "%.0f%s%s", raw,
                      unit.empty() ? "" : " ", unit.c_str());
    } else if (std::abs(raw) >= 10.0f) {
        std::snprintf(buffer, sizeof(buffer), "%.1f%s%s", raw,
                      unit.empty() ? "" : " ", unit.c_str());
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f%s%s", raw,
                      unit.empty() ? "" : " ", unit.c_str());
    }
    return buffer;
}

} // namespace

std::string format_parameter_value(const state::StateStore& store,
                                   state::ParamID id,
                                   float normalized) {
    const auto* info = store.info(id);
    if (!info)
        return {};

    const float raw = info->range.denormalize(
        std::clamp(normalized, 0.0f, 1.0f));
    if (!info->value_labels.empty()) {
        const float step = info->range.step > 0.0f ? info->range.step : 1.0f;
        const int index = std::clamp(
            static_cast<int>(std::lround((raw - info->range.min) / step)),
            0, static_cast<int>(info->value_labels.size()) - 1);
        return info->value_labels[static_cast<std::size_t>(index)];
    }
    if (info->to_string)
        return info->to_string(raw);
    return numeric_value(raw, info->unit);
}

DelayKnob::DelayKnob(state::StateStore& store,
                     const CharacterPalette& palette,
                     state::ParamID id,
                     std::string caption)
    : store_(&store), palette_(&palette), id_(id), caption_(std::move(caption)) {
    set_id("delay-param-" + std::to_string(id_));
    set_label(caption_);
    set_show_label(false);
    set_show_value(false);
    set_render_style(view::WidgetRenderStyle::standard);
}

float DelayKnob::pointer_angle() const noexcept {
    return view::Knob::start_angle
        + (view::Knob::end_angle - view::Knob::start_angle) * value();
}

DelayKnob::DialGeometry DelayKnob::dial_geometry() const noexcept {
    const auto b = local_bounds();
    constexpr float footer = 35.0f;
    const float radius =
        std::max(15.0f, std::min(b.width * 0.36f, (b.height - footer) * 0.40f));
    return {
        .center_x = b.width * 0.5f,
        .center_y = std::max(radius + 8.0f, (b.height - footer) * 0.49f),
        .body_radius = radius,
        .arc_radius = radius + 4.0f,
    };
}

void DelayKnob::sync_from_store(state::StateStore& store) {
    set_value(store.get_normalized(id_));
}

std::string DelayKnob::display_text() const {
    return store_ ? format_parameter_value(*store_, id_, value()) : std::string{};
}

void DelayKnob::paint(canvas::Canvas& c) {
    const auto b = local_bounds();
    const auto geometry = dial_geometry();
    const float radius = geometry.body_radius;
    const float cx = geometry.center_x;
    const float cy = geometry.center_y;
    const float angle = pointer_angle();

    if (has_focus()) {
        c.set_stroke_color(with_alpha(palette_->accent(), 0.78f));
        c.set_line_width(2.0f);
        c.stroke_circle(cx, cy, radius + 10.0f);
    } else if (is_hovered()) {
        c.set_stroke_color(with_alpha(palette_->soft(), 0.42f));
        c.set_line_width(1.5f);
        c.stroke_circle(cx, cy, radius + 8.0f);
    }

    c.set_line_cap(canvas::LineCap::round);
    c.set_stroke_color(color::track);
    c.set_line_width(5.0f);
    c.stroke_arc(cx, cy, geometry.arc_radius,
                 view::Knob::start_angle, view::Knob::end_angle);

    if (value() > 0.0001f) {
        c.set_stroke_color(palette_->accent());
        c.set_line_width(5.0f);
        c.stroke_arc(cx, cy, geometry.arc_radius,
                     view::Knob::start_angle, angle);
    }

    c.set_fill_color(color::black);
    c.fill_circle(cx, cy, radius);
    c.set_fill_color(color::raised);
    c.fill_circle(cx - radius * 0.05f, cy - radius * 0.08f, radius * 0.82f);
    c.set_stroke_color(color::border_bright);
    c.set_line_width(1.0f);
    c.stroke_circle(cx, cy, radius);

    const float pointer_inner = radius * 0.22f;
    const float pointer_outer = radius * 0.72f;
    c.set_stroke_color(palette_->accent());
    c.set_line_width(2.4f);
    c.set_line_cap(canvas::LineCap::round);
    c.stroke_line(cx + std::cos(angle) * pointer_inner,
                  cy + std::sin(angle) * pointer_inner,
                  cx + std::cos(angle) * pointer_outer,
                  cy + std::sin(angle) * pointer_outer);
    c.set_fill_color(palette_->accent());
    c.fill_circle(cx + std::cos(angle) * pointer_outer,
                  cy + std::sin(angle) * pointer_outer, 2.2f);

    text(c, display_text(), cx, b.height - 19.0f, 11.0f,
         color::ink, canvas::TextAlign::center, 650);
    text(c, caption_, cx, b.height - 5.0f, 8.5f,
         color::muted, canvas::TextAlign::center, 600, 0.8f);
}

void DelayKnob::on_focus_changed(bool gained) {
    view::View::on_focus_changed(gained);
    request_repaint();
}

DelayFader::DelayFader(state::StateStore& store,
                       const CharacterPalette& palette,
                       state::ParamID id,
                       std::string caption)
    : store_(&store), palette_(&palette), id_(id), caption_(std::move(caption)) {
    set_id("delay-param-" + std::to_string(id_));
    set_label(caption_);
    set_orientation(view::Fader::Orientation::horizontal);
    set_render_style(view::WidgetRenderStyle::standard);
}

std::string DelayFader::display_text() const {
    return store_ ? format_parameter_value(*store_, id_, value()) : std::string{};
}

void DelayFader::sync_from_store(state::StateStore& store) {
    set_value(store.get_normalized(id_));
}

void DelayFader::paint(canvas::Canvas& c) {
    const auto b = local_bounds();
    const float x0 = 2.0f;
    const float x1 = std::max(x0, b.width - 2.0f);
    const float y = b.height - 8.0f;
    const float thumb_x = x0 + (x1 - x0) * value();

    if (has_focus()) {
        c.set_stroke_color(with_alpha(palette_->accent(), 0.72f));
        c.set_line_width(1.0f);
        c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f,
                              b.height - 1.0f, metric::control_radius);
    }

    text(c, caption_, x0, 11.0f, 8.5f, color::muted,
         canvas::TextAlign::left, 600, 0.65f);
    text(c, display_text(), x1, 11.0f, 9.5f, color::ink,
         canvas::TextAlign::right, 600);

    c.set_line_cap(canvas::LineCap::round);
    c.set_stroke_color(color::track);
    c.set_line_width(4.0f);
    c.stroke_line(x0, y, x1, y);
    if (value() > 0.0001f) {
        c.set_stroke_color(palette_->accent());
        c.stroke_line(x0, y, thumb_x, y);
    }
    c.set_fill_color(color::panel_deep);
    c.fill_circle(thumb_x, y, 4.7f);
    c.set_stroke_color(palette_->accent());
    c.set_line_width(1.4f);
    c.stroke_circle(thumb_x, y, 4.7f);
}

void DelayFader::on_focus_changed(bool gained) {
    view::View::on_focus_changed(gained);
    request_repaint();
}

DelayTapField::DelayTapField(state::StateStore& store,
                             const CharacterPalette& palette,
                             state::ParamID time_id,
                             state::ParamID feedback_id,
                             state::ParamID sync_id,
                             state::ParamID division_id)
    : DelayFader(store, palette, time_id, "TIME"), feedback_id_(feedback_id),
      sync_id_(sync_id), division_id_(division_id) {
    feedback_listener_ = store.add_listener(
        [this](state::ParamID changed, float) {
            if (changed == feedback_id_ || changed == sync_id_ || changed == division_id_)
                request_repaint();
        },
        state::ListenerThread::Main);
}

std::string DelayTapField::timing_display_text() const {
    if (store_->get_value(sync_id_) < 0.5f)
        return display_text();
    return "SYNC " + format_parameter_value(
                         *store_, division_id_, store_->get_normalized(division_id_));
}

void DelayTapField::paint(canvas::Canvas& c) {
    const auto b = local_bounds();
    const float feedback = store_->get_normalized(feedback_id_);
    const bool sync = store_->get_value(sync_id_) >= 0.5f;
    const float time = sync ? store_->get_normalized(division_id_) : value();

    c.set_fill_color(color::panel_deep);
    c.fill_rounded_rect(0, 0, b.width, b.height, 6.0f);
    c.set_stroke_color(color::border);
    c.set_line_width(1.0f);
    c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f,
                          b.height - 1.0f, 6.0f);

    text(c, timing_display_text(), 16.0f, 28.0f, 22.0f, palette_->accent(),
         canvas::TextAlign::left, 700, -0.4f);
    text(c, format_parameter_value(*store_, feedback_id_, feedback),
         b.width - 16.0f, 27.0f, 15.0f, color::ink,
         canvas::TextAlign::right, 650);
    text(c, sync ? "HOST-SYNCED DELAY" : "DELAY TIME", 16.0f, 42.0f, 8.0f, color::muted,
         canvas::TextAlign::left, 600, 0.9f);
    text(c, "FEEDBACK", b.width - 16.0f, 42.0f, 8.0f, color::muted,
         canvas::TextAlign::right, 600, 0.9f);

    const float left = 16.0f;
    const float right = b.width - 16.0f;
    const float top = 54.0f;
    const float bottom = b.height - 29.0f;
    c.set_stroke_color(with_alpha(color::border, 0.72f));
    c.set_line_width(1.0f);
    for (int row = 0; row <= 4; ++row) {
        const float y = top + (bottom - top) * static_cast<float>(row) / 4.0f;
        c.stroke_line(left, y, right, y);
    }
    for (int column = 0; column <= 12; ++column) {
        const float x = left + (right - left) * static_cast<float>(column) / 12.0f;
        c.stroke_line(x, top, x, bottom);
    }

    const float mid = top + (bottom - top) * 0.58f;
    c.set_stroke_color(color::faint);
    c.set_line_width(1.0f);
    c.stroke_line(left, mid, right, mid);

    const float spacing = 31.0f + time * 42.0f;
    int tap = 0;
    for (float x = left + 22.0f; x < right - 8.0f && tap < 18;
         x += spacing, ++tap) {
        const float decay = tap == 0
            ? 1.0f
            : std::pow(std::max(0.015f, feedback),
                       0.30f + static_cast<float>(tap) * 0.27f);
        const float height = 16.0f + decay * (bottom - top) * 0.58f;
        const float y = mid - height * 0.5f;
        c.set_fill_color(with_alpha(palette_->accent(), 0.20f + decay * 0.78f));
        c.fill_rounded_rect(x, y, tap == 0 ? 8.0f : 5.0f,
                            height, 2.5f);
        c.set_stroke_color(with_alpha(palette_->soft(), 0.18f + decay * 0.45f));
        c.set_line_width(1.0f);
        const float next_x = std::min(right - 4.0f, x + spacing);
        c.stroke_line(x + 5.0f, mid, next_x, mid);
    }
    const float marker = left + (right - left) * time;
    c.set_stroke_color(with_alpha(palette_->accent(), 0.55f));
    c.set_line_width(1.0f);
    c.stroke_line(marker, top, marker, bottom);

    const float track_y = b.height - 13.0f;
    c.set_line_cap(canvas::LineCap::round);
    c.set_stroke_color(color::track);
    c.set_line_width(4.0f);
    c.stroke_line(left, track_y, right, track_y);
    c.set_stroke_color(palette_->accent());
    c.stroke_line(left, track_y, marker, track_y);
    c.set_fill_color(color::panel_deep);
    c.fill_circle(marker, track_y, 5.0f);
    c.set_stroke_color(palette_->accent());
    c.set_line_width(has_focus() ? 2.0f : 1.3f);
    c.stroke_circle(marker, track_y, 5.0f);
}

DelayChoice::DelayChoice(state::StateStore& store,
                         const CharacterPalette& palette,
                         state::ParamID id,
                         std::string caption,
                         std::vector<std::string> labels)
    : store_(&store),
      palette_(&palette),
      id_(id),
      caption_(std::move(caption)),
      labels_(std::move(labels)) {
    set_id("delay-param-" + std::to_string(id_));
    set_focusable(true);
    set_access_role(view::View::AccessRole::combo_box);
    if (const auto* info = store.info(id_))
        set_access_label(info->name);
    sync_access_value();
    listener_ = store.add_listener(
        [this](state::ParamID changed, float) {
            if (changed != id_)
                return;
            if (sync_access_value())
                request_repaint();
        },
        state::ListenerThread::Main);
}

float DelayChoice::normalized_value() const noexcept {
    return store_->get_normalized(id_);
}

void DelayChoice::sync_from_store(state::StateStore&) {
    if (sync_access_value())
        request_repaint();
}

int DelayChoice::selected_index() const noexcept {
    const auto* info = store_->info(id_);
    if (!info || labels_.empty())
        return 0;
    const float step = info->range.step > 0.0f ? info->range.step : 1.0f;
    return std::clamp(
        static_cast<int>(std::lround(
            (store_->get_value(id_) - info->range.min) / step)),
        0, static_cast<int>(labels_.size()) - 1);
}

bool DelayChoice::sync_access_value() {
    if (labels_.empty())
        return false;
    const auto& next = labels_[static_cast<std::size_t>(selected_index())];
    if (access_value() == next)
        return false;
    set_access_value(next);
    return true;
}

void DelayChoice::paint(canvas::Canvas& c) {
    const auto b = local_bounds();
    const float caption_height = caption_.empty() ? 0.0f : 12.0f;
    const float control_y = caption_height;
    const float control_h = std::max(1.0f, b.height - control_y);
    const int selected = selected_index();
    const float segment_width = labels_.empty()
        ? b.width
        : b.width / static_cast<float>(labels_.size());

    if (!caption_.empty()) {
        text(c, caption_, 1.0f, 9.0f, 7.8f, color::muted,
             canvas::TextAlign::left, 600, 0.6f);
    }
    c.set_fill_color(color::panel_deep);
    c.fill_rounded_rect(0, control_y, b.width, control_h, 4.0f);

    for (std::size_t index = 0; index < labels_.size(); ++index) {
        const float x = static_cast<float>(index) * segment_width;
        const bool active = static_cast<int>(index) == selected;
        if (active) {
            c.set_fill_color(palette_->accent());
            c.fill_rounded_rect(x + 1.0f, control_y + 1.0f,
                                segment_width - 2.0f, control_h - 2.0f, 3.0f);
        } else if (index > 0) {
            c.set_stroke_color(color::border);
            c.set_line_width(1.0f);
            c.stroke_line(x, control_y + 4.0f, x,
                          control_y + control_h - 4.0f);
        }
        const float font_size = labels_.size() > 8 ? 7.4f : 8.8f;
        text(c, labels_[index], x + segment_width * 0.5f,
             control_y + control_h * 0.64f, font_size,
             active ? color::black : color::text,
             canvas::TextAlign::center, active ? 750 : 550,
             labels_.size() > 8 ? -0.25f : 0.15f);
    }

    c.set_stroke_color(has_focus() ? palette_->accent() : color::border);
    c.set_line_width(has_focus() ? 1.5f : 1.0f);
    c.stroke_rounded_rect(0.5f, control_y + 0.5f, b.width - 1.0f,
                          control_h - 1.0f, 4.0f);
}

void DelayChoice::on_mouse_down(view::Point position) {
    if (labels_.empty() || bounds().width <= 0.0f)
        return;
    const int index = std::clamp(
        static_cast<int>(position.x / bounds().width
                         * static_cast<float>(labels_.size())),
        0, static_cast<int>(labels_.size()) - 1);
    const auto* info = store_->info(id_);
    if (!info)
        return;
    const float step = info->range.step > 0.0f ? info->range.step : 1.0f;
    const float raw = info->range.min + static_cast<float>(index) * step;
    store_->begin_gesture(id_);
    store_->set_value(id_, raw);
    store_->end_gesture(id_);
}

void DelayChoice::on_focus_changed(bool gained) {
    view::View::on_focus_changed(gained);
    request_repaint();
}

DelayActionCard::DelayActionCard(const CharacterPalette& palette,
                                 state::ParamID id,
                                 std::string caption,
                                 std::string subtitle,
                                 std::string glyph,
                                 bool compact,
                                 bool warning)
    : palette_(&palette),
      id_(id),
      caption_(std::move(caption)),
      subtitle_(std::move(subtitle)),
      glyph_(std::move(glyph)),
      compact_(compact),
      warning_(warning) {
    set_id("delay-param-" + std::to_string(id_));
    set_label(caption_);
}

void DelayActionCard::sync_from_store(state::StateStore& store) {
    set_on(store.get_value(id_) >= 0.5f);
}

void DelayActionCard::paint(canvas::Canvas& c) {
    const auto b = local_bounds();
    const bool active = is_on();
    const auto accent = warning_ ? color::warning : palette_->accent();
    const auto accent_dim = warning_
        ? color::warning.interpolate(color::panel_deep, 0.68f)
        : palette_->dim();
    c.set_fill_color(active
        ? with_alpha(accent_dim, 0.88f)
        : color::panel);
    c.fill_rounded_rect(0, 0, b.width, b.height, metric::panel_radius);
    c.set_stroke_color(
        has_focus() || active ? accent : color::border);
    c.set_line_width(has_focus() ? 2.0f : 1.0f);
    c.stroke_rounded_rect(0.5f, 0.5f, b.width - 1.0f,
                          b.height - 1.0f, metric::panel_radius);

    if (compact_) {
        c.set_fill_color(active ? accent : color::track);
        c.fill_circle(13.0f, b.height * 0.5f, 4.0f);
        text(c, caption_, 23.0f, b.height * 0.64f, 8.4f,
             active ? accent : color::text,
             canvas::TextAlign::left, 700, 0.35f);
        return;
    }

    c.set_fill_color(active ? accent : color::raised);
    c.fill_circle(34.0f, b.height * 0.5f, 18.0f);
    text(c, glyph_, 34.0f, b.height * 0.5f + 5.0f, 15.0f,
         active ? color::black : accent,
         canvas::TextAlign::center, 750);
    text(c, caption_, 64.0f, 30.0f, 13.0f,
         active ? accent : color::ink,
         canvas::TextAlign::left, 750, 1.0f);
    text(c, subtitle_, 64.0f, 48.0f, 8.5f, color::muted,
         canvas::TextAlign::left, 550, 0.45f);
    text(c, active ? "ON" : "OFF", b.width - 18.0f, 39.0f, 9.0f,
         active ? accent : color::muted,
         canvas::TextAlign::right, 700, 0.8f);
}

void DelayActionCard::on_focus_changed(bool gained) {
    view::View::on_focus_changed(gained);
    request_repaint();
}

} // namespace pulp::examples::delay::ui
