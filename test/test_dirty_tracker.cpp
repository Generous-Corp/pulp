#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/render/dirty_tracker.hpp>

using namespace pulp::render;

TEST_CASE("DirtyTracker starts dirty (first frame)", "[render][dirty]") {
    DirtyTracker dt;
    REQUIRE(dt.is_dirty());
    REQUIRE(dt.needs_full_repaint());
}

TEST_CASE("DirtyTracker clear resets state", "[render][dirty]") {
    DirtyTracker dt;
    dt.clear();
    REQUIRE_FALSE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
}

TEST_CASE("DirtyTracker invalidate adds rect", "[render][dirty]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(10, 20, 50, 30);
    REQUIRE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().size() == 1);
    REQUIRE(dt.dirty_rects()[0].x == Catch::Approx(10.0f));
    REQUIRE(dt.dirty_rects()[0].w == Catch::Approx(50.0f));
}

TEST_CASE("DirtyTracker bounds merges all rects", "[render][dirty]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(10, 10, 20, 20);
    dt.invalidate(50, 50, 20, 20);
    auto b = dt.bounds();
    REQUIRE(b.x == Catch::Approx(10.0f));
    REQUIRE(b.y == Catch::Approx(10.0f));
    REQUIRE(b.w == Catch::Approx(60.0f));
    REQUIRE(b.h == Catch::Approx(60.0f));
}

TEST_CASE("DirtyTracker coalesces overlapping rects", "[render][dirty]") {
    DirtyTracker dt;
    dt.clear();
    // Add many overlapping rects
    for (int i = 0; i < 20; ++i) {
        dt.invalidate(static_cast<float>(i * 2), 0, 10, 10);
    }
    // Should have been coalesced to fewer rects
    REQUIRE(dt.dirty_rects().size() <= 16);
}

TEST_CASE("DirtyTracker full repaint on large dirty area", "[render][dirty]") {
    DirtyTracker dt;
    dt.set_viewport(100, 100, 0.5f);
    dt.clear();
    // Invalidate > 50% of viewport
    dt.invalidate(0, 0, 80, 80); // 64% of 100x100
    REQUIRE(dt.needs_full_repaint());
}

TEST_CASE("DirtyTracker invalidate_all forces full repaint", "[render][dirty]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(10, 10, 20, 20);
    REQUIRE_FALSE(dt.needs_full_repaint());
    dt.invalidate_all();
    REQUIRE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().empty());
}

TEST_CASE("DirtyTracker frame counter increments", "[render][dirty]") {
    DirtyTracker dt;
    REQUIRE(dt.frame_count() == 0);
    dt.clear();
    REQUIRE(dt.frame_count() == 1);
    dt.clear();
    REQUIRE(dt.frame_count() == 2);
}

TEST_CASE("DirtyTracker ignores zero-size rects", "[render][dirty]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(10, 10, 0, 50);
    dt.invalidate(10, 10, 50, 0);
    REQUIRE_FALSE(dt.is_dirty());
}

TEST_CASE("Rect intersection", "[render][dirty]") {
    DirtyTracker::Rect a{0, 0, 10, 10};
    DirtyTracker::Rect b{5, 5, 10, 10};
    DirtyTracker::Rect c{20, 20, 10, 10};
    REQUIRE(a.intersects(b));
    REQUIRE_FALSE(a.intersects(c));
}
