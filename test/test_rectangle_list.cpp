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

TEST_CASE("RectangleList subtract", "[canvas][rect]") {
    RectangleList rl;
    rl.add({0, 0, 20, 20});
    rl.subtract({5, 5, 10, 10});
    // After subtraction, the original rect is split
    REQUIRE(rl.size() > 1);
    REQUIRE_FALSE(rl.contains(10, 10));  // center was removed
    REQUIRE(rl.contains(2, 2));  // corner still exists
}

TEST_CASE("RectangleList clear", "[canvas][rect]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    REQUIRE_FALSE(rl.empty());
    rl.clear();
    REQUIRE(rl.empty());
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
