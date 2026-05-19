#include <pulp/view/theme.hpp>
#include <choc/text/choc_JSON.h>
#include <sstream>
#include <fstream>

namespace pulp::view {

std::optional<Color> Theme::color(const std::string& name) const {
    auto it = colors.find(name);
    if (it != colors.end()) return it->second;
    return std::nullopt;
}

std::optional<float> Theme::dimension(const std::string& name) const {
    auto it = dimensions.find(name);
    if (it != dimensions.end()) return it->second;
    return std::nullopt;
}

std::optional<std::string> Theme::string_token(const std::string& name) const {
    auto it = strings.find(name);
    if (it != strings.end()) return it->second;
    return std::nullopt;
}

void Theme::apply_overrides(const Theme& overrides) {
    for (auto& [k, v] : overrides.colors) colors[k] = v;
    for (auto& [k, v] : overrides.dimensions) dimensions[k] = v;
    for (auto& [k, v] : overrides.strings) strings[k] = v;
}

static Color parse_hex_color(const std::string& hex) {
    if (hex.empty() || hex[0] != '#') return {};
    uint32_t val = 0;
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(hex.substr(1), &consumed, 16);
        if (consumed != hex.size() - 1 || parsed > 0xFFFFFFFFu) return {};
        val = static_cast<uint32_t>(parsed);
    } catch (...) {
        return {};
    }
    if (hex.size() == 7) return color_from_hex(static_cast<uint32_t>(val));
    if (hex.size() == 9) return color_from_hex_alpha(static_cast<uint32_t>(val));
    return {};
}

static std::string color_to_hex(const Color& c) {
    char buf[10];
    if (c.a8() == 255)
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r8(), c.g8(), c.b8());
    else
        snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.r8(), c.g8(), c.b8(), c.a8());
    return buf;
}

Theme Theme::from_json(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    if (root.hasObjectMember("colors")) {
        auto obj = root["colors"];
        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            theme.colors[std::string(member.name)] = parse_hex_color(std::string(member.value.toString()));
        }
    }

    if (root.hasObjectMember("dimensions")) {
        auto obj = root["dimensions"];
        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            theme.dimensions[std::string(member.name)] = static_cast<float>(member.value.getWithDefault<double>(0.0));
        }
    }

    if (root.hasObjectMember("strings")) {
        auto obj = root["strings"];
        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            theme.strings[std::string(member.name)] = std::string(member.value.toString());
        }
    }

    return theme;
}

std::string Theme::to_json() const {
    auto root = choc::value::createObject("");

    auto colors_obj = choc::value::createObject("");
    for (auto& [k, v] : colors)
        colors_obj.addMember(k, choc::value::createString(color_to_hex(v)));
    root.addMember("colors", colors_obj);

    auto dims_obj = choc::value::createObject("");
    for (auto& [k, v] : dimensions)
        dims_obj.addMember(k, choc::value::createFloat64(v));
    root.addMember("dimensions", dims_obj);

    auto strings_obj = choc::value::createObject("");
    for (auto& [k, v] : strings)
        strings_obj.addMember(k, choc::value::createString(v));
    root.addMember("strings", strings_obj);

    return choc::json::toString(root, true);
}

Theme Theme::dark() {
    Theme t;

    // Background
    t.colors["bg.primary"]     = color_from_hex(0x1E1E2E);
    t.colors["bg.secondary"]   = color_from_hex(0x2A2A3C);
    t.colors["bg.surface"]     = color_from_hex(0x313244);
    t.colors["bg.elevated"]    = color_from_hex(0x45475A);

    // Text
    t.colors["text.primary"]   = color_from_hex(0xCDD6F4);
    t.colors["text.secondary"] = color_from_hex(0xA6ADC8);
    t.colors["text.disabled"]  = color_from_hex(0x6C7086);

    // Accent
    t.colors["accent.primary"]   = color_from_hex(0x89B4FA);
    t.colors["accent.secondary"] = color_from_hex(0xF5C2E7);
    t.colors["accent.success"]   = color_from_hex(0xA6E3A1);
    t.colors["accent.warning"]   = color_from_hex(0xFAB387);
    t.colors["accent.error"]     = color_from_hex(0xF38BA8);

    // Controls
    t.colors["control.track"]    = color_from_hex(0x45475A);
    t.colors["control.fill"]     = color_from_hex(0x89B4FA);
    t.colors["control.thumb"]    = color_from_hex(0xCDD6F4);
    t.colors["control.border"]   = color_from_hex(0x585B70);

    // Dimensions
    t.dimensions["spacing.xs"]   = 2.0f;
    t.dimensions["spacing.sm"]   = 4.0f;
    t.dimensions["spacing.md"]   = 8.0f;
    t.dimensions["spacing.lg"]   = 16.0f;
    t.dimensions["spacing.xl"]   = 24.0f;

    t.dimensions["radius.sm"]    = 4.0f;
    t.dimensions["radius.md"]    = 8.0f;
    t.dimensions["radius.lg"]    = 12.0f;
    t.dimensions["radius.full"]  = 9999.0f;

    t.dimensions["font.xs"]      = 10.0f;
    t.dimensions["font.sm"]      = 12.0f;
    t.dimensions["font.md"]      = 14.0f;
    t.dimensions["font.lg"]      = 18.0f;
    t.dimensions["font.xl"]      = 24.0f;

    t.dimensions["control.knob_size"]   = 48.0f;
    t.dimensions["control.fader_width"] = 24.0f;
    t.dimensions["control.meter_width"] = 12.0f;

    // Motion durations (seconds)
    t.dimensions["motion.duration.fast"]       = 0.08f;
    t.dimensions["motion.duration.normal"]     = 0.15f;
    t.dimensions["motion.duration.slow"]       = 0.30f;
    t.dimensions["motion.duration.meter_decay"] = 0.30f;
    t.dimensions["motion.duration.peak_hold"]  = 1.50f;

    // Strings
    t.strings["font.family"] = "Inter";
    t.strings["font.mono"]   = "JetBrains Mono";

    // Motion easing names
    t.strings["motion.easing.interaction"] = "ease_out_cubic";
    t.strings["motion.easing.enter"]       = "ease_out_quad";
    t.strings["motion.easing.exit"]        = "ease_in_quad";

    return t;
}

Theme Theme::light() {
    Theme t;

    t.colors["bg.primary"]     = color_from_hex(0xEFF1F5);
    t.colors["bg.secondary"]   = color_from_hex(0xE6E9EF);
    t.colors["bg.surface"]     = color_from_hex(0xDCE0E8);
    t.colors["bg.elevated"]    = color_from_hex(0xCCD0DA);

    t.colors["text.primary"]   = color_from_hex(0x4C4F69);
    t.colors["text.secondary"] = color_from_hex(0x6C6F85);
    t.colors["text.disabled"]  = color_from_hex(0x9CA0B0);

    t.colors["accent.primary"]   = color_from_hex(0x1E66F5);
    t.colors["accent.secondary"] = color_from_hex(0xEA76CB);
    t.colors["accent.success"]   = color_from_hex(0x40A02B);
    t.colors["accent.warning"]   = color_from_hex(0xFE640B);
    t.colors["accent.error"]     = color_from_hex(0xD20F39);

    t.colors["control.track"]    = color_from_hex(0xCCD0DA);
    t.colors["control.fill"]     = color_from_hex(0x1E66F5);
    t.colors["control.thumb"]    = color_from_hex(0xFFFFFF);
    t.colors["control.border"]   = color_from_hex(0xACB0BE);

    // Same dimensions and strings as dark
    auto dark_theme = dark();
    t.dimensions = dark_theme.dimensions;
    t.strings = dark_theme.strings;

    return t;
}

Theme Theme::pro_audio() {
    Theme t;

    t.colors["bg.primary"]     = color_from_hex(0x141414);
    t.colors["bg.secondary"]   = color_from_hex(0x1C1C1C);
    t.colors["bg.surface"]     = color_from_hex(0x242424);
    t.colors["bg.elevated"]    = color_from_hex(0x2C2C2C);

    t.colors["text.primary"]   = color_from_hex(0xD4D4D4);
    t.colors["text.secondary"] = color_from_hex(0x8A8A8A);
    t.colors["text.disabled"]  = color_from_hex(0x505050);

    t.colors["accent.primary"]   = color_from_hex(0x3B82F6);
    t.colors["accent.secondary"] = color_from_hex(0x8B5CF6);
    t.colors["accent.success"]   = color_from_hex(0x22C55E);
    t.colors["accent.warning"]   = color_from_hex(0xF59E0B);
    t.colors["accent.error"]     = color_from_hex(0xEF4444);

    t.colors["control.track"]    = color_from_hex(0x2C2C2C);
    t.colors["control.fill"]     = color_from_hex(0x3B82F6);
    t.colors["control.thumb"]    = color_from_hex(0xE5E5E5);
    t.colors["control.border"]   = color_from_hex(0x3A3A3A);

    // Tighter spacing for pro audio
    auto dark_theme = dark();
    t.dimensions = dark_theme.dimensions;
    t.dimensions["spacing.md"] = 6.0f;
    t.dimensions["spacing.lg"] = 12.0f;
    t.dimensions["font.md"]    = 12.0f;
    t.dimensions["control.knob_size"] = 40.0f;

    // Snappier motion for pro audio
    t.dimensions["motion.duration.fast"]   = 0.06f;
    t.dimensions["motion.duration.normal"] = 0.12f;
    t.dimensions["motion.duration.slow"]   = 0.25f;

    t.strings = dark_theme.strings;

    return t;
}

// ── Import/Export ────────────────────────────────────────────────────────────

bool Theme::save_to_file(const std::string& path) const {
    auto json = to_json();
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << json;
    return file.good();
}

Theme Theme::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};

    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    if (json.empty()) return {};

    try {
        return from_json(json);
    } catch (...) {
        return {};
    }
}

// ── Validation ──────────────────────────────────────────────────────────────

const std::vector<std::string>& Theme::required_color_tokens() {
    static const std::vector<std::string> tokens = {
        "bg.primary", "bg.secondary", "bg.surface", "bg.elevated",
        "text.primary", "text.secondary", "text.disabled",
        "accent.primary", "accent.secondary", "accent.success",
        "accent.warning", "accent.error",
        "control.track", "control.fill", "control.thumb", "control.border",
    };
    return tokens;
}

std::vector<std::string> Theme::missing_tokens() const {
    std::vector<std::string> missing;
    for (auto& token : required_color_tokens()) {
        if (colors.find(token) == colors.end())
            missing.push_back(token);
    }
    return missing;
}

bool Theme::is_complete() const {
    return missing_tokens().empty();
}

void Theme::fill_from(const Theme& base) {
    for (auto& [k, v] : base.colors) {
        if (colors.find(k) == colors.end()) colors[k] = v;
    }
    for (auto& [k, v] : base.dimensions) {
        if (dimensions.find(k) == dimensions.end()) dimensions[k] = v;
    }
    for (auto& [k, v] : base.strings) {
        if (strings.find(k) == strings.end()) strings[k] = v;
    }
}

} // namespace pulp::view
