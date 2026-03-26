#pragma once

/// @file design_export.hpp
/// Export design tokens to multiple formats: JSON, CSS variables, C++ headers,
/// shader uniforms, OKLCH CSS.

#include <pulp/view/theme.hpp>
#include <string>

namespace pulp::view {

/// Export a Theme's design tokens to various output formats.
///
/// Design tokens are the structured data behind a plugin's visual identity:
/// colors, dimensions, typography, widget geometry, and rendering styles.
///
/// @code
/// Theme theme = load_theme("dark.json");
/// auto json = DesignExport::to_json(theme);
/// auto css = DesignExport::to_css(theme);
/// auto cpp = DesignExport::to_cpp_header(theme, "my_theme");
/// @endcode
class DesignExport {
public:
    /// Export to JSON (round-trippable with Theme::from_json).
    static std::string to_json(const Theme& theme);

    /// Export to CSS custom properties.
    /// @code
    /// :root {
    ///   --pulp-background: #1a1a2e;
    ///   --pulp-accent: #e94560;
    ///   --pulp-knob-size: 60px;
    /// }
    /// @endcode
    static std::string to_css(const Theme& theme, const std::string& prefix = "pulp");

    /// Export to a C++ header with constexpr color/dimension values.
    /// @code
    /// namespace my_theme {
    ///   constexpr uint32_t kBackground = 0xFF1a1a2e;
    ///   constexpr float kKnobSize = 60.0f;
    /// }
    /// @endcode
    static std::string to_cpp_header(const Theme& theme, const std::string& namespace_name);

    /// Export to OKLCH CSS color format.
    /// @code
    /// --pulp-accent: oklch(0.628 0.258 12.5);
    /// @endcode
    static std::string to_oklch_css(const Theme& theme, const std::string& prefix = "pulp");

    /// Export to GPU shader uniform declarations (WGSL).
    /// @code
    /// struct ThemeUniforms {
    ///   background: vec4<f32>,
    ///   accent: vec4<f32>,
    ///   knob_size: f32,
    /// };
    /// @endcode
    static std::string to_wgsl_uniforms(const Theme& theme, const std::string& struct_name = "ThemeUniforms");
};

} // namespace pulp::view
