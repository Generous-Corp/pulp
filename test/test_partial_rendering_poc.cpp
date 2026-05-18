// Headless POC test for the partial-rendering / damage-tracking
// path. The production host wiring lives behind macOS (Skia + Metal)
// and needs a real GPU device for end-to-end coverage; this file
// exercises the `pulp::render::DirtyTracker` contract the host wiring
// depends on so any change to invalidation, coalescing, union-bounds,
// or clear semantics is caught before touching the host code.
//
// Assertions:
//   1. First frame is dirty (no clear before any invalidate →
//      needs_full_repaint() == true).
//   2. After clear() the tracker is fully clean.
//   3. A single per-rect invalidate() yields a union exactly equal to
//      the rect, with needs_full_repaint() == false.
//   4. Two disjoint per-rect invalidations coalesce to their bounding
//      box, no flip to full repaint while under the area threshold.
//   5. invalidate_all() wins regardless of prior pushes — matches the
//      host path where the animation / FrameClock pump invalidates
//      the whole viewport.
//   6. The frame counter increments on each clear(), letting the host
//      debug print correlate "[partial-render] frame=N" with vsync
//      ticks.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/render/dirty_tracker.hpp>

using pulp::render::DirtyTracker;

TEST_CASE("partial-rendering POC: first frame is full-repaint dirty", "[render][partial-render]") {
    DirtyTracker dt;
    REQUIRE(dt.is_dirty());
    REQUIRE(dt.needs_full_repaint());
    REQUIRE(dt.frame_count() == 0u);
}

TEST_CASE("partial-rendering POC: clear() resets dirty state and advances frame counter",
          "[render][partial-render]") {
    DirtyTracker dt;
    dt.clear();
    REQUIRE_FALSE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.frame_count() == 1u);

    dt.clear();
    REQUIRE(dt.frame_count() == 2u);
}

TEST_CASE("partial-rendering POC: single request_repaint(rect) → union equals that rect, no full repaint",
          "[render][partial-render]") {
    // This is the per-View / per-widget invalidation path the follow-up partial-rendering slice
    // will plumb through. The the host wiring still calls
    // invalidate_all(), but the tracker contract must support the
    // rect path so the follow-up partial-rendering slice doesn't require a tracker API change.
    DirtyTracker dt;
    dt.clear();  // start clean

    // Viewport big enough that 30x40 is well below the full-repaint
    // threshold (default 60%).
    dt.set_viewport(800, 600);
    dt.invalidate(10, 20, 30, 40);

    REQUIRE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().size() == 1);

    auto b = dt.bounds();
    REQUIRE(b.x == Catch::Approx(10.0f));
    REQUIRE(b.y == Catch::Approx(20.0f));
    REQUIRE(b.w == Catch::Approx(30.0f));
    REQUIRE(b.h == Catch::Approx(40.0f));
}

TEST_CASE("partial-rendering POC: two disjoint invalidations coalesce to bounding box",
          "[render][partial-render]") {
    DirtyTracker dt;
    dt.clear();
    dt.set_viewport(800, 600);
    dt.invalidate(10, 10, 20, 20);   // top-left small rect
    dt.invalidate(700, 500, 50, 50); // bottom-right small rect

    // Two small disjoint rects; combined area is tiny relative to the
    // 800x600 viewport, so we must still be in partial-repaint mode.
    REQUIRE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());

    auto b = dt.bounds();
    REQUIRE(b.x == Catch::Approx(10.0f));
    REQUIRE(b.y == Catch::Approx(10.0f));
    // bounding box from (10,10) to (750,550)
    REQUIRE(b.x + b.w == Catch::Approx(750.0f));
    REQUIRE(b.y + b.h == Catch::Approx(550.0f));
}

TEST_CASE("partial-rendering POC: invalidate_all() always wins, matching the pump-driver path",
          "[render][partial-render]") {
    // Codex review correction 2: animation / FrameClock pump drivers
    // call tracker_.invalidate_all() because they mutate visual state
    // without going through request_repaint(). This test pins that
    // contract: invalidate_all() must override and clear any pending
    // partial rects so the the follow-up partial-rendering slice clip would degrade to full-screen
    // (which is safe; better than missing pixels).
    DirtyTracker dt;
    dt.clear();
    dt.set_viewport(800, 600);
    dt.invalidate(10, 20, 30, 40);
    REQUIRE_FALSE(dt.needs_full_repaint());

    dt.invalidate_all();
    REQUIRE(dt.is_dirty());
    REQUIRE(dt.needs_full_repaint());
    // invalidate_all() also clears any pending per-rect entries —
    // dirty_rects() is empty when we are in full-repaint mode.
    REQUIRE(dt.dirty_rects().empty());
}

TEST_CASE("partial-rendering POC: area threshold escalates partial → full when many rects accumulate",
          "[render][partial-render]") {
    // The host pushes one invalidate_all per repaint() today, so the
    // area-threshold escalation is mostly a the follow-up partial-rendering slice concern. Test it
    // here anyway so the contract doesn't quietly drift.
    DirtyTracker dt;
    dt.clear();
    dt.set_viewport(100, 100);  // tiny viewport, 60% = 6000 sq px
    dt.invalidate(0, 0, 80, 80); // 6400 sq px > 6000 → triggers full
    REQUIRE(dt.needs_full_repaint());
}
