#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/theme.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("Color from hex", "[view][theme]") {
    auto c = color_from_hex(0xFF8800);
    REQUIRE(c.r == 0xFF);
    REQUIRE(c.g == 0x88);
    REQUIRE(c.b == 0x00);
    REQUIRE(c.a == 0xFF);

    auto c2 = color_from_hex_alpha(0xFF880080);
    REQUIRE(c2.r == 0xFF);
    REQUIRE(c2.g == 0x88);
    REQUIRE(c2.b == 0x00);
    REQUIRE(c2.a == 0x80);
}

TEST_CASE("Theme dark has required tokens", "[view][theme]") {
    auto theme = Theme::dark();

    REQUIRE(theme.color("bg.primary").has_value());
    REQUIRE(theme.color("text.primary").has_value());
    REQUIRE(theme.color("accent.primary").has_value());
    REQUIRE(theme.color("control.fill").has_value());

    REQUIRE(theme.dimension("spacing.md").has_value());
    REQUIRE(theme.dimension("radius.md").has_value());
    REQUIRE(theme.dimension("font.md").has_value());
    REQUIRE(theme.dimension("control.knob_size").has_value());

    REQUIRE(theme.string_token("font.family").has_value());
}

TEST_CASE("Theme light has required tokens", "[view][theme]") {
    auto theme = Theme::light();

    REQUIRE(theme.color("bg.primary").has_value());
    REQUIRE(theme.color("text.primary").has_value());

    // Light and dark should have different backgrounds
    auto dark_bg = Theme::dark().color("bg.primary");
    auto light_bg = theme.color("bg.primary");
    REQUIRE_FALSE(dark_bg.value() == light_bg.value());
}

TEST_CASE("Theme pro_audio has required tokens", "[view][theme]") {
    auto theme = Theme::pro_audio();

    REQUIRE(theme.color("bg.primary").has_value());
    REQUIRE(theme.dimension("control.knob_size").has_value());

    // Pro audio has tighter spacing
    auto dark_knob = Theme::dark().dimension("control.knob_size").value();
    auto pro_knob = theme.dimension("control.knob_size").value();
    REQUIRE(pro_knob < dark_knob);
}

TEST_CASE("Theme apply_overrides", "[view][theme]") {
    auto base = Theme::dark();
    Theme overrides;
    overrides.colors["bg.primary"] = color_from_hex(0xFF0000);
    overrides.dimensions["spacing.md"] = 99.0f;

    base.apply_overrides(overrides);

    REQUIRE(base.color("bg.primary")->r == 0xFF);
    REQUIRE(base.color("bg.primary")->g == 0x00);
    REQUIRE_THAT(base.dimension("spacing.md").value(), WithinAbs(99.0, 0.001));

    // Non-overridden tokens should still be there
    REQUIRE(base.color("text.primary").has_value());
}

TEST_CASE("Theme JSON round-trip", "[view][theme]") {
    auto original = Theme::dark();
    auto json = original.to_json();
    REQUIRE_FALSE(json.empty());

    auto restored = Theme::from_json(json);

    // Verify colors round-trip
    auto orig_bg = original.color("bg.primary").value();
    auto rest_bg = restored.color("bg.primary").value();
    REQUIRE(orig_bg == rest_bg);

    // Verify dimensions round-trip
    auto orig_sp = original.dimension("spacing.md").value();
    auto rest_sp = restored.dimension("spacing.md").value();
    REQUIRE_THAT(rest_sp, WithinAbs(orig_sp, 0.001));

    // Verify strings round-trip
    auto orig_font = original.string_token("font.family").value();
    auto rest_font = restored.string_token("font.family").value();
    REQUIRE(orig_font == rest_font);
}

TEST_CASE("Theme missing token returns nullopt", "[view][theme]") {
    Theme empty;
    REQUIRE_FALSE(empty.color("nonexistent").has_value());
    REQUIRE_FALSE(empty.dimension("nonexistent").has_value());
    REQUIRE_FALSE(empty.string_token("nonexistent").has_value());
}

// ── Motion token tests ──────────────────────────────────────────────────────

TEST_CASE("Dark theme has motion duration tokens", "[view][theme][motion]") {
    auto theme = Theme::dark();
    REQUIRE(theme.dimension("motion.duration.fast").has_value());
    REQUIRE(theme.dimension("motion.duration.normal").has_value());
    REQUIRE(theme.dimension("motion.duration.slow").has_value());
    REQUIRE(theme.dimension("motion.duration.meter_decay").has_value());
    REQUIRE(theme.dimension("motion.duration.peak_hold").has_value());

    // Verify ordering: fast < normal < slow
    float fast = theme.dimension("motion.duration.fast").value();
    float normal = theme.dimension("motion.duration.normal").value();
    float slow = theme.dimension("motion.duration.slow").value();
    REQUIRE(fast < normal);
    REQUIRE(normal < slow);
}

TEST_CASE("Dark theme has motion easing tokens", "[view][theme][motion]") {
    auto theme = Theme::dark();
    REQUIRE(theme.string_token("motion.easing.interaction").has_value());
    REQUIRE(theme.string_token("motion.easing.enter").has_value());
    REQUIRE(theme.string_token("motion.easing.exit").has_value());
}

TEST_CASE("Light theme inherits motion tokens from dark", "[view][theme][motion]") {
    auto theme = Theme::light();
    REQUIRE(theme.dimension("motion.duration.fast").has_value());
    REQUIRE(theme.string_token("motion.easing.interaction").has_value());
}

TEST_CASE("Pro audio theme has snappier motion", "[view][theme][motion]") {
    auto dark = Theme::dark();
    auto pro = Theme::pro_audio();

    float dark_fast = dark.dimension("motion.duration.fast").value();
    float pro_fast = pro.dimension("motion.duration.fast").value();
    REQUIRE(pro_fast <= dark_fast);

    float dark_normal = dark.dimension("motion.duration.normal").value();
    float pro_normal = pro.dimension("motion.duration.normal").value();
    REQUIRE(pro_normal <= dark_normal);
}

TEST_CASE("Motion tokens survive JSON round-trip", "[view][theme][motion]") {
    auto original = Theme::dark();
    auto json = original.to_json();
    auto restored = Theme::from_json(json);

    auto orig_fast = original.dimension("motion.duration.fast").value();
    auto rest_fast = restored.dimension("motion.duration.fast").value();
    REQUIRE_THAT(rest_fast, WithinAbs(orig_fast, 0.001));

    auto orig_ease = original.string_token("motion.easing.interaction").value();
    auto rest_ease = restored.string_token("motion.easing.interaction").value();
    REQUIRE(orig_ease == rest_ease);
}

TEST_CASE("Theme from_json with custom tokens", "[view][theme]") {
    auto json = R"({
        "colors": {
            "custom.accent": "#ff6600"
        },
        "dimensions": {
            "custom.size": 42.5
        },
        "strings": {
            "custom.label": "My Plugin"
        }
    })";

    auto theme = Theme::from_json(json);
    REQUIRE(theme.color("custom.accent")->r == 0xFF);
    REQUIRE(theme.color("custom.accent")->g == 0x66);
    REQUIRE(theme.color("custom.accent")->b == 0x00);
    REQUIRE_THAT(theme.dimension("custom.size").value(), WithinAbs(42.5, 0.001));
    REQUIRE(theme.string_token("custom.label").value() == "My Plugin");
}
