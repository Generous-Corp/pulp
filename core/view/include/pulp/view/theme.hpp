#pragma once

#include <pulp/canvas/canvas.hpp>
#include <string>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace pulp::view {

// Use the canvas Color type throughout the view system
using Color = canvas::Color;

// Convenience helpers for hex color creation
inline Color color_from_hex(uint32_t hex) {
    return Color::rgba8(
        static_cast<uint8_t>((hex >> 16) & 0xFF),
        static_cast<uint8_t>((hex >> 8) & 0xFF),
        static_cast<uint8_t>(hex & 0xFF));
}

inline Color color_from_hex_alpha(uint32_t hex) {
    return Color::rgba8(
        static_cast<uint8_t>((hex >> 24) & 0xFF),
        static_cast<uint8_t>((hex >> 16) & 0xFF),
        static_cast<uint8_t>((hex >> 8) & 0xFF),
        static_cast<uint8_t>(hex & 0xFF));
}

// Design tokens define the visual language
// Tokens are named values — colors, dimensions, strings
struct Theme {
    // Token storage — flat map from token name to value
    std::unordered_map<std::string, Color> colors;
    std::unordered_map<std::string, float> dimensions;
    std::unordered_map<std::string, std::string> strings;

    // Look up a color token, returns nullopt if not found
    std::optional<Color> color(const std::string& name) const;

    // Look up a dimension token (spacing, radius, font size, etc.)
    std::optional<float> dimension(const std::string& name) const;

    // Look up a string token (font family, etc.)
    std::optional<std::string> string_token(const std::string& name) const;

    // Merge another theme on top (overrides values)
    void apply_overrides(const Theme& overrides);

    // Load from JSON string (choc::json format)
    static Theme from_json(const std::string& json);

    // Serialize to JSON string
    std::string to_json() const;

    // Built-in themes
    static Theme dark();
    static Theme light();
    static Theme pro_audio();

    // ── Import/Export ────────────────────────────────────────────────────────

    /// Save theme to a JSON file. Returns true on success.
    bool save_to_file(const std::string& path) const;

    /// Load theme from a JSON file. Returns empty Theme on failure.
    static Theme load_from_file(const std::string& path);

    // ── Validation ──────────────────────────────────────────────────────────

    /// List of required color tokens that every complete theme must define.
    static const std::vector<std::string>& required_color_tokens();

    /// Check if this theme has all required tokens. Returns list of missing token names.
    std::vector<std::string> missing_tokens() const;

    /// Returns true if this theme defines all required tokens.
    bool is_complete() const;

    /// Merge with a base theme to fill in any missing tokens.
    /// Missing tokens in *this are filled from base. Existing tokens are kept.
    void fill_from(const Theme& base);
};

} // namespace pulp::view
