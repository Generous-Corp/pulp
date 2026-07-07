#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/theme_contrast.hpp>
#include <functional>
#include <vector>

namespace pulp::view {

// ── ColorPicker ─────────────────────────────────────────────────────────────
// HSL/HSB/hex color selection with swatches.

class ColorPicker : public View {
public:
    ColorPicker();

    /// Set/get the current color.
    void set_color(Color c);
    Color color() const { return color_; }

    /// Set/get as HSL.
    void set_hsl(HSL hsl);
    HSL hsl() const { return hsl_; }

    /// Set/get as hex string (e.g., "#ff6600").
    void set_hex(const std::string& hex);
    std::string hex() const;

    /// Callback when color changes.
    std::function<void(Color)> on_change;

    /// Preset swatches.
    void set_swatches(std::vector<Color> swatches);
    const std::vector<Color>& swatches() const { return swatches_; }

    /// Display mode.
    enum class Mode { hsl, hsb, hex_only };
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    /// Show/hide alpha slider.
    void set_show_alpha(bool show) { show_alpha_ = show; }
    bool show_alpha() const { return show_alpha_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool wants_mouse_input() const override { return true; }

    float intrinsic_height() const override { return 260.0f; }

private:
    Color color_{};
    HSL hsl_{};
    Mode mode_ = Mode::hsl;
    bool show_alpha_ = false;
    std::vector<Color> swatches_;

    enum class DragTarget { none, saturation_lightness, hue, alpha, swatch };
    DragTarget drag_target_ = DragTarget::none;

    Rect sl_area() const;      // Saturation/Lightness square
    Rect hue_bar() const;      // Hue slider bar
    Rect alpha_bar() const;    // Alpha slider bar
    Rect swatch_area() const;  // Swatch grid area

    void update_from_hsl();
    void update_from_color();
};

} // namespace pulp::view
