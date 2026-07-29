#include <pulp/view/css_effect_parse.hpp>

#include <unordered_map>

namespace pulp::view {

std::optional<float> css_blur_radius(const std::string& filter) {
    const auto open = filter.find("blur(");
    if (open == std::string::npos) return std::nullopt;
    const auto close = filter.find(')', open);
    if (close == std::string::npos) return std::nullopt;
    const auto inner = filter.substr(open + 5, close - open - 5);
    try {
        const float radius = std::stof(inner);  // stof stops at the unit
        if (!(radius > 0.0f)) return std::nullopt;
        return radius;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<canvas::Canvas::BlendMode> css_blend_mode(const std::string& keyword) {
    using BlendMode = canvas::Canvas::BlendMode;
    static const std::unordered_map<std::string, BlendMode> kModes{
        {"normal", BlendMode::normal},
        {"multiply", BlendMode::multiply},
        {"screen", BlendMode::screen},
        {"overlay", BlendMode::overlay},
        {"darken", BlendMode::darken},
        {"lighten", BlendMode::lighten},
        {"color-dodge", BlendMode::color_dodge},
        {"color-burn", BlendMode::color_burn},
        {"hard-light", BlendMode::hard_light},
        {"soft-light", BlendMode::soft_light},
        {"difference", BlendMode::difference},
        {"exclusion", BlendMode::exclusion},
        {"hue", BlendMode::hue},
        {"saturation", BlendMode::saturation},
        {"color", BlendMode::color},
        {"luminosity", BlendMode::luminosity},
    };
    const auto it = kModes.find(keyword);
    return it == kModes.end() ? std::nullopt
                              : std::optional<BlendMode>(it->second);
}

}  // namespace pulp::view
