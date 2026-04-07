#pragma once

/// @file waveform_editor.hpp
/// Interactive waveform editor with selection, zoom, scroll, and region marking.

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>
#include <vector>
#include <functional>
#include <algorithm>
#include <cmath>

namespace pulp::view {

/// A region (selection or marker) within the waveform.
struct WaveformRegion {
    int start_sample = 0;    ///< Start sample index
    int end_sample = 0;      ///< End sample index (exclusive)
    std::string label;
    canvas::Color color = canvas::Color::rgba8(65, 105, 225, 80);

    int length() const { return end_sample - start_sample; }
};

/// Interactive waveform editor for audio file editing and sample selection.
///
/// Extends WaveformView with:
/// - Click-drag selection
/// - Zoom in/out (horizontal)
/// - Scroll (horizontal)
/// - Named regions/markers
/// - Playhead position tracking
///
/// @code
/// WaveformEditor editor;
/// editor.set_audio_data(samples.data(), samples.size(), sample_rate);
/// editor.on_selection_changed = [&](int start, int end) {
///     set_loop_region(start, end);
/// };
/// @endcode
class WaveformEditor : public View {
public:
    WaveformEditor() { set_focusable(true); }

    /// Set mono audio data to display.
    void set_audio_data(const float* data, int num_samples, float sample_rate) {
        audio_data_.assign(data, data + num_samples);
        sample_rate_ = sample_rate;
        total_samples_ = num_samples;
        visible_start_ = 0;
        visible_length_ = num_samples;
    }

    int total_samples() const { return total_samples_; }
    float sample_rate() const { return sample_rate_; }

    // ── Selection ────────────────────────────────────────────────────────

    void set_selection(int start, int end) {
        selection_start_ = std::clamp(start, 0, total_samples_);
        selection_end_ = std::clamp(end, 0, total_samples_);
        if (on_selection_changed) on_selection_changed(selection_start_, selection_end_);
    }

    void clear_selection() { set_selection(0, 0); }
    bool has_selection() const { return selection_start_ != selection_end_; }
    int selection_start() const { return std::min(selection_start_, selection_end_); }
    int selection_end() const { return std::max(selection_start_, selection_end_); }
    int selection_length() const { return selection_end() - selection_start(); }

    std::function<void(int start, int end)> on_selection_changed;

    // ── Zoom and scroll ──────────────────────────────────────────────────

    /// Set the visible range (in samples).
    void set_visible_range(int start, int length) {
        visible_start_ = std::max(0, start);
        visible_length_ = std::clamp(length, 16, total_samples_);
        if (visible_start_ + visible_length_ > total_samples_)
            visible_start_ = total_samples_ - visible_length_;
    }

    int visible_start() const { return visible_start_; }
    int visible_length() const { return visible_length_; }

    /// Zoom in by a factor (centered on the view midpoint).
    void zoom_in(float factor = 2.0f) {
        int new_len = std::max(16, static_cast<int>(static_cast<float>(visible_length_) / factor));
        int center = visible_start_ + visible_length_ / 2;
        set_visible_range(center - new_len / 2, new_len);
    }

    /// Zoom out by a factor.
    void zoom_out(float factor = 2.0f) {
        int new_len = std::min(total_samples_,
            static_cast<int>(static_cast<float>(visible_length_) * factor));
        int center = visible_start_ + visible_length_ / 2;
        set_visible_range(center - new_len / 2, new_len);
    }

    /// Zoom to fit all audio data.
    void zoom_to_fit() { set_visible_range(0, total_samples_); }

    /// Zoom to selection.
    void zoom_to_selection() {
        if (has_selection()) set_visible_range(selection_start(), selection_length());
    }

    /// Scroll by a number of samples.
    void scroll(int delta_samples) {
        set_visible_range(visible_start_ + delta_samples, visible_length_);
    }

    // ── Playhead ─────────────────────────────────────────────────────────

    void set_playhead(int sample) { playhead_ = std::clamp(sample, 0, total_samples_); }
    int playhead() const { return playhead_; }

    // ── Regions ──────────────────────────────────────────────────────────

    void add_region(WaveformRegion region) { regions_.push_back(std::move(region)); }
    void clear_regions() { regions_.clear(); }
    const std::vector<WaveformRegion>& regions() const { return regions_; }

    // ── Painting ─────────────────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override {
        auto b = local_bounds();
        if (audio_data_.empty()) return;

        // Background
        canvas.set_fill_color(resolve_color("waveform_bg",
            canvas::Color::hex(0x0a0a1a)));
        canvas.fill_rect(b.x, b.y, b.width, b.height);

        float center_y = b.y + b.height / 2;
        float half_h = b.height / 2 - 2;

        // Draw regions
        for (const auto& region : regions_) {
            float rx = sample_to_x(region.start_sample, b);
            float rw = sample_to_x(region.end_sample, b) - rx;
            canvas.set_fill_color(region.color);
            canvas.fill_rect(rx, b.y, rw, b.height);
        }

        // Draw selection
        if (has_selection()) {
            float sx = sample_to_x(selection_start(), b);
            float sw = sample_to_x(selection_end(), b) - sx;
            canvas.set_fill_color(resolve_color("selection",
                canvas::Color::rgba8(65, 105, 225, 60)));
            canvas.fill_rect(sx, b.y, sw, b.height);
        }

        // Draw waveform (min/max per pixel)
        canvas.set_stroke_color(resolve_color("waveform",
            canvas::Color::hex(0x4a9eff)));
        canvas.set_line_width(1);

        float samples_per_pixel = static_cast<float>(visible_length_) / b.width;
        for (int px = 0; px < static_cast<int>(b.width); ++px) {
            int s0 = visible_start_ + static_cast<int>(static_cast<float>(px) * samples_per_pixel);
            int s1 = visible_start_ + static_cast<int>(static_cast<float>(px + 1) * samples_per_pixel);
            s0 = std::clamp(s0, 0, total_samples_ - 1);
            s1 = std::clamp(s1, s0 + 1, total_samples_);

            float min_val = 0, max_val = 0;
            for (int s = s0; s < s1; ++s) {
                float v = audio_data_[static_cast<size_t>(s)];
                min_val = std::min(min_val, v);
                max_val = std::max(max_val, v);
            }

            float x = b.x + static_cast<float>(px);
            float y_top = center_y - max_val * half_h;
            float y_bot = center_y - min_val * half_h;
            canvas.stroke_line(x, y_top, x, y_bot);
        }

        // Center line
        canvas.set_stroke_color(resolve_color("waveform_center",
            canvas::Color::rgba8(255, 255, 255, 30)));
        canvas.stroke_line(b.x, center_y, b.x + b.width, center_y);

        // Playhead
        if (playhead_ >= visible_start_ && playhead_ < visible_start_ + visible_length_) {
            float px = sample_to_x(playhead_, b);
            canvas.set_stroke_color(resolve_color("playhead",
                canvas::Color::hex(0xff4444)));
            canvas.set_line_width(1.5f);
            canvas.stroke_line(px, b.y, px, b.y + b.height);
        }
    }

    // ── Events ───────────────────────────────────────────────────────────

    void on_mouse_event(const MouseEvent& event) override {
        if (!event.is_down && !dragging_) return;
        auto b = local_bounds();

        int sample = x_to_sample(event.position.x, b);

        if (event.is_down) {
            if (event.isShiftDown() && has_selection()) {
                // Extend selection
                selection_end_ = sample;
            } else {
                selection_start_ = sample;
                selection_end_ = sample;
                dragging_ = true;
            }
        } else if (dragging_) {
            // Mouse up — finalize selection
            dragging_ = false;
        }

        // Drag — extend selection
        if (dragging_ && !event.is_down) {
            selection_end_ = sample;
        }

        if (on_selection_changed && has_selection())
            on_selection_changed(selection_start(), selection_end());
    }

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down) return false;

        if (event.isMainModifier()) {
            if (event.key == KeyCode::a) { set_selection(0, total_samples_); return true; }
        }

        if (event.key == KeyCode::left) { scroll(-visible_length_ / 10); return true; }
        if (event.key == KeyCode::right) { scroll(visible_length_ / 10); return true; }

        // +/- for zoom
        if (event.key == KeyCode::num0 && event.isMainModifier()) { zoom_to_fit(); return true; }

        return false;
    }

private:
    std::vector<float> audio_data_;
    float sample_rate_ = 44100.0f;
    int total_samples_ = 0;
    int visible_start_ = 0;
    int visible_length_ = 0;
    int selection_start_ = 0;
    int selection_end_ = 0;
    int playhead_ = 0;
    bool dragging_ = false;
    std::vector<WaveformRegion> regions_;

    float sample_to_x(int sample, const Rect& b) const {
        float frac = static_cast<float>(sample - visible_start_) / static_cast<float>(visible_length_);
        return b.x + frac * b.width;
    }

    int x_to_sample(float x, const Rect& b) const {
        float frac = (x - b.x) / b.width;
        return visible_start_ + static_cast<int>(frac * static_cast<float>(visible_length_));
    }
};

} // namespace pulp::view
