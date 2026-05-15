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

TEST_CASE("DirtyTracker full repaint keeps empty partial bounds",
          "[render][dirty][issue-644]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(1, 2, 3, 4);
    REQUIRE_FALSE(dt.dirty_rects().empty());

    dt.invalidate_all();
    REQUIRE(dt.is_dirty());
    REQUIRE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().empty());
    auto b = dt.bounds();
    REQUIRE(b.x == Catch::Approx(0.0f));
    REQUIRE(b.y == Catch::Approx(0.0f));
    REQUIRE(b.w == Catch::Approx(0.0f));
    REQUIRE(b.h == Catch::Approx(0.0f));
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

TEST_CASE("DirtyTracker ignores negative-size rects", "[render][dirty][issue-646]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(10, 10, -5, 50);
    dt.invalidate(10, 10, 50, -5);
    REQUIRE_FALSE(dt.is_dirty());
    REQUIRE(dt.dirty_rects().empty());
}

TEST_CASE("DirtyTracker empty bounds returns zero rect", "[render][dirty][issue-646]") {
    DirtyTracker dt;
    dt.clear();
    auto b = dt.bounds();
    REQUIRE(b.x == Catch::Approx(0.0f));
    REQUIRE(b.y == Catch::Approx(0.0f));
    REQUIRE(b.w == Catch::Approx(0.0f));
    REQUIRE(b.h == Catch::Approx(0.0f));
}

TEST_CASE("DirtyTracker debug overlay flag toggles independently", "[render][dirty][issue-646]") {
    DirtyTracker dt;
    REQUIRE_FALSE(dt.debug_overlay());
    dt.set_debug_overlay(true);
    REQUIRE(dt.debug_overlay());
    dt.clear();
    REQUIRE(dt.debug_overlay());
    dt.set_debug_overlay(false);
    REQUIRE_FALSE(dt.debug_overlay());
}

TEST_CASE("DirtyTracker threshold sums partial dirty regions", "[render][dirty][issue-646]") {
    DirtyTracker dt;
    dt.set_viewport(100, 100, 0.1f);
    dt.clear();

    dt.invalidate(0, 0, 20, 20);
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().size() == 1);

    dt.invalidate(50, 50, 30, 30);
    REQUIRE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().empty());
}

TEST_CASE("DirtyTracker threshold equality remains partial repaint",
          "[render][dirty][issue-644]") {
    DirtyTracker dt;
    dt.set_viewport(100, 100, 0.25f);
    dt.clear();

    dt.invalidate(0, 0, 50, 50);
    REQUIRE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().size() == 1);
}

TEST_CASE("DirtyTracker ignores invalid viewport dimensions for promotion",
          "[render][dirty][issue-644]") {
    DirtyTracker dt;
    dt.set_viewport(-100, 100, 0.01f);
    dt.clear();
    dt.invalidate(0, 0, 1000, 1000);

    REQUIRE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().size() == 1);
}

TEST_CASE("DirtyTracker does not promote to full repaint without viewport", "[render][dirty][issue-646]") {
    DirtyTracker dt;
    dt.clear();
    dt.invalidate(0, 0, 1000, 1000);
    REQUIRE(dt.is_dirty());
    REQUIRE_FALSE(dt.needs_full_repaint());
    REQUIRE(dt.dirty_rects().size() == 1);
}

TEST_CASE("DirtyTracker coalesces nearby non-overlapping rects", "[render][dirty][issue-646]") {
    DirtyTracker dt;
    dt.clear();

    for (int i = 0; i < 20; ++i) {
        dt.invalidate(static_cast<float>(i * 11), 0, 10, 10);
    }

    REQUIRE(dt.dirty_rects().size() < 20);
    auto b = dt.bounds();
    REQUIRE(b.x == Catch::Approx(0.0f));
    REQUIRE(b.y == Catch::Approx(0.0f));
    REQUIRE(b.w >= 219.0f);
    REQUIRE(b.h == Catch::Approx(10.0f));
}

TEST_CASE("Rect intersection", "[render][dirty]") {
    DirtyTracker::Rect a{0, 0, 10, 10};
    DirtyTracker::Rect b{5, 5, 10, 10};
    DirtyTracker::Rect c{20, 20, 10, 10};
    REQUIRE(a.intersects(b));
    REQUIRE_FALSE(a.intersects(c));
}

TEST_CASE("Rect merge returns bounding box", "[render][dirty][issue-646]") {
    DirtyTracker::Rect a{10, 20, 30, 40};
    DirtyTracker::Rect b{-5, 10, 20, 15};
    auto m = a.merged(b);
    REQUIRE(m.x == Catch::Approx(-5.0f));
    REQUIRE(m.y == Catch::Approx(10.0f));
    REQUIRE(m.w == Catch::Approx(45.0f));
    REQUIRE(m.h == Catch::Approx(50.0f));
}

TEST_CASE("Rect intersection excludes edge-touching rects", "[render][dirty][issue-646]") {
    DirtyTracker::Rect a{0, 0, 10, 10};
    DirtyTracker::Rect touches_right{10, 0, 10, 10};
    DirtyTracker::Rect touches_bottom{0, 10, 10, 10};
    REQUIRE_FALSE(a.intersects(touches_right));
    REQUIRE_FALSE(a.intersects(touches_bottom));
}
