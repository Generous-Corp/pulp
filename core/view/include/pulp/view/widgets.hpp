#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <string>
#include <cmath>
#include <functional>
#include <array>

namespace pulp::view {

// ── Label ────────────────────────────────────────────────────────────────────
// Static or dynamic text display

class Label : public View {
public:
    Label() = default;
    explicit Label(std::string text) : text_(std::move(text)) {}

    void set_text(std::string text) { text_ = std::move(text); }
    const std::string& text() const { return text_; }

    void set_font_size(float size) { font_size_ = size; }
    float font_size() const { return font_size_; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::string text_;
    float font_size_ = 14.0f;
};

// ── Knob ─────────────────────────────────────────────────────────────────────
// Rotary control for audio parameters (gain, frequency, etc.)

class Knob : public View {
public:
    Knob() = default;

    void set_value(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    float value() const { return value_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    // Display format: called with normalized value to produce display text
    void set_format(std::function<std::string(float)> fmt) { format_ = std::move(fmt); }

    void paint(canvas::Canvas& canvas) override;

    // Arc range in radians (default: 270-degree sweep)
    static constexpr float start_angle = 2.356f;  // 135 degrees (bottom-left)
    static constexpr float end_angle = 7.069f;    // 405 degrees (bottom-right via top)

private:
    float value_ = 0.0f;
    std::string label_;
    std::function<std::string(float)> format_;
};

// ── Fader ────────────────────────────────────────────────────────────────────
// Linear slider for audio parameters

class Fader : public View {
public:
    enum class Orientation { vertical, horizontal };

    Fader() = default;

    void set_value(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    float value() const { return value_; }

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    void paint(canvas::Canvas& canvas) override;

private:
    float value_ = 0.0f;
    Orientation orientation_ = Orientation::vertical;
    std::string label_;
};

// ── Toggle ───────────────────────────────────────────────────────────────────
// Boolean on/off switch

class Toggle : public View {
public:
    Toggle() = default;

    void set_on(bool v) { on_ = v; }
    bool is_on() const { return on_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    void paint(canvas::Canvas& canvas) override;

private:
    bool on_ = false;
    std::string label_;
};

// ── Meter ────────────────────────────────────────────────────────────────────
// Audio level meter with peak hold

class Meter : public View {
public:
    enum class Orientation { vertical, horizontal };

    Meter() = default;

    // Set levels directly (normalized 0-1)
    void set_level(float rms, float peak);

    // Update from MeterBallistics (call once per frame with dt)
    void update(float raw_peak, float raw_rms, float dt);

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    // Ballistics accessors for testing
    float display_rms() const { return ballistics_.display_rms; }
    float display_peak() const { return ballistics_.display_peak; }
    float held_peak() const { return ballistics_.held_peak; }

    void paint(canvas::Canvas& canvas) override;

private:
    Orientation orientation_ = Orientation::vertical;
    MeterBallistics ballistics_;
    float current_rms_ = 0;
    float current_peak_ = 0;
};

// ── XYPad ────────────────────────────────────────────────────────────────────
// 2D parameter control surface (e.g., filter frequency × resonance)

class XYPad : public View {
public:
    XYPad() = default;

    void set_x(float v) { x_ = std::clamp(v, 0.0f, 1.0f); }
    void set_y(float v) { y_ = std::clamp(v, 0.0f, 1.0f); }
    float x_value() const { return x_; }
    float y_value() const { return y_; }

    void set_x_label(std::string l) { x_label_ = std::move(l); }
    void set_y_label(std::string l) { y_label_ = std::move(l); }

    void paint(canvas::Canvas& canvas) override;

private:
    float x_ = 0.5f, y_ = 0.5f;
    std::string x_label_, y_label_;
};

// ── WaveformView ─────────────────────────────────────────────────────────────
// Displays audio waveform data

class WaveformView : public View {
public:
    WaveformView() = default;

    // Set waveform data (normalized -1 to 1)
    void set_data(const float* samples, size_t count);
    void set_data(std::vector<float> samples);

    size_t sample_count() const { return samples_.size(); }

    void paint(canvas::Canvas& canvas) override;

private:
    std::vector<float> samples_;
};

// ── SpectrumView ─────────────────────────────────────────────────────────────
// Displays frequency spectrum (magnitude data from FFT)

class SpectrumView : public View {
public:
    enum class Style { bars, line, filled };

    SpectrumView() = default;

    // Set spectrum magnitudes (dB, typically -100 to 0)
    void set_spectrum(const float* magnitudes_db, size_t bin_count);
    void set_spectrum(std::vector<float> magnitudes_db);

    size_t bin_count() const { return bins_.size(); }

    void set_style(Style s) { style_ = s; }
    Style style() const { return style_; }

    // dB range for display
    void set_range(float min_db, float max_db) { min_db_ = min_db; max_db_ = max_db; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::vector<float> bins_;
    Style style_ = Style::filled;
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;
};

} // namespace pulp::view
