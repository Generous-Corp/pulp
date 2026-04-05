#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/theme_presets.hpp>
#include <pulp/view/theme_contrast.hpp>

using namespace pulp::view;

// ── Preset Library ──────────────────────────────────────────────────────────

TEST_CASE("All presets are available", "[view][presets]") {
    auto& presets = all_presets();
    REQUIRE(presets.size() == 37);
}

TEST_CASE("Preset IDs are unique", "[view][presets]") {
    auto ids = preset_ids();
    std::sort(ids.begin(), ids.end());
    auto dups = std::adjacent_find(ids.begin(), ids.end());
    REQUIRE(dups == ids.end());
}

TEST_CASE("Each preset has non-empty name and id", "[view][presets]") {
    for (auto& p : all_presets()) {
        REQUIRE_FALSE(p.id.empty());
        REQUIRE_FALSE(p.name.empty());
    }
}

TEST_CASE("find_preset returns correct preset", "[view][presets]") {
    auto* p = find_preset("catppuccin");
    REQUIRE(p != nullptr);
    REQUIRE(p->name == "Catppuccin");
}

TEST_CASE("find_preset returns nullptr for unknown", "[view][presets]") {
    REQUIRE(find_preset("nonexistent") == nullptr);
}

// ── Derivation Layer ────────────────────────────────────────────────────────

TEST_CASE("Derived theme has all required tokens", "[view][presets][derive]") {
    auto& presets = all_presets();
    for (auto& preset : presets) {
        INFO("Preset: " << preset.id);
        auto theme_dark = theme_from_preset(preset, true);
        auto theme_light = theme_from_preset(preset, false);

        // Check required color tokens
        REQUIRE(theme_dark.color("bg.primary").has_value());
        REQUIRE(theme_dark.color("text.primary").has_value());
        REQUIRE(theme_dark.color("accent.primary").has_value());
        REQUIRE(theme_dark.color("control.fill").has_value());
        REQUIRE(theme_dark.color("knob.arc").has_value());
        REQUIRE(theme_dark.color("slider.fill").has_value());
        REQUIRE(theme_dark.color("meter.green").has_value());
        REQUIRE(theme_dark.color("meter.yellow").has_value());
        REQUIRE(theme_dark.color("meter.red").has_value());
        REQUIRE(theme_dark.color("waveform.line").has_value());
        REQUIRE(theme_dark.color("card.empty").has_value());
        REQUIRE(theme_dark.color("tab.active").has_value());
        REQUIRE(theme_dark.color("progress.fill").has_value());
        REQUIRE(theme_dark.color("gradient.start").has_value());

        REQUIRE(theme_light.color("bg.primary").has_value());
        REQUIRE(theme_light.color("text.primary").has_value());

        // Check dimensions
        REQUIRE(theme_dark.dimension("spacing.md").has_value());
        REQUIRE(theme_dark.dimension("font.md").has_value());
        REQUIRE(theme_dark.dimension("radius.md").has_value());

        // Check strings
        REQUIRE(theme_dark.string_token("font.family").has_value());
    }
}

TEST_CASE("Derived theme light/dark backgrounds differ", "[view][presets][derive]") {
    auto* p = find_preset("modern-minimal");
    REQUIRE(p != nullptr);

    auto dark = theme_from_preset(*p, true);
    auto light = theme_from_preset(*p, false);

    auto dark_bg = dark.color("bg.primary").value();
    auto light_bg = light.color("bg.primary").value();

    // Dark bg should be darker than light bg
    float dark_lum = relative_luminance(dark_bg);
    float light_lum = relative_luminance(light_bg);
    REQUIRE(dark_lum < light_lum);
}

TEST_CASE("Derived theme JSON round-trip", "[view][presets][derive]") {
    auto* p = find_preset("claude");
    REQUIRE(p != nullptr);

    auto original = theme_from_preset(*p, true);
    auto json = original.to_json();
    auto restored = Theme::from_json(json);

    auto orig_bg = original.color("bg.primary").value();
    auto rest_bg = restored.color("bg.primary").value();
    REQUIRE(orig_bg == rest_bg);

    auto orig_knob = original.color("knob.arc").value();
    auto rest_knob = restored.color("knob.arc").value();
    REQUIRE(orig_knob == rest_knob);
}

// ── Specific Presets ────────────────────────────────────────────────────────

TEST_CASE("Known presets exist", "[view][presets]") {
    std::vector<std::string> expected = {
        "modern-minimal", "violet-bloom", "t3-chat", "twitter",
        "mocha-mousse", "bubblegum", "amethyst-haze", "notebook",
        "doom-64", "catppuccin", "graphite", "perpetuity",
        "kodama-grove", "cosmic-night", "tangerine", "quantum-rose",
        "nature", "bold-tech", "elegant-luxury", "amber-minimal",
        "supabase", "neo-brutalism", "solar-dusk", "claymorphism",
        "cyberpunk", "pastel-dreams", "clean-slate", "caffeine",
        "ocean-breeze", "retro-arcade", "midnight-bloom", "candyland",
        "northern-lights", "vintage-paper", "sunset-horizon",
        "starry-night", "claude"
    };

    for (auto& id : expected) {
        INFO("Missing preset: " << id);
        REQUIRE(find_preset(id) != nullptr);
    }
}
