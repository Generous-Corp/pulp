// test_design_viewport_inverse.cpp — the shared window->root inverse transform
// (WAH-10).
//
// Every plug-in editor host needs to map a click from window space back into
// root/design space so it hit-tests the widget the user visually pointed at.
// Four hosts had their own byte-identical copy of that inverse: the macOS GPU
// and CPU plug-in hosts, the iOS host, and the Windows host. Four copies is
// four chances for one to drift from the paint-side transform, and the symptom
// of that drift — clicks landing on the wrong control — does not look like a
// coordinate bug when you hit it.
//
// The contract these pin is ROUND-TRIP with the forward transform, because
// that is the property that actually matters: the point paint puts on screen
// at X must be the point input recovers from a click at X. Asserting the
// inverse's arithmetic in isolation would let both sides drift together.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/window_host.hpp>

using pulp::view::Point;
using pulp::view::WindowHost;
using Catch::Matchers::WithinAbs;

namespace {

/// Apply the FORWARD transform the way paint does, so a test can check that the
/// inverse undoes exactly what paint did.
Point forward(Point root, float win_w, float win_h, float design_w,
              float design_h, bool top_align = false) {
    float sx, sy, tx, ty;
    REQUIRE(WindowHost::compute_design_viewport_transform(
        win_w, win_h, design_w, design_h, sx, sy, tx, ty, top_align));
    return {root.x * sx + tx, root.y * sy + ty};
}

void require_round_trip(Point root, float win_w, float win_h, float design_w,
                        float design_h, bool top_align = false) {
    const Point window = forward(root, win_w, win_h, design_w, design_h, top_align);
    const Point back = WindowHost::design_viewport_window_to_root(
        window, win_w, win_h, design_w, design_h, top_align);
    REQUIRE_THAT(back.x, WithinAbs(root.x, 0.001f));
    REQUIRE_THAT(back.y, WithinAbs(root.y, 0.001f));
}

}  // namespace

TEST_CASE("no design viewport is the identity", "[design-viewport][wah-10]") {
    // The un-pinned case every caller relies on: a host with no viewport must
    // pass the point through untouched, not scale it by an accidental 1/0.
    const Point pt{123.0f, 45.0f};
    const Point out =
        WindowHost::design_viewport_window_to_root(pt, 800, 600, 0, 0);
    REQUIRE(out.x == pt.x);
    REQUIRE(out.y == pt.y);
}

TEST_CASE("a degenerate window is the identity", "[design-viewport][wah-10]") {
    // A minimized or zero-sized host must not produce a division by zero.
    const Point pt{10.0f, 10.0f};
    const Point out =
        WindowHost::design_viewport_window_to_root(pt, 0, 0, 400, 300);
    REQUIRE(out.x == pt.x);
    REQUIRE(out.y == pt.y);
}

TEST_CASE("uniform scale round-trips", "[design-viewport][wah-10]") {
    // Same aspect: pure 2x scale, no letterbox.
    require_round_trip({0, 0}, 800, 600, 400, 300);
    require_round_trip({400, 300}, 800, 600, 400, 300);
    require_round_trip({123.5f, 47.25f}, 800, 600, 400, 300);
}

TEST_CASE("pillarboxed round-trips", "[design-viewport][wah-10]") {
    // Host wider than the design aspect: horizontal slack, centered.
    require_round_trip({0, 0}, 1200, 600, 400, 300);
    require_round_trip({400, 300}, 1200, 600, 400, 300);
    require_round_trip({77.0f, 211.0f}, 1200, 600, 400, 300);
}

TEST_CASE("letterboxed round-trips", "[design-viewport][wah-10]") {
    // Host taller than the design aspect: vertical slack, centered.
    require_round_trip({0, 0}, 800, 1000, 400, 300);
    require_round_trip({400, 300}, 800, 1000, 400, 300);
    require_round_trip({19.5f, 288.75f}, 800, 1000, 400, 300);
}

TEST_CASE("top-aligned letterbox round-trips", "[design-viewport][wah-10]") {
    // The AU v3 case. top_align MUST match between paint and input mapping;
    // this pins that the inverse honours it rather than always centering.
    require_round_trip({0, 0}, 800, 1000, 400, 300, /*top_align*/ true);
    require_round_trip({400, 300}, 800, 1000, 400, 300, true);
    require_round_trip({150.0f, 90.0f}, 800, 1000, 400, 300, true);
}

TEST_CASE("top-align and centered disagree when there is vertical slack",
          "[design-viewport][wah-10]") {
    // If these ever produced the same answer the top_align flag would be
    // silently dead, and an AU v3 editor's clicks would drift by half the
    // vertical slack without any test noticing.
    const Point window{400.0f, 500.0f};
    const Point centered = WindowHost::design_viewport_window_to_root(
        window, 800, 1000, 400, 300, /*top_align*/ false);
    const Point topped = WindowHost::design_viewport_window_to_root(
        window, 800, 1000, 400, 300, /*top_align*/ true);
    REQUIRE(centered.y != topped.y);
    // Horizontal centering is unchanged by top-align.
    REQUIRE_THAT(centered.x, WithinAbs(topped.x, 0.001f));
}

TEST_CASE("a point outside the design surface maps outside it",
          "[design-viewport][wah-10]") {
    // A click on the letterbox bar must NOT clamp into the design surface —
    // hit-testing has to be able to tell that it missed the content.
    const Point on_the_bar{400.0f, 5.0f};  // above the centered content
    const Point root = WindowHost::design_viewport_window_to_root(
        on_the_bar, 800, 1000, 400, 300);
    REQUIRE(root.y < 0.0f);
}
