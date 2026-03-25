#pragma once

#include <pulp/view/view.hpp>
#include <string>
#include <cmath>
#include <functional>

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

} // namespace pulp::view
