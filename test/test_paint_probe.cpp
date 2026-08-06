// Does the paint probe agree with what the renderer actually does?
//
// The probe decides whether whole suites run or skip, so a probe that drifts
// from reality is silent in both directions: too strict and it skips lanes
// that could have caught a paint bug, too loose and every guarded case fails
// for the wrong reason again.
//
// These cases run on EVERY lane and are meaningful on both sides of the
// answer, so a no-Skia lane is not a lane where this file stops testing
// anything.

#include <catch2/catch_test_macros.hpp>

#include "paint_probe.hpp"

#include <pulp/view/geometry.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

using pulp::test::paint_available;

namespace {

/// A tree the renderer has no reason to refuse: one opaque view, no assets, no
/// GPU content, no native overlay.
std::vector<uint8_t> render_plain_fill(uint32_t* w, uint32_t* h) {
    pulp::view::View root;
    root.set_bounds(pulp::view::Rect{0.0f, 0.0f, 16.0f, 16.0f});
    root.set_background_color(pulp::view::Color::rgba8(0, 128, 255));
    return pulp::view::render_to_rgba(root, 16, 16, 1.0f, w, h);
}

}  // namespace

TEST_CASE("the paint probe reports what render_to_rgba actually does",
          "[view][paint-probe]") {
    // Asserted against a SECOND, independent render rather than against the
    // probe's own cached answer, which would agree with itself by
    // construction.
    uint32_t w = 0, h = 0;
    const auto px = render_plain_fill(&w, &h);
    const bool rendered = !px.empty() && w > 0 && h > 0;

    INFO("paint_available() = " << paint_available()
                                << ", independent render produced " << px.size()
                                << " bytes at " << w << "x" << h);
    CHECK(rendered == paint_available());
}

TEST_CASE("the paint probe is stable within a run", "[view][paint-probe]") {
    // The answer is cached, and a cache that returns a different value on the
    // second call would let one case in a file run while its sibling skips.
    const bool first = paint_available();
    CHECK(paint_available() == first);
    CHECK(paint_available() == first);
}

TEST_CASE("a build that can paint puts the requested colour in the buffer",
          "[view][paint-probe]") {
    // The probe deliberately accepts any non-empty buffer, so this is the case
    // that still fails when a rasteriser returns a correctly sized blank. It
    // is the reason the probe is allowed to be that permissive.
    PULP_SKIP_WITHOUT_PAINT("the probe's own colour-reaches-the-buffer control");

    uint32_t w = 0, h = 0;
    const auto px = render_plain_fill(&w, &h);
    REQUIRE_FALSE(px.empty());
    REQUIRE(w == 16u);
    REQUIRE(h == 16u);

    const std::size_t i = (static_cast<std::size_t>(8) * w + 8u) * 4u;
    REQUIRE(i + 3 < px.size());
    INFO("centre rgba " << static_cast<int>(px[i]) << ","
                        << static_cast<int>(px[i + 1]) << ","
                        << static_cast<int>(px[i + 2]) << ","
                        << static_cast<int>(px[i + 3]));
    CHECK(std::abs(static_cast<int>(px[i]) - 0) <= 2);
    CHECK(std::abs(static_cast<int>(px[i + 1]) - 128) <= 2);
    CHECK(std::abs(static_cast<int>(px[i + 2]) - 255) <= 2);
}
