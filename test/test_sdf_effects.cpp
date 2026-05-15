// Tests for SdfEffect mask + presets (Phase 4 host-side API).

#include <pulp/canvas/sdf_effects.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp::canvas;

TEST_CASE("SdfEffectParams default to inactive shader-safe values",
          "[canvas][sdf][effects][coverage][issue-650]") {
    SdfEffectParams p;
    REQUIRE(p.active == 0);
    REQUIRE_FALSE(has_effect(p.active, SdfEffect::Outline));
    REQUIRE_FALSE(has_effect(p.active, SdfEffect::Glow));
    REQUIRE_FALSE(has_effect(p.active, SdfEffect::Shadow));
    REQUIRE_FALSE(has_effect(p.active, SdfEffect::Bevel));
    REQUIRE(p.outline_color[3] == 1.0f);
    REQUIRE(p.glow_color[3] == 1.0f);
    REQUIRE(p.shadow_color[3] == 0.5f);
    REQUIRE(p.bevel_mix == 0.0f);
}

TEST_CASE("SdfEffect bitmask composes with |", "[canvas][sdf][effects]") {
    const std::uint8_t mask = SdfEffect::Outline | SdfEffect::Shadow;
    REQUIRE(has_effect(mask, SdfEffect::Outline));
    REQUIRE(has_effect(mask, SdfEffect::Shadow));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::Glow));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::Bevel));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::None));
}

TEST_CASE("SdfEffect bitmask accepts manually combined active masks",
          "[canvas][sdf][effects][coverage][issue-650]") {
    const auto mask = static_cast<std::uint8_t>(SdfEffect::Outline)
                    | static_cast<std::uint8_t>(SdfEffect::Glow)
                    | static_cast<std::uint8_t>(SdfEffect::Bevel);
    REQUIRE(has_effect(mask, SdfEffect::Outline));
    REQUIRE(has_effect(mask, SdfEffect::Glow));
    REQUIRE(has_effect(mask, SdfEffect::Bevel));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::Shadow));
}

TEST_CASE("preset_subtle_shadow has only Shadow active",
          "[canvas][sdf][effects][presets]") {
    auto p = preset_subtle_shadow();
    REQUIRE(has_effect(p.active, SdfEffect::Shadow));
    REQUIRE_FALSE(has_effect(p.active, SdfEffect::Glow));
    REQUIRE(p.shadow_offset_y > 0.0f);
    REQUIRE(p.shadow_color[3] > 0.0f);
}

TEST_CASE("preset_outline width is respected",
          "[canvas][sdf][effects][presets]") {
    auto p = preset_outline(3.0f);
    REQUIRE(has_effect(p.active, SdfEffect::Outline));
    REQUIRE(p.outline_width == 3.0f);
}

TEST_CASE("SDF effect presets preserve explicit zero dimensions",
          "[canvas][sdf][effects][presets][coverage][issue-650]") {
    auto outline = preset_outline(0.0f);
    REQUIRE(has_effect(outline.active, SdfEffect::Outline));
    REQUIRE(outline.outline_width == 0.0f);

    auto glow = preset_glow(0.0f, {0.2f, 0.3f, 0.4f, 0.5f});
    REQUIRE(has_effect(glow.active, SdfEffect::Glow));
    REQUIRE(glow.glow_radius == 0.0f);
    REQUIRE(glow.glow_color[2] == 0.4f);
    REQUIRE(glow.glow_color[3] == 0.5f);
}

TEST_CASE("preset_glow carries the given color",
          "[canvas][sdf][effects][presets]") {
    auto p = preset_glow(8.0f, {1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(has_effect(p.active, SdfEffect::Glow));
    REQUIRE(p.glow_radius == 8.0f);
    REQUIRE(p.glow_color[0] == 1.0f);
    REQUIRE(p.glow_color[1] == 0.0f);
}

TEST_CASE("preset_pressed_bevel uses downward light",
          "[canvas][sdf][effects][presets]") {
    auto p = preset_pressed_bevel();
    REQUIRE(has_effect(p.active, SdfEffect::Bevel));
    REQUIRE(p.bevel_light_y > 0.0f);
    REQUIRE(p.bevel_mix > 0.0f);
}
