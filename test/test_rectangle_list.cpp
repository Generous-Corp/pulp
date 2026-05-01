#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/canvas/rectangle_list.hpp>

using namespace pulp::canvas;

TEST_CASE("RectangleList add and size", "[canvas][rect]") {
    RectangleList rl;
    REQUIRE(rl.empty());
    rl.add({10, 20, 30, 40});
    REQUIRE_FALSE(rl.empty());
    REQUIRE(rl.size() == 1);
}

TEST_CASE("RectangleList ignores empty rectangles", "[canvas][rect][issue-641]") {
    RectangleList rl;
    rl.add({0, 0, 0, 10});
    rl.add({0, 0, 10, 0});
    rl.add({0, 0, -1, 10});
    REQUIRE(rl.empty());
    REQUIRE(rl.size() == 0);
    REQUIRE(rl.bounding_box().empty());
    REQUIRE(rl.total_area() == Catch::Approx(0.0f));
}

TEST_CASE("RectangleList multiple rects", "[canvas][rect]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({20, 20, 10, 10});
    REQUIRE(rl.size() == 2);
}

TEST_CASE("RectangleList contains point", "[canvas][rect]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({20, 20, 10, 10});
    REQUIRE(rl.contains(5, 5));
    REQUIRE(rl.contains(25, 25));
    REQUIRE_FALSE(rl.contains(15, 15));  // gap between rects
}

TEST_CASE("RectangleList intersects uses rectangle area overlap", "[canvas][rect][issue-641]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({20, 20, 10, 10});

    REQUIRE(rl.intersects({5, 5, 1, 1}));
    REQUIRE(rl.intersects({25, 25, 2, 2}));
    REQUIRE_FALSE(rl.intersects({10, 0, 5, 5})); // edge-touching only
    REQUIRE_FALSE(rl.intersects({11, 11, 5, 5})); // gap between rects
}

TEST_CASE("RectangleList subtract", "[canvas][rect]") {
    RectangleList rl;
    rl.add({0, 0, 20, 20});
    rl.subtract({5, 5, 10, 10});
    // After subtraction, the original rect is split
    REQUIRE(rl.size() > 1);
    REQUIRE_FALSE(rl.contains(10, 10));  // center was removed
    REQUIRE(rl.contains(2, 2));  // corner still exists
}

TEST_CASE("RectangleList subtract handles no-op and full-cover cases",
          "[canvas][rect][issue-641]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({20, 20, 5, 5});
    REQUIRE(rl.size() == 1);
    REQUIRE(rl[0] == Rect{0, 0, 10, 10});

    rl.subtract({10, 0, 5, 10});
    REQUIRE(rl.size() == 1);
    REQUIRE(rl[0] == Rect{0, 0, 10, 10});

    rl.subtract({-1, -1, 12, 12});
    REQUIRE(rl.empty());
}

TEST_CASE("RectangleList empty clip and subtract are no-ops",
          "[canvas][rect][issue-641]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({});
    REQUIRE(rl.size() == 1);
    REQUIRE(rl[0] == Rect{0, 0, 10, 10});

    auto clipped_empty = rl.clipped({});
    REQUIRE(clipped_empty.empty());

    RectangleList empty;
    empty.subtract({0, 0, 1, 1});
    REQUIRE(empty.empty());
}

TEST_CASE("RectangleList clear", "[canvas][rect]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    REQUIRE_FALSE(rl.empty());
    rl.clear();
    REQUIRE(rl.empty());
}

TEST_CASE("RectangleList bounding box and area", "[canvas][rect][issue-641]") {
    RectangleList rl;
    rl.add({10, 20, 5, 6});
    rl.add({-5, 25, 3, 4});
    rl.add({20, -10, 10, 8});

    auto bounds = rl.bounding_box();
    REQUIRE(bounds.x == Catch::Approx(-5.0f));
    REQUIRE(bounds.y == Catch::Approx(-10.0f));
    REQUIRE(bounds.width == Catch::Approx(35.0f));
    REQUIRE(bounds.height == Catch::Approx(39.0f));
    REQUIRE(rl.total_area() == Catch::Approx(122.0f));
}

TEST_CASE("RectangleList clipped keeps only intersections", "[canvas][rect][issue-641]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({8, 8, 6, 6});
    rl.add({30, 30, 5, 5});

    auto clipped = rl.clipped({5, 5, 10, 10});
    REQUIRE(clipped.size() == 2);

    REQUIRE(clipped[0].x == Catch::Approx(5.0f));
    REQUIRE(clipped[0].y == Catch::Approx(5.0f));
    REQUIRE(clipped[0].width == Catch::Approx(5.0f));
    REQUIRE(clipped[0].height == Catch::Approx(5.0f));

    REQUIRE(clipped[1].x == Catch::Approx(8.0f));
    REQUIRE(clipped[1].y == Catch::Approx(8.0f));
    REQUIRE(clipped[1].width == Catch::Approx(6.0f));
    REQUIRE(clipped[1].height == Catch::Approx(6.0f));
}

TEST_CASE("Rect intersection", "[canvas][rect]") {
    Rect a{0, 0, 10, 10};
    Rect b{5, 5, 10, 10};
    REQUIRE(a.intersects(b));

    auto inter = a.intersection(b);
    REQUIRE(inter.x == Catch::Approx(5.0f));
    REQUIRE(inter.y == Catch::Approx(5.0f));
    REQUIRE(inter.width == Catch::Approx(5.0f));
    REQUIRE(inter.height == Catch::Approx(5.0f));
}

TEST_CASE("Rect no intersection", "[canvas][rect]") {
    Rect a{0, 0, 10, 10};
    Rect b{20, 20, 10, 10};
    REQUIRE_FALSE(a.intersects(b));
    auto inter = a.intersection(b);
    REQUIRE(inter.empty());
}

TEST_CASE("Rect contains point", "[canvas][rect]") {
    Rect r{10, 20, 30, 40};
    REQUIRE(r.contains(15, 25));
    REQUIRE(r.contains(10, 20));       // inclusive at top-left
    REQUIRE_FALSE(r.contains(40, 60)); // exclusive at bottom-right
    REQUIRE_FALSE(r.contains(5, 25));  // outside left
}

TEST_CASE("Rect enclosing union", "[canvas][rect]") {
    Rect a{0, 0, 10, 10};
    Rect b{5, 5, 10, 10};
    auto u = a.enclosing_union(b);
    REQUIRE(u.x == Catch::Approx(0.0f));
    REQUIRE(u.y == Catch::Approx(0.0f));
    REQUIRE(u.width == Catch::Approx(15.0f));
    REQUIRE(u.height == Catch::Approx(15.0f));
}

TEST_CASE("Rect enclosing union preserves non-empty side",
          "[canvas][rect][issue-641]") {
    Rect empty{};
    Rect non_empty{3, 4, 5, 6};

    REQUIRE(empty.enclosing_union(non_empty) == non_empty);
    REQUIRE(non_empty.enclosing_union(empty) == non_empty);
}
