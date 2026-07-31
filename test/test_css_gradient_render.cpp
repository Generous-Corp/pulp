// CSS gradient strings, judged by the pixels they produce.
//
// A gradient setter accepting a string proves nothing — every defect this file
// covers was a string the parser ACCEPTED and then painted wrong. So each case
// renders the CSS under test beside a hand-written equivalent that means the
// same thing, and requires the two images to match. Where a form is expected to
// be refused, the refusal itself is asserted rather than inferred from a blank
// image.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/css_gradient.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <vector>

using namespace pulp::view;

namespace {

constexpr int kSize = 160;

// Renders a background gradient over a fixed box. The box is square so a conic
// sweep is radially symmetric and a missing wedge shows up as a flat region
// rather than as an artefact of the aspect ratio.
std::vector<uint8_t> render_css(const std::string& css, bool* applied = nullptr) {
    View v;
    v.set_bounds({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize)});
    v.set_background_color({20, 20, 20, 255});
    const bool ok = apply_css_background_gradient(v, css);
    if (applied != nullptr) *applied = ok;
    return render_to_png(v, kSize, kSize, 1.0f, ScreenshotBackend::skia);
}

// Two CSS values that mean the same thing must paint the same pixels. Exact
// rather than tolerant: both sides run through one renderer on one machine, so
// any difference at all is the parser disagreeing with itself.
void require_same_render(const std::string& a, const std::string& b) {
    bool a_applied = false, b_applied = false;
    const auto lhs = render_css(a, &a_applied);
    const auto rhs = render_css(b, &b_applied);
    INFO("A: " << a << "\nB: " << b);
    REQUIRE(a_applied);
    REQUIRE(b_applied);
    // A pair of blank renders would satisfy any similarity check. Require the
    // reference to actually contain a gradient first.
    const auto content = analyze_screenshot_content(rhs);
    INFO("reference luminance stddev: " << content.luminance_stddev);
    REQUIRE(content.luminance_stddev > 1.0);
    const auto cmp = compare_screenshots(lhs, rhs);
    INFO("similarity " << cmp.similarity << " mean_error " << cmp.mean_error);
    CHECK(cmp.similarity > 0.999f);
}

}  // namespace

// A sweep covers the whole circle. Skia clamps angles outside the shader's
// [start, end] window instead of wrapping, so passing the CSS rotation as the
// window's start left the rest of the turn painted flat in the last stop's
// colour: `from 0deg` — the default — lost a 90° wedge, because the parser
// applies a -90° correction to make 0deg point up. Rotating the shader keeps
// the window at a full turn, so no angle falls outside it.
TEST_CASE("a conic gradient covers the whole circle at any start angle",
          "[view][gradient][conic]") {
    // A pattern whose period is a quarter turn is unchanged by a quarter-turn
    // rotation, so every one of these must render the SAME image. That is what
    // makes the wedge visible without needing to rotate a bitmap to compare:
    // the gradient is rotation-invariant, the defect is not — it parks a flat
    // clamped region wherever the start angle happens to fall.
    //
    // The ramp is smooth rather than hard-banded on purpose. A rotation is
    // applied as a shader matrix, and resampling a hard colour edge through one
    // costs a pixel or two of difference along every edge — enough to force a
    // loose threshold that a real defect could hide under. With no edges to
    // resample the invariance is exact, so the comparison below can stay strict.
    std::string quarter_periodic = "conic-gradient(from ANGLE";
    for (int i = 0; i < 4; ++i) {
        const float a = static_cast<float>(i) * 25.0f;
        quarter_periodic += ", #ff0000 " + std::to_string(a) + "%";
        quarter_periodic += ", #00ff00 " + std::to_string(a + 12.5f) + "%";
    }
    quarter_periodic += ", #ff0000 100%)";

    const auto at = [&](const char* angle) {
        std::string css = quarter_periodic;
        css.replace(css.find("ANGLE"), 5, angle);
        bool applied = false;
        auto png = render_css(css, &applied);
        REQUIRE(applied);
        return png;
    };

    // 90deg is the one start angle that already worked — its -90° correction
    // lands on a zero offset, leaving the shader window at a full turn — so it
    // is the reference the other rotations are held to.
    const auto reference = at("90deg");
    REQUIRE(analyze_screenshot_content(reference).luminance_stddev > 1.0);
    for (const char* rotated : {"0deg", "180deg", "270deg"}) {
        INFO("from " << rotated);
        const auto cmp = compare_screenshots(at(rotated), reference);
        INFO("similarity " << cmp.similarity << " mean_error " << cmp.mean_error);
        CHECK(cmp.similarity > 0.999f);
    }
}

// A stop's position carries a unit and the unit decides its meaning. stof stops
// at the first non-digit, so `180deg` used to parse as the raw number 180 and
// land at 18000% — clamped to the end of the ramp — while `50%` parsed
// correctly. Every conic in the wild is authored in degrees.
TEST_CASE("conic gradient stops honour angular units",
          "[view][gradient][conic]") {
    SECTION("degrees are a fraction of a turn, not a raw number") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0deg, #00ff00 180deg, "
            "#ff0000 360deg)",
            "conic-gradient(from 90deg, #ff0000 0%, #00ff00 50%, "
            "#ff0000 100%)");
    }
    SECTION("turns") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0turn, #00ff00 0.25turn, "
            "#ff0000 1turn)",
            "conic-gradient(from 90deg, #ff0000 0%, #00ff00 25%, "
            "#ff0000 100%)");
    }
    SECTION("a degree stop is no longer the same as the bare number") {
        // The exact confusion being fixed: `180deg` and `180` must now differ.
        bool a = false, b = false;
        const auto with_unit = render_css(
            "conic-gradient(from 90deg, #ff0000 0deg, #00ff00 180deg, "
            "#ff0000 360deg)", &a);
        const auto bare = render_css(
            "conic-gradient(from 90deg, #ff0000 0, #00ff00 180, #ff0000 360)",
            &b);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(compare_screenshots(with_unit, bare).similarity < 0.999f);
    }
}

// One stop, two positions: `#fff 0deg 2deg` is shorthand for the colour running
// from 0deg to 2deg. Only one trailing token used to be peeled, so the first
// position stayed glued to the colour and took the colour down with it.
TEST_CASE("a gradient stop may carry two positions",
          "[view][gradient][conic]") {
    SECTION("percent") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0% 50%, #00ff00 50% 100%)",
            "conic-gradient(from 90deg, #ff0000 0%, #ff0000 50%, "
            "#00ff00 50%, #00ff00 100%)");
    }
    SECTION("degrees") {
        require_same_render(
            "conic-gradient(from 90deg, #ff0000 0deg 180deg, "
            "#00ff00 180deg 360deg)",
            "conic-gradient(from 90deg, #ff0000 0deg, #ff0000 180deg, "
            "#00ff00 180deg, #00ff00 360deg)");
    }
    SECTION("linear gradients too") {
        require_same_render(
            "linear-gradient(to right, #ff0000 0% 50%, #00ff00 50% 100%)",
            "linear-gradient(to right, #ff0000 0%, #ff0000 50%, "
            "#00ff00 50%, #00ff00 100%)");
    }
}

// The repeat is Skia's: a sweep shader given a sub-turn window and a repeating
// tile mode tiles the band around the circle. Before this the whole value was
// refused by the prefix match and nothing painted at all.
TEST_CASE("repeating-conic-gradient tiles its band around the circle",
          "[view][gradient][conic]") {
    // Six 60° bands written out explicitly is what one 60° repeating band
    // means. Both sides are the same shader with the same stops; only the way
    // the repetition is expressed differs.
    std::string explicit_bands = "conic-gradient(from 90deg";
    for (int i = 0; i < 6; ++i) {
        const int a = i * 60;
        explicit_bands += ", #ff0000 " + std::to_string(a) + "deg";
        explicit_bands += ", #ff0000 " + std::to_string(a + 30) + "deg";
        explicit_bands += ", #00ff00 " + std::to_string(a + 30) + "deg";
        explicit_bands += ", #00ff00 " + std::to_string(a + 60) + "deg";
    }
    explicit_bands += ")";
    require_same_render(
        "repeating-conic-gradient(from 90deg, #ff0000 0deg 30deg, "
        "#00ff00 30deg 60deg)",
        explicit_bands);
}
