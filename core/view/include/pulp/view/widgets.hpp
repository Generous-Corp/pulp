#pragma once

#include <algorithm>
#include <pulp/view/view.hpp>
#include <pulp/view/audio_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/sprite_strip.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/multi_channel_meter.hpp>
#include <string>
#include <cmath>
#include <functional>
#include <array>
#include <vector>

namespace pulp::view {

// ── Label ────────────────────────────────────────────────────────────────────
// Static or dynamic text display

/// Text alignment for Label
enum class LabelAlign { left, center, right };

class Label : public View {
public:
    Label() { set_access_role(AccessRole::label); }
    explicit Label(std::string text) : text_(std::move(text)) {
        set_access_role(AccessRole::label);
        set_access_label(text_);
    }

    void set_text(std::string text) { text_ = std::move(text); }
    const std::string& text() const { return text_; }

    void set_font_size(float size) { font_size_ = size; }
    float font_size() const { return font_size_; }

    void set_font_weight(int weight) { font_weight_ = weight; }  // 100-900, 400=normal, 700=bold
    int font_weight() const { return font_weight_; }

    void set_font_style(int style) { font_style_ = style; }  // 0=normal, 1=italic
    int font_style() const { return font_style_; }

    void set_letter_spacing(float sp) { letter_spacing_ = sp; }
    float letter_spacing() const { return letter_spacing_; }

    void set_line_height(float lh) { line_height_ = lh; }
    float line_height() const { return line_height_; }

    void set_text_align(LabelAlign align) { text_align_ = align; }
    LabelAlign text_align() const { return text_align_; }

    void set_multi_line(bool ml) { multi_line_ = ml; }
    bool multi_line() const { return multi_line_; }

    /// CSS text-transform: uppercase, lowercase, capitalize, none
    enum class TextTransform { none, uppercase, lowercase, capitalize };
    void set_text_transform(TextTransform t) { text_transform_ = t; }
    TextTransform text_transform() const { return text_transform_; }

    /// CSS text-decoration: none, underline, line-through, overline
    enum class TextDecoration { none, underline, line_through, overline };
    void set_text_decoration(TextDecoration d) { text_decoration_ = d; }
    void set_text_decoration_color(canvas::Color c) { decoration_color_ = c; has_decoration_color_ = true; }

    void paint(canvas::Canvas& canvas) override;

    /// Intrinsic height based on font size and line height.
    float intrinsic_height() const override {
        return line_height_ > 0 ? line_height_ : font_size_ * 1.4f;
    }

private:
    std::string text_;
    float font_size_ = 14.0f;
    int font_weight_ = 400;       ///< 400=normal, 700=bold
    int font_style_ = 0;          ///< 0=normal, 1=italic
    float letter_spacing_ = 0;    ///< Extra spacing between characters (px)
    float line_height_ = 0;       ///< 0=auto (font_size * 1.4)
    LabelAlign text_align_ = LabelAlign::left;
    bool multi_line_ = false;
    TextTransform text_transform_ = TextTransform::none;
    TextDecoration text_decoration_ = TextDecoration::none;
    canvas::Color decoration_color_{};
    bool has_decoration_color_ = false;
    canvas::TextDirection text_direction_ = canvas::TextDirection::left_to_right;
    canvas::TextVerticalAlign vertical_align_ = canvas::TextVerticalAlign::top;

public:
    /// Set text direction (LTR, RTL, vertical top-to-bottom, vertical bottom-to-top).
    void set_text_direction(canvas::TextDirection d) { text_direction_ = d; }
    canvas::TextDirection text_direction() const { return text_direction_; }

    /// Set vertical text alignment within the label's bounds.
    void set_vertical_align(canvas::TextVerticalAlign a) { vertical_align_ = a; }
    canvas::TextVerticalAlign vertical_align() const { return vertical_align_; }
};

// ── Knob ─────────────────────────────────────────────────────────────────────
// Rotary control for audio parameters (gain, frequency, etc.)

/// Rendering style for audio widgets.
/// `standard` = interactive widget (arcs, thumbs, fills).
/// `minimal` = design-preview mode (simple shapes matching design tools).
enum class WidgetRenderStyle { standard, minimal };

class Knob : public View {
public:
    Knob() { set_access_role(AccessRole::slider); set_focusable(true); }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    void set_value(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    float value() const { return value_; }

    void set_default_value(float v) { default_value_ = std::clamp(v, 0.0f, 1.0f); }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    // Display format: called with normalized value to produce display text
    void set_format(std::function<std::string(float)> fmt) { format_ = std::move(fmt); }

    /// Called when the value changes (from user interaction or programmatic).
    std::function<void(float)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;

    // Animation accessors for testing
    float hover_glow() const { return hover_glow_.value(); }
    void advance_animations(float dt);

    // Arc range in radians (default: 270-degree sweep)
    static constexpr float start_angle = 2.356f;  // 135 degrees (bottom-left)
    static constexpr float end_angle = 7.069f;    // 405 degrees (bottom-right via top)

    // Custom shader: if set, replaces the body/track/fill arc drawing with SkSL GPU shader.
    // Labels, value text, and hover glow still draw in C++.
    void set_custom_shader(std::string sksl) { custom_sksl_ = std::move(sksl); }
    void clear_custom_shader() { custom_sksl_.clear(); }
    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }
    bool shader_uses_time() const { return custom_sksl_.find("time") != std::string::npos; }

    // Declarative widget schema: JSON defining appearance as data (Rive-inspired)
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }

private:
    float value_ = 0.0f;
    float default_value_ = 0.5f;
    std::string label_;
    std::function<std::string(float)> format_;
    ValueAnimation hover_glow_{0.0f};
    float drag_start_y_ = 0;
    float drag_start_value_ = 0;
    std::string custom_sksl_;     // SkSL source for GPU shader body
    std::string widget_schema_;   // JSON declarative schema
    std::string lottie_json_;     // Lottie animation JSON
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;
    float lottie_time_ = 0;       // Current playback position (0-1)

public:
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

    /// Sprite strip: designer-created filmstrip for knob appearance.
    /// The current value selects which frame to display.
    void set_sprite_strip(std::shared_ptr<SpriteStrip> strip) { sprite_strip_ = std::move(strip); }
    const std::shared_ptr<SpriteStrip>& sprite_strip() const { return sprite_strip_; }

private:
    std::shared_ptr<SpriteStrip> sprite_strip_;
};

// ── Fader ────────────────────────────────────────────────────────────────────
// Linear slider for audio parameters

class Fader : public View {
public:
    enum class Orientation { vertical, horizontal };

    Fader() { set_access_role(AccessRole::slider); set_focusable(true); }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

    void set_value(float v) { value_ = std::clamp(v, 0.0f, 1.0f); }
    float value() const { return value_; }

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    /// Called when the value changes.
    std::function<void(float)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;

    // Animation accessors for testing
    float hover_scale() const { return hover_thumb_scale_.value(); }
    void advance_animations(float dt);

    void set_custom_shader(std::string sksl) { custom_sksl_ = std::move(sksl); }
    void clear_custom_shader() { custom_sksl_.clear(); }
    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }
    bool shader_uses_time() const { return custom_sksl_.find("time") != std::string::npos; }
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

private:
    float value_ = 0.0f;
    Orientation orientation_ = Orientation::vertical;
    std::string label_;
    ValueAnimation hover_thumb_scale_{1.0f};
    bool dragging_ = false;
    std::string custom_sksl_;
    std::string widget_schema_;
    std::string lottie_json_;
    float lottie_time_ = 0.0f;
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;

public:
    /// Sprite strip: designer-created filmstrip for fader appearance.
    void set_sprite_strip(std::shared_ptr<SpriteStrip> strip) { sprite_strip_ = std::move(strip); }
    const std::shared_ptr<SpriteStrip>& sprite_strip() const { return sprite_strip_; }

private:
    std::shared_ptr<SpriteStrip> sprite_strip_;
};

// ── Toggle ───────────────────────────────────────────────────────────────────
// Boolean on/off switch

class Toggle : public View {
public:
    Toggle() : thumb_position_(0.0f), hover_opacity_(0.0f) {
        set_access_role(AccessRole::toggle); set_focusable(true);
    }

    void set_on(bool v);
    bool is_on() const { return on_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    /// Called when the toggle state changes.
    std::function<void(bool)> on_toggle;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

    // Animation accessors for testing
    float thumb_position() const { return thumb_position_.value(); }
    float hover_opacity() const { return hover_opacity_.value(); }
    void advance_animations(float dt);

    void set_custom_shader(std::string sksl) { custom_sksl_ = std::move(sksl); }
    void clear_custom_shader() { custom_sksl_.clear(); }
    bool has_custom_shader() const { return !custom_sksl_.empty(); }
    const std::string& custom_shader() const { return custom_sksl_; }
    bool shader_uses_time() const { return custom_sksl_.find("time") != std::string::npos; }
    void set_widget_schema(std::string json) { widget_schema_ = std::move(json); }
    const std::string& widget_schema() const { return widget_schema_; }
    void set_lottie_json(std::string json) { lottie_json_ = std::move(json); }
    const std::string& lottie_json() const { return lottie_json_; }
    void set_lottie_time(float t) { lottie_time_ = std::clamp(t, 0.0f, 1.0f); }
    float lottie_time() const { return lottie_time_; }

private:
    bool on_ = false;
    std::string label_;
    ValueAnimation thumb_position_;
    ValueAnimation hover_opacity_;
    std::string custom_sksl_;
    std::string widget_schema_;
    std::string lottie_json_;
    float lottie_time_ = 0.0f;
};

// ── Checkbox ────────────────────────────────────────────────────────────────
// Circular checkbox with check glyph

class Checkbox : public View {
public:
    Checkbox() { set_access_role(AccessRole::toggle); set_focusable(true); }

    void set_checked(bool v) { checked_ = v; }
    bool is_checked() const { return checked_; }

    std::function<void(bool)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    bool checked_ = false;
};

// ── ToggleButton ────────────────────────────────────────────────────────────
// Full-width rounded button that toggles on/off

class ToggleButton : public View {
public:
    ToggleButton() { set_access_role(AccessRole::toggle); set_focusable(true); }

    void set_on(bool v) { on_ = v; }
    bool is_on() const { return on_; }

    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    std::function<void(bool)> on_toggle;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

    float intrinsic_height() const override { return 36.0f; }

private:
    bool on_ = false;
    std::string label_;
};

// ── Icon ────────────────────────────────────────────────────────────────────
// Simple vector icons drawn with canvas strokes

class Icon : public View {
public:
    enum class Type { image_upload, send, search, close };

    Icon() {}
    explicit Icon(Type t) : type_(t) {}

    void set_type(Type t) { type_ = t; }
    Type type() const { return type_; }

    void paint(canvas::Canvas& canvas) override;

private:
    Type type_ = Type::image_upload;
};

// ── ImageView ───────────────────────────────────────────────────────────────
// Displays an image loaded from a file path

class ImageView : public View {
public:
    ImageView() {}

    void set_image_path(const std::string& path) {
        if (path != path_) {
            path_ = path;
            loaded_ = false;
            cached_data_.clear();
        }
    }
    const std::string& image_path() const { return path_; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::string path_;
    bool loaded_ = false;
    std::vector<uint8_t> cached_data_;  // File bytes cached after first successful load
};

// ── Meter ────────────────────────────────────────────────────────────────────
// Audio level meter with peak hold

class Meter : public View {
public:
    enum class Orientation { vertical, horizontal };

    Meter() { set_access_role(AccessRole::meter); }

    void set_render_style(WidgetRenderStyle s) { render_style_ = s; }
    WidgetRenderStyle render_style() const { return render_style_; }

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
    WidgetRenderStyle render_style_ = WidgetRenderStyle::standard;
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

    std::function<void(float, float)> on_change;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;

private:
    void update_from_pos(Point pos);
    float x_ = 0.5f, y_ = 0.5f;
    std::string x_label_, y_label_;
};

// ── WaveformView ─────────────────────────────────────────────────────────────
// Displays audio waveform data

class WaveformView : public View {
public:
    // Triggering mode for periodic signal display.
    // free_run:     no triggering — caller is responsible for continuity
    // rising_zero:  align to the first rising-edge zero crossing
    // falling_zero: align to the first falling-edge zero crossing
    enum class TriggerMode { free_run, rising_zero, falling_zero };

    WaveformView() = default;

    // Set waveform data (normalized -1 to 1). If a non-free_run trigger
    // mode is active, the buffer is rotated so the first matching
    // crossing becomes index 0, producing a stable display for periodic
    // signals. If no crossing is found, the buffer is stored as-is.
    void set_data(const float* samples, size_t count);
    void set_data(std::vector<float> samples);

    size_t sample_count() const { return samples_.size(); }

    void set_trigger_mode(TriggerMode mode) { trigger_mode_ = mode; }
    TriggerMode trigger_mode() const { return trigger_mode_; }

    // Locate the first crossing of `mode` in the buffer without mutating
    // it. Returns the crossing index, or 0 if none is found or
    // mode is free_run.
    static size_t find_trigger_index(const float* samples, size_t count,
                                     TriggerMode mode);

    void paint(canvas::Canvas& canvas) override;

private:
    void apply_trigger();

    std::vector<float> samples_;
    TriggerMode trigger_mode_ = TriggerMode::free_run;
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

// ── Panel ────────────────────────────────────────────────────────────────────
// Styled container with background, border, and rounding. For visual chrome.

class Panel : public View {
public:
    Panel() = default;

    void set_background_token(std::string token) { bg_token_ = std::move(token); }
    void set_border_token(std::string token) { border_token_ = std::move(token); }
    void set_corner_radius(float r) { corner_radius_ = r; }
    void set_border_width(float w) { border_width_ = w; }

    const std::string& background_token() const { return bg_token_; }
    const std::string& border_token() const { return border_token_; }
    float corner_radius() const { return corner_radius_; }
    float border_width() const { return border_width_; }

    void paint(canvas::Canvas& canvas) override;

private:
    std::string bg_token_ = "bg.surface";
    std::string border_token_ = "control.border";
    float corner_radius_ = 8.0f;
    float border_width_ = 1.0f;
};

// ── SpectrogramView ──────────────────────────────────────────────────────────
// Scrolling time-frequency display. Each STFT frame becomes a column of
// colored pixels, scrolling left as new frames arrive.

class SpectrogramView : public View {
public:
    SpectrogramView() = default;

    /// Push a new STFT frame (dB magnitudes) for display.
    void push_spectrum(const float* magnitudes_db, int num_bins);

    /// Configure display dimensions and color mapping.
    void configure(int history_columns, int freq_rows,
                   signal::ColorRamp ramp = signal::ColorRamp::inferno,
                   float min_db = -80.0f, float max_db = 0.0f);

    void set_color_ramp(signal::ColorRamp ramp) { mapper_.set_ramp(ramp); }
    void set_range(float min_db, float max_db) { min_db_ = min_db; max_db_ = max_db; }

    int history_columns() const { return buffer_.width(); }
    int freq_rows() const { return buffer_.height(); }

    void paint(canvas::Canvas& canvas) override;

private:
    signal::SpectrogramBuffer buffer_;
    signal::ColorMapper mapper_{signal::ColorRamp::inferno};
    float min_db_ = -80.0f;
    float max_db_ = 0.0f;
    bool configured_ = false;
};

// ── MultiMeter ──────────────────────────────────────────────────────────────
// Multi-channel level meter with configurable layout. Supports arbitrary
// channel counts (mono through ambisonic). Uses MultiChannelBallistics
// for smooth display.

class MultiMeter : public View {
public:
    enum class Layout { vertical, horizontal };

    MultiMeter() { set_access_role(AccessRole::meter); }

    /// Update from multi-channel meter data. Call once per UI frame.
    void update(const signal::MultiChannelMeterData& data, float dt);

    void set_layout(Layout l) { layout_ = l; }
    Layout layout() const { return layout_; }

    void set_channel_count(int count);
    int channel_count() const { return ballistics_.num_channels; }

    /// Access ballistics for testing.
    const signal::MultiChannelBallistics& ballistics() const { return ballistics_; }

    void paint(canvas::Canvas& canvas) override;

private:
    Layout layout_ = Layout::vertical;
    signal::MultiChannelBallistics ballistics_;
};

// ── CorrelationMeter ────────────────────────────────────────────────────────
// Stereo correlation display (-1 to +1). Shows phase relationship between
// left and right channels.

class CorrelationMeter : public View {
public:
    CorrelationMeter() { set_access_role(AccessRole::meter); }

    /// Update with new correlation value (-1 to +1). Call once per UI frame.
    void update(float correlation, float dt);

    float display_correlation() const { return display_correlation_; }

    void paint(canvas::Canvas& canvas) override;

private:
    float display_correlation_ = 0.0f;
    float smoothing_coeff_ = 0.1f; // Exponential smoothing
};

} // namespace pulp::view
