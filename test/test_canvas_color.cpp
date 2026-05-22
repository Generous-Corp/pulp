#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>

using Catch::Matchers::WithinAbs;
using pulp::canvas::Color;

namespace {

void require_near(float actual, float expected, float margin = 0.001f) {
    REQUIRE_THAT(actual, WithinAbs(expected, margin));
}

void require_color_near(const Color& color,
                        float r,
                        float g,
                        float b,
                        float a = 1.0f,
                        float margin = 0.001f) {
    require_near(color.r, r, margin);
    require_near(color.g, g, margin);
    require_near(color.b, b, margin);
    require_near(color.a, a, margin);
}

}  // namespace

TEST_CASE("Color HSV covers primary and secondary hue sectors",
          "[canvas][color][coverage][phase3]") {
    struct Case {
        const char* name;
        float hue;
        Color color;
    };

    const Case cases[] = {
        {"red", 0.0f, Color::rgba(1.0f, 0.0f, 0.0f)},
        {"yellow", 60.0f, Color::rgba(1.0f, 1.0f, 0.0f)},
        {"green", 120.0f, Color::rgba(0.0f, 1.0f, 0.0f)},
        {"cyan", 180.0f, Color::rgba(0.0f, 1.0f, 1.0f)},
        {"blue", 240.0f, Color::rgba(0.0f, 0.0f, 1.0f)},
        {"magenta", 300.0f, Color::rgba(1.0f, 0.0f, 1.0f)},
    };

    for (const auto& entry : cases) {
        CAPTURE(entry.name);
        auto hsv = entry.color.to_hsv();
        require_near(hsv.h, entry.hue, 0.01f);
        require_near(hsv.s, 1.0f);
        require_near(hsv.v, 1.0f);

        auto round_trip = Color::from_hsv({entry.hue, 1.0f, 1.0f}, 0.625f);
        require_color_near(round_trip,
                           entry.color.r,
                           entry.color.g,
                           entry.color.b,
                           0.625f);
    }
}

TEST_CASE("Color OKLCH covers sRGB transfer thresholds and wrapped hue",
          "[canvas][color][coverage][phase3]") {
    auto near_black = Color::rgba(0.003f, 0.003f, 0.003f).to_oklch();
    require_near(near_black.L, 0.06146f, 0.0002f);
    require_near(near_black.C, 0.0f, 0.0001f);
    REQUIRE(near_black.h >= 0.0f);
    REQUIRE(near_black.h < 360.0f);

    auto blue = Color::rgba(0.0f, 0.0f, 1.0f).to_oklch();
    REQUIRE(blue.h > 250.0f);
    REQUIRE(blue.h < 280.0f);
    REQUIRE(blue.C > 0.25f);

    auto round_trip = Color::from_oklch(blue, 0.5f);
    require_color_near(round_trip, 0.0f, 0.0f, 1.0f, 0.5f, 0.02f);
}

TEST_CASE("Color binary encoding preserves unclamped float channels",
          "[canvas][color][coverage][phase3]") {
    auto original = Color::rgba(1.25f, -0.125f, 0.5f, 0.75f);

    uint8_t bytes[16]{};
    original.encode(bytes);
    auto decoded = Color::decode(bytes);

    require_color_near(decoded, 1.25f, -0.125f, 0.5f, 0.75f);
}
