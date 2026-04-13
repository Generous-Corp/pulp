// Tests for SdfEffect mask + presets (Phase 4 host-side API).

#include <pulp/canvas/sdf_effects.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp::canvas;

TEST_CASE("SdfEffect bitmask composes with |", "[canvas][sdf][effects]") {
    const std::uint8_t mask = SdfEffect::Outline | SdfEffect::Shadow;
    REQUIRE(has_effect(mask, SdfEffect::Outline));
    REQUIRE(has_effect(mask, SdfEffect::Shadow));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::Glow));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::Bevel));
    REQUIRE_FALSE(has_effect(mask, SdfEffect::None));
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
