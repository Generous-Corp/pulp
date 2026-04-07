#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/color_picker.hpp>
#include <functional>

namespace pulp::view {

/// Interactive theme editor widget — displays all color tokens as editable
/// swatches. Click a swatch to open a ColorPicker. Changes update the
/// active Theme in real-time.
///
/// Designed for the style designer workflow and developer theme iteration.
class ThemeEditor : public View {
public:
    ThemeEditor();

    /// Set the theme to edit. Changes are applied in real-time.
    void set_theme(Theme theme) {
        editing_theme_ = std::move(theme);
        build_swatches();
    }

    /// Get the current edited theme.
    const Theme& editing_theme() const { return editing_theme_; }

    /// Callback when any color token is changed.
    std::function<void(const std::string& token, Color new_color)> on_color_changed;

    /// Callback when the entire theme is exported (e.g., save button).
    std::function<void(const Theme& theme)> on_export;

    /// Export the current theme as JSON.
    std::string export_json() const { return editing_theme_.to_json(); }

    /// Get the list of color token names in the theme.
    std::vector<std::string> token_names() const {
        std::vector<std::string> names;
        names.reserve(editing_theme_.colors.size());
        for (auto& [k, _] : editing_theme_.colors)
            names.push_back(k);
        return names;
    }

    /// Set the currently selected token (opens ColorPicker for it).
    void select_token(const std::string& name) {
        selected_token_ = name;
        auto it = editing_theme_.colors.find(name);
        if (it != editing_theme_.colors.end()) {
            picker_color_ = it->second;
        }
    }

    const std::string& selected_token() const { return selected_token_; }

protected:
    void paint(canvas::Canvas& canvas) override;

private:
    void build_swatches();

    Theme editing_theme_;
    std::string selected_token_;
    Color picker_color_;
    float swatch_size_ = 24.0f;
    float swatch_gap_ = 4.0f;
};

} // namespace pulp::view
