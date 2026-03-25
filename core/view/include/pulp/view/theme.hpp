#pragma once

#include <string>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace pulp::view {

// RGBA color (0-255 per channel)
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;

    static Color from_hex(uint32_t hex) {
        return {
            static_cast<uint8_t>((hex >> 16) & 0xFF),
            static_cast<uint8_t>((hex >> 8) & 0xFF),
            static_cast<uint8_t>(hex & 0xFF),
            255
        };
    }

    static Color from_hex_alpha(uint32_t hex) {
        return {
            static_cast<uint8_t>((hex >> 24) & 0xFF),
            static_cast<uint8_t>((hex >> 16) & 0xFF),
            static_cast<uint8_t>((hex >> 8) & 0xFF),
            static_cast<uint8_t>(hex & 0xFF)
        };
    }

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

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
};

} // namespace pulp::view
