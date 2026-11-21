#include <pulp/vellum_audio/audio_widgets.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace pulp::vellum_audio {

void LevelMeter::set_feed(AudioDisplayProvider* feed, std::size_t channel) noexcept {
    feed_ = feed;
    channel_ = channel;
    last_publish_ = 0;
    quiet_ticks_ = 0;
    stale_ = true;
    rms_ = peak_ = 0.0f;
    ballistics_ = {};
}

void LevelMeter::set_levels(float primary, float secondary) {
    rms_ = std::clamp(primary, 0.0f, 1.0f);
    peak_ = std::clamp(secondary, 0.0f, 1.0f);
    (void)ballistics_.update(peak_, rms_, 1.0f / 60.0f);
    stale_ = false;
    request_repaint();
}

::vellum::canvas::Color LevelMeter::level_color(float level) const {
    if (level > 0.9f)
        return resolve_color("meter.red", ::vellum::canvas::Color::rgba8(240, 60, 60));
    if (level > 0.7f)
        return resolve_color("meter.yellow", ::vellum::canvas::Color::rgba8(240, 180, 60));
    return resolve_color("meter.green", ::vellum::canvas::Color::rgba8(80, 200, 80));
}

MidiKeyboard::MidiKeyboard() {
    set_focusable(true);
    set_access_role(::vellum::view::View::AccessRole::group);
}

void MidiKeyboard::set_range(int first, int last) {
    first_note_ = std::clamp(first, 0, 127);
    last_note_ = std::clamp(last, first_note_, 127);
    request_repaint();
}

void MidiKeyboard::note_on(int note, float velocity) {
    if (note >= 0 && note < 128)
        note_states_[note] = {velocity, true};
    request_repaint();
}

void MidiKeyboard::note_off(int note) {
    if (note >= 0 && note < 128)
        note_states_[note] = {0.0f, false};
    request_repaint();
}

void MidiKeyboard::all_notes_off() {
    for (auto& state : note_states_)
        state = {};
    request_repaint();
}

bool MidiKeyboard::is_note_on(int note) const {
    return note >= 0 && note < 128 && note_states_[note].active;
}

bool MidiKeyboard::is_black_key(int note) const {
    const int pitch_class = note % 12;
    return pitch_class == 1 || pitch_class == 3 || pitch_class == 6 || pitch_class == 8 ||
           pitch_class == 10;
}

int MidiKeyboard::white_key_count() const {
    int count = 0;
    for (int note = first_note_; note <= last_note_; ++note)
        count += !is_black_key(note);
    return count;
}

float MidiKeyboard::white_key_width() const {
    const int count = white_key_count();
    if (count == 0)
        return 0.0f;
    const auto bounds = local_bounds();
    return orientation_ == Orientation::horizontal ? bounds.width / count : bounds.height / count;
}

::vellum::view::Rect MidiKeyboard::key_rect(int note) const {
    if (note < first_note_ || note > last_note_)
        return {};
    const auto bounds = local_bounds();
    if (orientation_ == Orientation::vertical_chromatic_rows) {
        const float rows = static_cast<float>(last_note_ - first_note_ + 1);
        const int row = last_note_ - note;
        const float top = bounds.height * row / rows;
        return {0, top, bounds.width, bounds.height / rows};
    }
    const float white_width = white_key_width();
    int white_index = 0;
    for (int n = first_note_; n < note; ++n)
        white_index += !is_black_key(n);
    const bool horizontal = orientation_ == Orientation::horizontal;
    if (!is_black_key(note)) {
        return horizontal
                   ? ::vellum::view::Rect{white_index * white_width, 0, white_width, bounds.height}
                   : ::vellum::view::Rect{0, white_index * white_width, bounds.width, white_width};
    }
    const float black_width = white_width * 0.6f;
    const float black_height = (horizontal ? bounds.height : bounds.width) * 0.6f;
    const float offset = white_width - black_width * 0.5f;
    return horizontal ? ::vellum::view::Rect{white_index * white_width + offset - white_width, 0,
                                             black_width, black_height}
                      : ::vellum::view::Rect{0, white_index * white_width + offset - white_width,
                                             black_height, black_width};
}

int MidiKeyboard::note_at_position(::vellum::view::Point pos) const {
    for (int note = last_note_; note >= first_note_; --note)
        if (is_black_key(note)) {
            const auto rect = key_rect(note);
            if (pos.x >= rect.x && pos.x < rect.x + rect.width && pos.y >= rect.y &&
                pos.y < rect.y + rect.height)
                return note;
        }
    for (int note = first_note_; note <= last_note_; ++note)
        if (!is_black_key(note)) {
            const auto rect = key_rect(note);
            if (pos.x >= rect.x && pos.x < rect.x + rect.width && pos.y >= rect.y &&
                pos.y < rect.y + rect.height)
                return note;
        }
    return -1;
}

void MidiKeyboard::paint(::vellum::canvas::Canvas& canvas) {
    const auto active =
        highlight_color_.a > 0
            ? highlight_color_
            : resolve_color("accent.primary", ::vellum::canvas::Color::rgba8(100, 160, 255));
    const auto border =
        resolve_color("control.border", ::vellum::canvas::Color::rgba8(100, 100, 100));
    const auto text =
        resolve_color("text.secondary", ::vellum::canvas::Color::rgba8(120, 120, 120));
    for (int note = first_note_; note <= last_note_; ++note) {
        if (is_black_key(note))
            continue;
        const auto rect = key_rect(note);
        canvas.set_fill_color(is_note_on(note) ? active
                                               : ::vellum::canvas::Color::rgba8(236, 239, 243));
        canvas.fill_rounded_rect(rect.x, rect.y, rect.width, rect.height, 2.0f);
        canvas.set_stroke_color(border);
        canvas.set_line_width(0.5f);
        canvas.stroke_rounded_rect(rect.x, rect.y, rect.width, rect.height, 2.0f);
        if (show_names_ && note % 12 == 0) {
            canvas.set_fill_color(text);
            canvas.set_font("", 9.0f);
            canvas.fill_text("C" + std::to_string(note / 12 - 1), rect.x + 2,
                             rect.y + rect.height - 12);
        }
    }
    for (int note = first_note_; note <= last_note_; ++note) {
        if (!is_black_key(note))
            continue;
        const auto rect = key_rect(note);
        canvas.set_fill_color(is_note_on(note) ? active
                                               : ::vellum::canvas::Color::rgba8(22, 26, 33));
        canvas.fill_rounded_rect(rect.x, rect.y, rect.width, rect.height, 2.0f);
    }
}

void MidiKeyboard::on_mouse_down(::vellum::view::Point pos) {
    const int note = note_at_position(pos);
    if (note < 0)
        return;
    mouse_note_ = note;
    note_on(note, 0.8f);
    if (on_note_on)
        on_note_on(note, 0.8f);
}

void MidiKeyboard::on_mouse_drag(::vellum::view::Point pos) {
    const int note = note_at_position(pos);
    if (note == mouse_note_)
        return;
    if (mouse_note_ >= 0) {
        note_off(mouse_note_);
        if (on_note_off)
            on_note_off(mouse_note_);
    }
    mouse_note_ = note;
    if (note >= 0) {
        note_on(note, 0.8f);
        if (on_note_on)
            on_note_on(note, 0.8f);
    }
}

void MidiKeyboard::on_mouse_up(::vellum::view::Point) {
    if (mouse_note_ >= 0) {
        note_off(mouse_note_);
        if (on_note_off)
            on_note_off(mouse_note_);
    }
    mouse_note_ = -1;
}

void MidiKeyboard::on_mouse_event(const ::vellum::view::MouseEvent& event) {
    ::vellum::view::View::on_mouse_event(event);
}

void LevelMeter::set_level_fill(std::optional<::vellum::canvas::Color> background,
                                std::vector<::vellum::canvas::Color> gradient) {
    if (background)
        background_ = *background;
    if (!gradient.empty())
        fill_ = gradient.back();
    request_repaint();
}

void LevelMeter::clear_level_fill() {
    background_ = {0.08f, 0.08f, 0.1f, 1.0f};
    fill_ = {0.25f, 0.85f, 0.35f, 1.0f};
    request_repaint();
}

void LevelMeter::sample() noexcept {
    if (feed_ == nullptr)
        return;
    const auto published = feed_->publish_count(channel_);
    if (published == 0) { // never published
        stale_ = true;
        rms_ = peak_ = 0.0f;
        return;
    }
    if (published == last_publish_) {
        ++quiet_ticks_;
        if (quiet_ticks_ >= 8) {
            stale_ = true;
            rms_ = peak_ = 0.0f;
        }
        (void)ballistics_.update(0.0f, 0.0f, 1.0f / 60.0f);
        return;
    }
    std::array<float, AudioDisplayProvider::kMaxMeterChannels> rms{};
    std::array<float, AudioDisplayProvider::kMaxMeterChannels> peak{};
    AudioFrameInfo next_info;
    if (feed_->read_meter(channel_, rms, peak, next_info) != 0) {
        rms_ = std::clamp(rms[0], 0.0f, 1.0f);
        peak_ = std::clamp(peak[0], 0.0f, 1.0f);
        (void)ballistics_.update(peak_, rms_, 1.0f / 60.0f);
        stale_ = false;
        info_ = next_info;
        last_publish_ = published;
        quiet_ticks_ = 0;
        request_repaint();
        return;
    }
    // A frame with zero channels is not consumed: stay stale and retry next tick.
    stale_ = true;
    rms_ = peak_ = 0.0f;
    (void)ballistics_.update(0.0f, 0.0f, 1.0f / 60.0f);
}

void LevelMeter::paint(::vellum::canvas::Canvas& canvas) {
    const auto b = local_bounds();
    canvas.set_fill_color(background_);
    canvas.fill_rect(0, 0, b.width, b.height);
    const bool vertical = orientation_ == Orientation::vertical;
    const float length = vertical ? b.height : b.width;
    const float level = std::clamp(ballistics_.display_rms, 0.0f, 1.0f);
    const float fill = std::round(level * length);
    canvas.set_fill_color(level_color(level));
    const float cross = vertical ? b.width * std::clamp(bar_ratio_, 0.05f, 1.0f) : b.height;
    if (vertical)
        canvas.fill_rect((b.width - cross) * 0.5f, b.height - fill, cross, fill);
    else
        canvas.fill_rect(0, (b.height - cross) * 0.5f, fill, cross);
    canvas.set_stroke_color({1.0f, 1.0f, 1.0f, 1.0f});
    canvas.set_line_width(1.0f);
    const float peak_pos = std::round(std::clamp(ballistics_.display_peak, 0.0f, 1.0f) * length);
    if (vertical)
        canvas.stroke_line(0, b.height - peak_pos, b.width, b.height - peak_pos);
    else
        canvas.stroke_line(peak_pos, 0, peak_pos, b.height);
    const float held_pos = std::round(std::clamp(ballistics_.held_peak, 0.0f, 1.0f) * length);
    canvas.set_stroke_color(
        ballistics_.held_peak > 0.9f
            ? resolve_color("accent.error", ::vellum::canvas::Color::rgba8(255, 50, 50))
            : ::vellum::canvas::Color::rgba8(255, 100, 100));
    canvas.set_line_width(2.0f);
    if (vertical)
        canvas.stroke_line(0, b.height - held_pos, b.width, b.height - held_pos);
    else
        canvas.stroke_line(held_pos, 0, held_pos, b.height);
}

void AnalyzerScope::set_feed(AudioDisplayProvider* feed, std::size_t channel) noexcept {
    feed_ = feed;
    channel_ = channel;
    last_publish_ = 0;
    quiet_ticks_ = 0;
    stale_ = true;
    column_count_ = 0; // nothing drawn until a frame arrives; mapped_columns_ is kept
}

void AnalyzerScope::configure_mapping(const pulp::signal::SpectrumBinMappingConfig& config) {
    mapping_.configure(config);
    mapped_columns_ = std::min(config.columns, columns_.size());
    column_count_ = 0;
}

bool AnalyzerScope::map_bins_(const float* values, std::size_t count) noexcept {
    if (mapping_.valid()) {
        // A configured mapping owns the column count; a frame of any other
        // size is dropped rather than silently bypassing the mapping.
        if (count != mapping_.bin_count())
            return false;
        mapping_.map({values, count}, {columns_.data(), mapped_columns_},
                     pulp::signal::SpectrumAggregation::peak);
        column_count_ = mapped_columns_;
        return true;
    }
    column_count_ = std::min(count, columns_.size());
    std::copy_n(values, column_count_, columns_.begin());
    return true;
}

void AnalyzerScope::set_bins(const float* values, std::size_t count) {
    if (!map_bins_(values, count))
        return;
    stale_ = false;
    request_repaint();
}

void AnalyzerScope::sample() noexcept {
    if (feed_ == nullptr)
        return;
    const auto published = feed_->publish_count(channel_);
    if (published == 0) { // never published
        stale_ = true;
        return;
    }
    if (published == last_publish_) {
        ++quiet_ticks_;
        if (quiet_ticks_ >= 8)
            stale_ = true;
        return;
    }
    AudioFrameInfo next_info;
    const auto count = feed_->read_vector(channel_, bins_, next_info);
    if (!map_bins_(bins_.data(), count)) {
        stale_ = true;
        return;
    }
    info_ = next_info;
    stale_ = false;
    last_publish_ = published;
    quiet_ticks_ = 0;
    request_repaint();
}

void AnalyzerScope::paint(::vellum::canvas::Canvas& canvas) {
    const auto b = local_bounds();
    canvas.set_fill_color({0.04f, 0.05f, 0.07f, 1.0f});
    canvas.fill_rect(0, 0, b.width, b.height);
    if (stale_ || column_count_ == 0)
        return;
    const float floor = info_.floor_db;
    const float span = std::max(1.0f, -floor);
    canvas.set_stroke_color({0.3f, 0.85f, 1.0f, 1.0f});
    canvas.set_line_width(1.0f);
    float previous_x = 0.0f;
    float previous_y = b.height;
    for (std::size_t i = 0; i < column_count_; ++i) {
        const float x = column_count_ == 1 ? 0.0f
                                           : b.width * static_cast<float>(i) /
                                                 static_cast<float>(column_count_ - 1);
        const float normalized = std::clamp((columns_[i] - floor) / span, 0.0f, 1.0f);
        const float y = b.height * (1.0f - normalized);
        if (i != 0)
            canvas.stroke_line(previous_x, previous_y, x, y);
        previous_x = x;
        previous_y = y;
    }
}

} // namespace pulp::vellum_audio
