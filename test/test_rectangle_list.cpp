#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/canvas/rectangle_list.hpp>

using namespace pulp::canvas;

namespace {

void require_rect(const Rect& rect, float x, float y, float width, float height) {
    REQUIRE(rect.x == Catch::Approx(x));
    REQUIRE(rect.y == Catch::Approx(y));
    REQUIRE(rect.width == Catch::Approx(width));
    REQUIRE(rect.height == Catch::Approx(height));
}

}  // namespace

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

TEST_CASE("RectangleList subtract emits all side bands around a center cut",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({3, 2, 4, 5});

    REQUIRE(rl.size() == 4);
    require_rect(rl[0], 0, 0, 10, 2);
    require_rect(rl[1], 0, 7, 10, 3);
    require_rect(rl[2], 0, 2, 3, 5);
    require_rect(rl[3], 7, 2, 3, 5);
    REQUIRE_FALSE(rl.contains(5, 4));
    REQUIRE(rl.total_area() == Catch::Approx(80.0f));
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

TEST_CASE("Rect right bottom and empty semantics cover zero and negative extents",
          "[canvas][rect][coverage][phase3-large]") {
    Rect normal{2, 3, 4, 5};
    REQUIRE(normal.right() == Catch::Approx(6.0f));
    REQUIRE(normal.bottom() == Catch::Approx(8.0f));
    REQUIRE_FALSE(normal.empty());

    REQUIRE(Rect{0, 0, 0, 5}.empty());
    REQUIRE(Rect{0, 0, 5, 0}.empty());
    REQUIRE(Rect{0, 0, -1, 5}.empty());
    REQUIRE(Rect{0, 0, 5, -1}.empty());
}

TEST_CASE("Rect contains keeps right and bottom edges exclusive",
          "[canvas][rect][coverage][phase3-large]") {
    Rect r{-2, -3, 5, 7};

    REQUIRE(r.contains(-2.0f, -3.0f));
    REQUIRE(r.contains(2.999f, 3.999f));
    REQUIRE_FALSE(r.contains(3.0f, 0.0f));
    REQUIRE_FALSE(r.contains(0.0f, 4.0f));
    REQUIRE_FALSE(r.contains(-2.001f, 0.0f));
    REQUIRE_FALSE(r.contains(0.0f, -3.001f));
}

TEST_CASE("Rect intersection is commutative for overlapping rectangles",
          "[canvas][rect][coverage][phase3-large]") {
    Rect a{-5, 2, 12, 6};
    Rect b{0, -1, 4, 10};

    auto ab = a.intersection(b);
    auto ba = b.intersection(a);

    REQUIRE(ab == ba);
    require_rect(ab, 0, 2, 4, 6);
}

TEST_CASE("Rect intersection of contained rectangle returns contained bounds",
          "[canvas][rect][coverage][phase3-large]") {
    Rect outer{-10, -10, 30, 30};
    Rect inner{-2, 3, 5, 6};

    REQUIRE(outer.intersection(inner) == inner);
    REQUIRE(inner.intersection(outer) == inner);
}

TEST_CASE("Rect intersection treats edge and corner contact as empty",
          "[canvas][rect][coverage][phase3-large]") {
    Rect base{0, 0, 10, 10};

    REQUIRE_FALSE(base.intersects({10, 2, 3, 3}));
    REQUIRE_FALSE(base.intersects({2, 10, 3, 3}));
    REQUIRE_FALSE(base.intersects({10, 10, 3, 3}));
    REQUIRE(base.intersection({10, 2, 3, 3}).empty());
    REQUIRE(base.intersection({2, 10, 3, 3}).empty());
}

TEST_CASE("Rect enclosing union handles negative coordinates",
          "[canvas][rect][coverage][phase3-large]") {
    Rect a{-10, 5, 4, 5};
    Rect b{3, -2, 8, 3};

    auto u = a.enclosing_union(b);
    require_rect(u, -10, -2, 21, 12);
}

TEST_CASE("Rect enclosing union of two empty rectangles is empty",
          "[canvas][rect][coverage][phase3-large]") {
    Rect a{5, 6, 0, 7};
    Rect b{-1, -2, 8, 0};

    REQUIRE(a.enclosing_union(b).empty());
    REQUIRE(b.enclosing_union(a).empty());
}

TEST_CASE("RectangleList preserves insertion order for indexing and iteration",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({3, 0, 1, 1});
    rl.add({1, 0, 1, 1});
    rl.add({2, 0, 1, 1});

    REQUIRE(rl.size() == 3);
    REQUIRE(rl[0] == Rect{3, 0, 1, 1});
    REQUIRE(rl[1] == Rect{1, 0, 1, 1});
    REQUIRE(rl[2] == Rect{2, 0, 1, 1});

    float sum_x = 0.0f;
    for (const auto& rect : rl)
        sum_x += rect.x;
    REQUIRE(sum_x == Catch::Approx(6.0f));
}

TEST_CASE("RectangleList contains checks negative-coordinate rectangles",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({-10, -5, 4, 8});
    rl.add({5, -2, 3, 3});

    REQUIRE(rl.contains(-9.0f, -4.0f));
    REQUIRE(rl.contains(7.0f, 0.0f));
    REQUIRE_FALSE(rl.contains(-6.0f, -1.0f));
    REQUIRE_FALSE(rl.contains(8.0f, 1.0f));
}

TEST_CASE("RectangleList intersects ignores empty query rectangles",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    REQUIRE_FALSE(rl.intersects({5, 5, 0, 3}));
    REQUIRE_FALSE(rl.intersects({5, 5, 3, 0}));
    REQUIRE_FALSE(rl.intersects({5, 5, -1, 3}));
}

TEST_CASE("RectangleList bounding box for a single rectangle is that rectangle",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({-4, 8, 6, 2});

    REQUIRE(rl.bounding_box() == Rect{-4, 8, 6, 2});
}

TEST_CASE("RectangleList total_area intentionally double-counts overlaps",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({5, 5, 10, 10});

    REQUIRE(rl.total_area() == Catch::Approx(200.0f));
}

TEST_CASE("RectangleList clipped preserves source order and leaves source intact",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({20, 20, 10, 10});
    rl.add({5, 5, 20, 20});

    auto clipped = rl.clipped({4, 4, 10, 10});

    REQUIRE(rl.size() == 3);
    REQUIRE(clipped.size() == 2);
    require_rect(clipped[0], 4, 4, 6, 6);
    require_rect(clipped[1], 5, 5, 9, 9);
}

TEST_CASE("RectangleList clipped against disjoint bounds returns empty list",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({20, 20, 5, 5});

    REQUIRE(rl.clipped({100, 100, 10, 10}).empty());
}

TEST_CASE("RectangleList clipped handles negative clip coordinates",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({-10, -10, 8, 8});
    rl.add({-3, -3, 8, 8});

    auto clipped = rl.clipped({-5, -5, 6, 6});

    REQUIRE(clipped.size() == 2);
    require_rect(clipped[0], -5, -5, 3, 3);
    require_rect(clipped[1], -3, -3, 4, 4);
}

TEST_CASE("RectangleList subtract top strip keeps the lower remainder",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({0, 0, 10, 3});

    REQUIRE(rl.size() == 1);
    require_rect(rl[0], 0, 3, 10, 7);
}

TEST_CASE("RectangleList subtract bottom strip keeps the upper remainder",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({0, 7, 10, 3});

    REQUIRE(rl.size() == 1);
    require_rect(rl[0], 0, 0, 10, 7);
}

TEST_CASE("RectangleList subtract left strip keeps the right remainder",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({0, 0, 4, 10});

    REQUIRE(rl.size() == 1);
    require_rect(rl[0], 4, 0, 6, 10);
}

TEST_CASE("RectangleList subtract right strip keeps the left remainder",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({6, 0, 4, 10});

    REQUIRE(rl.size() == 1);
    require_rect(rl[0], 0, 0, 6, 10);
}

TEST_CASE("RectangleList subtract vertical middle split creates left and right pieces",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({4, -5, 2, 20});

    REQUIRE(rl.size() == 2);
    require_rect(rl[0], 0, 0, 4, 10);
    require_rect(rl[1], 6, 0, 4, 10);
}

TEST_CASE("RectangleList subtract horizontal middle split creates top and bottom pieces",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({-5, 4, 20, 2});

    REQUIRE(rl.size() == 2);
    require_rect(rl[0], 0, 0, 10, 4);
    require_rect(rl[1], 0, 6, 10, 4);
}

TEST_CASE("RectangleList subtract corner overlap creates two non-overlapping pieces",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({0, 0, 5, 5});

    REQUIRE(rl.size() == 2);
    require_rect(rl[0], 0, 5, 10, 5);
    require_rect(rl[1], 5, 0, 5, 5);
    REQUIRE_FALSE(rl.contains(2, 2));
    REQUIRE(rl.contains(8, 2));
    REQUIRE(rl.contains(2, 8));
}

TEST_CASE("RectangleList subtract overlap extending past left edge",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});

    rl.subtract({-5, 2, 8, 6});

    REQUIRE(rl.size() == 3);
    require_rect(rl[0], 0, 0, 10, 2);
    require_rect(rl[1], 0, 8, 10, 2);
    require_rect(rl[2], 3, 2, 7, 6);
}

TEST_CASE("RectangleList subtract applies independently to multiple rectangles",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 10, 10});
    rl.add({20, 0, 10, 10});

    rl.subtract({5, -1, 20, 12});

    REQUIRE(rl.size() == 2);
    require_rect(rl[0], 0, 0, 5, 10);
    require_rect(rl[1], 25, 0, 5, 10);
}

TEST_CASE("RectangleList repeated subtracts work on previously split pieces",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 12, 12});

    rl.subtract({4, 0, 4, 12});
    rl.subtract({0, 4, 12, 4});

    REQUIRE(rl.size() == 4);
    REQUIRE(rl.contains(2, 2));
    REQUIRE(rl.contains(10, 2));
    REQUIRE(rl.contains(2, 10));
    REQUIRE(rl.contains(10, 10));
    REQUIRE_FALSE(rl.contains(6, 6));
}

TEST_CASE("RectangleList clear allows reuse without stale bounds",
          "[canvas][rect][coverage][phase3-large]") {
    RectangleList rl;
    rl.add({0, 0, 100, 100});
    rl.clear();
    rl.add({5, 6, 7, 8});

    REQUIRE(rl.size() == 1);
    REQUIRE(rl.bounding_box() == Rect{5, 6, 7, 8});
    REQUIRE(rl.total_area() == Catch::Approx(56.0f));
}
