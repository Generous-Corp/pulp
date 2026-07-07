#pragma once

/// @file waveform_editor.hpp
/// Interactive waveform editor with selection, zoom, scroll, and region marking.

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/waveform_editor_primitives.hpp>
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
/// Provides:
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
        if (!data || num_samples <= 0) {
            audio_data_.clear();
            sample_rate_ = sample_rate;
            total_samples_ = 0;
            viewport_.set_total_samples(0);
            clamp_editor_state_to_audio_bounds();
            return;
        }

        audio_data_.assign(data, data + num_samples);
        sample_rate_ = sample_rate;
        total_samples_ = num_samples;
        viewport_.set_total_samples(num_samples);
        viewport_.set_visible_range(0, num_samples);
        clamp_editor_state_to_audio_bounds();
    }

    int total_samples() const { return total_samples_; }
    float sample_rate() const { return sample_rate_; }

    // ── Selection ────────────────────────────────────────────────────────

    void set_selection(int start, int end) {
        set_selection_impl(start, end, true);
        dragging_ = false;
    }

    void clear_selection() { set_selection(0, 0); }
    bool has_selection() const { return selection_start_ != selection_end_; }
    int selection_start() const { return selection_start_; }
    int selection_end() const { return selection_end_; }
    int selection_length() const { return selection_end() - selection_start(); }

    std::function<void(int start, int end)> on_selection_changed;

    // ── Zoom and scroll ──────────────────────────────────────────────────

    /// Set the visible range (in samples).
    void set_visible_range(int start, int length) {
        viewport_.set_total_samples(total_samples_);
        viewport_.set_visible_range(start, length);
    }

    int visible_start() const { return static_cast<int>(viewport_.visible_start); }
    int visible_length() const { return static_cast<int>(viewport_.visible_length); }

    /// Zoom in by a factor (centered on the view midpoint).
    void zoom_in(float factor = 2.0f) { viewport_.zoom_in(factor); }

    /// Zoom out by a factor.
    void zoom_out(float factor = 2.0f) { viewport_.zoom_out(factor); }

    /// Zoom to fit all audio data.
    void zoom_to_fit() { viewport_.zoom_to_fit(); }

    /// Zoom to selection.
    void zoom_to_selection() {
        if (has_selection()) set_visible_range(selection_start(), selection_length());
    }

    /// Scroll by a number of samples.
    void scroll(int delta_samples) { viewport_.scroll(delta_samples); }

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
        if (audio_data_.empty() || b.is_empty()) return;
        const auto viewport = viewport_for_bounds(b);

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

        float samples_per_pixel = static_cast<float>(viewport.samples_per_pixel());
        for (int px = 0; px < static_cast<int>(b.width); ++px) {
            int s0 = static_cast<int>(viewport.visible_start + static_cast<int64_t>(static_cast<float>(px) * samples_per_pixel));
            int s1 = static_cast<int>(viewport.visible_start + static_cast<int64_t>(static_cast<float>(px + 1) * samples_per_pixel));
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
        const auto playhead = build_waveform_playhead_overlay(viewport, playhead_);
        if (playhead.visible) {
            canvas.set_stroke_color(resolve_color("playhead",
                canvas::Color::hex(0xff4444)));
            canvas.set_line_width(1.5f);
            canvas.stroke_line(playhead.x, b.y, playhead.x, b.y + b.height);
        }
    }

    // ── Events ───────────────────────────────────────────────────────────

    void on_mouse_event(const MouseEvent& event) override {
        if (event.is_cancelled) {
            dragging_ = false;
            return;
        }

        const bool explicit_phase = event.hasExplicitPhase();
        const bool is_press = explicit_phase ? event.isPress() : (!dragging_ && event.is_down);
        const bool is_drag = explicit_phase ? event.isDrag() : (dragging_ && !event.is_down);
        const bool is_release = explicit_phase ? event.isRelease() : (dragging_ && event.is_down);

        if (!is_press && !is_drag && !is_release) return;

        auto b = local_bounds();
        int sample = x_to_sample(event.position.x, b);

        if (is_press) {
            drag_anchor_ = (event.isShiftDown() && has_selection()) ? selection_start() : sample;
            set_selection_impl(drag_anchor_, sample, true);
            dragging_ = true;
        } else if (is_drag && dragging_) {
            set_selection_impl(drag_anchor_, sample, true);
        } else if (is_release && dragging_) {
            set_selection_impl(drag_anchor_, sample, true);
            dragging_ = false;
        }
    }

    bool on_key_event(const KeyEvent& event) override {
        if (!event.is_down) return false;

        if (event.isMainModifier()) {
            if (event.key == KeyCode::a) { set_selection(0, total_samples_); return true; }
        }

        if (event.key == KeyCode::left) { scroll(-visible_length() / 10); return true; }
        if (event.key == KeyCode::right) { scroll(visible_length() / 10); return true; }

        // Main-modifier + 0 zooms to fit.
        if (event.key == KeyCode::num0 && event.isMainModifier()) { zoom_to_fit(); return true; }

        return false;
    }

    bool wants_mouse_input() const override { return true; }

private:
    std::vector<float> audio_data_;
    float sample_rate_ = 44100.0f;
    int total_samples_ = 0;
    WaveformViewport viewport_;
    int selection_start_ = 0;
    int selection_end_ = 0;
    int playhead_ = 0;
    int drag_anchor_ = 0;
    bool dragging_ = false;
    std::vector<WaveformRegion> regions_;

    void set_selection_impl(int start, int end, bool notify) {
        start = std::clamp(start, 0, total_samples_);
        end = std::clamp(end, 0, total_samples_);
        if (end < start) std::swap(start, end);

        const bool changed = start != selection_start_ || end != selection_end_;
        selection_start_ = start;
        selection_end_ = end;
        if (notify && changed && on_selection_changed) {
            on_selection_changed(selection_start_, selection_end_);
        }
    }

    void clamp_editor_state_to_audio_bounds() {
        set_selection_impl(selection_start_, selection_end_, false);
        playhead_ = std::clamp(playhead_, 0, total_samples_);
        drag_anchor_ = std::clamp(drag_anchor_, 0, total_samples_);
        dragging_ = false;
        for (auto& region : regions_) {
            region.start_sample = std::clamp(region.start_sample, 0, total_samples_);
            region.end_sample = std::clamp(region.end_sample, 0, total_samples_);
        }
    }

protected:
    // Viewport-aware sample<->pixel mapping, shared with subclasses so custom
    // overlays (playheads, hit-testing, drag targets) stay aligned with the
    // rendered waveform under zoom/scroll.
    WaveformViewport viewport_for_bounds(const Rect& b) const {
        auto viewport = viewport_;
        viewport.set_bounds(b);
        return viewport;
    }

    float sample_to_x(int sample, const Rect& b) const {
        return viewport_for_bounds(b).sample_to_x(sample);
    }

    int x_to_sample(float x, const Rect& b) const {
        return static_cast<int>(viewport_for_bounds(b).x_to_sample(x));
    }
};

} // namespace pulp::view
