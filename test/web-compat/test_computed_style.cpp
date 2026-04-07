// Computed style tests — validates style resolution, inheritance, and theme tokens

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using namespace pulp::canvas;
using Catch::Matchers::WithinAbs;

// ═══════════════════════════════════════════════════════════════════════════════
// Theme color resolution
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: resolve_color finds theme color", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    auto c = root.resolve_color("bg.primary", Color{});
    // Dark theme bg.primary should not be default black
    REQUIRE((c.r != 0 || c.g != 0 || c.b != 0));
}

TEST_CASE("Style: resolve_color returns fallback for unknown token", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    Color fallback{42, 43, 44, 255};
    auto c = root.resolve_color("nonexistent.token", fallback);
    REQUIRE(c == fallback);
}

TEST_CASE("Style: resolve_color inherits from parent", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    auto child = std::make_unique<View>();
    auto* cp = child.get();
    root.add_child(std::move(child));

    // Child should inherit parent's theme tokens
    auto parent_c = root.resolve_color("accent.primary", Color{});
    auto child_c = cp->resolve_color("accent.primary", Color{});
    REQUIRE(parent_c == child_c);
}

TEST_CASE("Style: child theme override takes precedence", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    auto child = std::make_unique<View>();
    child->set_theme(Theme::light());
    auto* cp = child.get();
    root.add_child(std::move(child));

    auto dark_bg = root.resolve_color("bg.primary", Color{});
    auto light_bg = cp->resolve_color("bg.primary", Color{});
    // Light and dark should differ
    REQUIRE_FALSE(dark_bg == light_bg);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Theme dimension resolution
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: resolve_dimension finds theme dimension", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    float val = root.resolve_dimension("spacing.md", 0.0f);
    REQUIRE(val > 0.0f);
}

TEST_CASE("Style: resolve_dimension returns fallback for unknown", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    float val = root.resolve_dimension("nonexistent.dim", 42.0f);
    REQUIRE(val == 42.0f);
}

TEST_CASE("Style: resolve_dimension inherits from parent", "[style][computed]") {
    View root;
    root.set_theme(Theme::dark());
    auto child = std::make_unique<View>();
    auto* cp = child.get();
    root.add_child(std::move(child));

    float parent_val = root.resolve_dimension("radius.md", 0.0f);
    float child_val = cp->resolve_dimension("radius.md", 0.0f);
    REQUIRE(parent_val == child_val);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Theme switching
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: dark theme has expected tokens", "[style][theme]") {
    auto t = Theme::dark();
    REQUIRE(t.color("bg.primary").has_value());
    REQUIRE(t.color("text.primary").has_value());
    REQUIRE(t.color("accent.primary").has_value());
    REQUIRE(t.dimension("spacing.md").has_value());
    REQUIRE(t.dimension("radius.md").has_value());
}

TEST_CASE("Style: light theme has expected tokens", "[style][theme]") {
    auto t = Theme::light();
    REQUIRE(t.color("bg.primary").has_value());
    REQUIRE(t.color("text.primary").has_value());
    REQUIRE(t.color("accent.primary").has_value());
}

TEST_CASE("Style: pro_audio theme has expected tokens", "[style][theme]") {
    auto t = Theme::pro_audio();
    REQUIRE(t.color("bg.primary").has_value());
    REQUIRE(t.dimension("spacing.md").has_value());
}

TEST_CASE("Style: dark and light themes differ in bg.primary", "[style][theme]") {
    auto dark_bg = Theme::dark().color("bg.primary").value();
    auto light_bg = Theme::light().color("bg.primary").value();
    REQUIRE_FALSE(dark_bg == light_bg);
}

TEST_CASE("Style: pro_audio has tighter spacing than dark", "[style][theme]") {
    auto dark_spacing = Theme::dark().dimension("spacing.md").value();
    auto pro_spacing = Theme::pro_audio().dimension("spacing.md").value();
    REQUIRE(pro_spacing <= dark_spacing);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Theme JSON round-trip
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: theme to_json and from_json round-trip", "[style][theme]") {
    auto original = Theme::dark();
    auto json = original.to_json();
    auto restored = Theme::from_json(json);

    auto orig_bg = original.color("bg.primary");
    auto rest_bg = restored.color("bg.primary");
    REQUIRE(orig_bg.has_value());
    REQUIRE(rest_bg.has_value());
    REQUIRE(orig_bg.value() == rest_bg.value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Opacity computed value
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: opacity default is 1.0", "[style][computed]") {
    View v;
    REQUIRE(v.opacity() == 1.0f);
}

TEST_CASE("Style: set_opacity 0.5 reads back correctly", "[style][computed]") {
    View v;
    v.set_opacity(0.5f);
    REQUIRE_THAT(v.opacity(), WithinAbs(0.5f, 0.001f));
}

TEST_CASE("Style: opacity clamps to 0", "[style][computed]") {
    View v;
    v.set_opacity(-1.0f);
    REQUIRE(v.opacity() == 0.0f);
}

TEST_CASE("Style: opacity clamps to 1", "[style][computed]") {
    View v;
    v.set_opacity(5.0f);
    REQUIRE(v.opacity() == 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Background/border computed state
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: no background by default", "[style][computed]") {
    View v;
    REQUIRE_FALSE(v.has_background_color());
}

TEST_CASE("Style: set_background_color makes has_background true", "[style][computed]") {
    View v;
    v.set_background_color(Color::rgba8(255, 0, 0));
    REQUIRE(v.has_background_color());
}

TEST_CASE("Style: clear_background_color resets state", "[style][computed]") {
    View v;
    v.set_background_color(Color::rgba8(255, 0, 0));
    v.clear_background_color();
    REQUIRE_FALSE(v.has_background_color());
}

TEST_CASE("Style: set_border sets corner_radius", "[style][computed]") {
    View v;
    v.set_border(Color::rgba8(0, 0, 0), 2.0f, 8.0f);
    REQUIRE(v.corner_radius() == 8.0f);
}

TEST_CASE("Style: no box_shadow by default", "[style][computed]") {
    View v;
    REQUIRE_FALSE(v.has_box_shadow());
}

TEST_CASE("Style: set_box_shadow makes has_box_shadow true", "[style][computed]") {
    View v;
    v.set_box_shadow(0, 2, 4, 0, Color::rgba8(0, 0, 0, 80));
    REQUIRE(v.has_box_shadow());
}

TEST_CASE("Style: clear_box_shadow resets state", "[style][computed]") {
    View v;
    v.set_box_shadow(0, 2, 4, 0, Color::rgba8(0, 0, 0, 80));
    v.clear_box_shadow();
    REQUIRE_FALSE(v.has_box_shadow());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Visibility computed state
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: visible by default", "[style][computed]") {
    View v;
    REQUIRE(v.visible() == true);
}

TEST_CASE("Style: set_visible false", "[style][computed]") {
    View v;
    v.set_visible(false);
    REQUIRE(v.visible() == false);
}

TEST_CASE("Style: set_visible back to true", "[style][computed]") {
    View v;
    v.set_visible(false);
    v.set_visible(true);
    REQUIRE(v.visible() == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Transform computed state
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: translate stores x and y", "[style][computed]") {
    View v;
    v.set_translate(10.0f, 20.0f);
    REQUIRE(v.translate_x() == 10.0f);
    REQUIRE(v.translate_y() == 20.0f);
}

TEST_CASE("Style: rotation stores degrees", "[style][computed]") {
    View v;
    v.set_rotation(45.0f);
    REQUIRE(v.rotation() == 45.0f);
}

TEST_CASE("Style: scale default is 1.0", "[style][computed]") {
    View v;
    REQUIRE(v.scale() == 1.0f);
}

TEST_CASE("Style: scale stores value", "[style][computed]") {
    View v;
    v.set_scale(2.0f);
    REQUIRE(v.scale() == 2.0f);
}

TEST_CASE("Style: transform_origin default is center (0.5, 0.5)", "[style][computed]") {
    View v;
    REQUIRE(v.transform_origin_x() == 0.5f);
    REQUIRE(v.transform_origin_y() == 0.5f);
}

TEST_CASE("Style: set_transform_origin stores values", "[style][computed]") {
    View v;
    v.set_transform_origin(0.0f, 1.0f);
    REQUIRE(v.transform_origin_x() == 0.0f);
    REQUIRE(v.transform_origin_y() == 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Position computed state
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Style: position default is static", "[style][computed]") {
    View v;
    REQUIRE(v.position() == View::Position::static_);
}

TEST_CASE("Style: set_position absolute", "[style][computed]") {
    View v;
    v.set_position(View::Position::absolute);
    REQUIRE(v.position() == View::Position::absolute);
}

TEST_CASE("Style: set_position relative", "[style][computed]") {
    View v;
    v.set_position(View::Position::relative);
    REQUIRE(v.position() == View::Position::relative);
}

TEST_CASE("Style: set_position fixed", "[style][computed]") {
    View v;
    v.set_position(View::Position::fixed);
    REQUIRE(v.position() == View::Position::fixed);
}
