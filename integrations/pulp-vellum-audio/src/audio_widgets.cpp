#include <pulp/vellum_audio/audio_widgets.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::vellum_audio {

void LevelMeter::set_feed(AudioDisplayProvider* feed, std::size_t channel) noexcept {
    feed_ = feed;
    channel_ = channel;
    last_publish_ = 0;
    quiet_ticks_ = 0;
    stale_ = true;
    rms_ = peak_ = 0.0f;
}

void LevelMeter::set_levels(float primary, float secondary) {
    rms_ = std::clamp(primary, 0.0f, 1.0f);
    peak_ = std::clamp(secondary, 0.0f, 1.0f);
    stale_ = false;
    request_repaint();
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
    AudioFrameInfo next_info;
    const bool has_frame = feed_->read_info(channel_, next_info);
    if (!has_frame) {
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
        return;
    }
    std::array<float, AudioDisplayProvider::kMaxMeterChannels> rms{};
    std::array<float, AudioDisplayProvider::kMaxMeterChannels> peak{};
    if (feed_->read_meter(channel_, rms, peak) != 0) {
        rms_ = std::clamp(rms[0], 0.0f, 1.0f);
        peak_ = std::clamp(peak[0], 0.0f, 1.0f);
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
}

void LevelMeter::paint(::vellum::canvas::Canvas& canvas) {
    const auto b = local_bounds();
    canvas.set_fill_color(background_);
    canvas.fill_rect(0, 0, b.width, b.height);
    const bool vertical = orientation_ == Orientation::vertical;
    const float length = vertical ? b.height : b.width;
    const float level = std::clamp(rms_, 0.0f, 1.0f);
    const float fill = std::round(level * length);
    canvas.set_fill_color(fill_);
    const float cross = vertical ? b.width * std::clamp(bar_ratio_, 0.05f, 1.0f) : b.height;
    if (vertical)
        canvas.fill_rect((b.width - cross) * 0.5f, b.height - fill, cross, fill);
    else
        canvas.fill_rect(0, (b.height - cross) * 0.5f, fill, cross);
    canvas.set_stroke_color({1.0f, 1.0f, 1.0f, 1.0f});
    canvas.set_line_width(1.0f);
    const float peak_pos = std::round(std::clamp(peak_, 0.0f, 1.0f) * length);
    if (vertical)
        canvas.stroke_line(0, b.height - peak_pos, b.width, b.height - peak_pos);
    else
        canvas.stroke_line(peak_pos, 0, peak_pos, b.height);
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
    AudioFrameInfo next_info;
    if (!feed_->read_info(channel_, next_info)) {
        stale_ = true;
        return;
    }
    if (published == last_publish_) {
        ++quiet_ticks_;
        if (quiet_ticks_ >= 8)
            stale_ = true;
        return;
    }
    const auto count = feed_->read_vector(channel_, bins_);
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
