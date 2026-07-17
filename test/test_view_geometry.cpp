// Tests for pulp::view::Rect / IntRect -- the layout geometry primitives.
//
// Rect is the float rectangle every widget lays itself out with: it slices rows
// and columns off a bounds rect, insets for padding, unions child extents, and
// hit-tests points. IntRect is its discrete twin, and the whole reason it exists
// is that its center truncates where the float center does not -- the half pixel
// that decides whether a 7px row lands on the grid or blurs across two rows.
//
// Every expected value below is computed by hand from the rectangle arithmetic,
// so a passing assertion means the helper is correct, not merely self-consistent.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/geometry.hpp>

using pulp::view::IntRect;
using pulp::view::Point;
using pulp::view::Rect;
using pulp::view::Size;

namespace {
void require_rect(const Rect& r, float x, float y, float w, float h) {
    REQUIRE_THAT(r.x, Catch::Matchers::WithinAbs(x, 1e-4));
    REQUIRE_THAT(r.y, Catch::Matchers::WithinAbs(y, 1e-4));
    REQUIRE_THAT(r.width, Catch::Matchers::WithinAbs(w, 1e-4));
    REQUIRE_THAT(r.height, Catch::Matchers::WithinAbs(h, 1e-4));
}
} // namespace

TEST_CASE("rect edges and center are exact halves", "[view][geometry]") {
    const Rect r{2, 4, 10, 6};
    REQUIRE_THAT(r.right(), Catch::Matchers::WithinAbs(12.0, 1e-4));
    REQUIRE_THAT(r.bottom(), Catch::Matchers::WithinAbs(10.0, 1e-4));
    REQUIRE_THAT(r.center_x(), Catch::Matchers::WithinAbs(7.0, 1e-4));
    REQUIRE_THAT(r.center_y(), Catch::Matchers::WithinAbs(7.0, 1e-4));

    // An odd extent keeps its half in float space -- no truncation.
    const Rect odd{0, 0, 7, 7};
    REQUIRE_THAT(odd.center_x(), Catch::Matchers::WithinAbs(3.5, 1e-4));

    const Point c = r.center();
    REQUIRE_THAT(c.x, Catch::Matchers::WithinAbs(7.0, 1e-4));
    const Size s = r.size();
    REQUIRE(s == Size{10, 6});
}

TEST_CASE("contains uses a half-open rule on the far edges", "[view][geometry]") {
    const Rect r{0, 0, 10, 10};
    REQUIRE(r.contains(0, 0));            // top-left included
    REQUIRE(r.contains(5, 5));
    REQUIRE(r.contains(Point{9.9f, 9.9f}));
    REQUIRE_FALSE(r.contains(10, 5));     // right edge excluded
    REQUIRE_FALSE(r.contains(5, 10));     // bottom edge excluded
    REQUIRE_FALSE(r.contains(-1, 5));
}

TEST_CASE("encloses is true only when the other rect fits entirely inside",
          "[view][geometry]") {
    const Rect outer{0, 0, 100, 100};
    REQUIRE(outer.encloses({10, 10, 50, 50}));   // wholly inside
    REQUIRE(outer.encloses(outer));              // edges may touch (<=)
    REQUIRE(outer.encloses({0, 0, 100, 100}));

    REQUIRE_FALSE(outer.encloses({-1, 10, 10, 10}));   // pokes out the left
    REQUIRE_FALSE(outer.encloses({90, 90, 20, 20}));   // right/bottom = 110 > 100
    REQUIRE_FALSE(outer.encloses({50, 95, 10, 10}));   // bottom = 105 > 100
}

TEST_CASE("inset, reduced, and expanded move every edge", "[view][geometry]") {
    const Rect r{0, 0, 10, 10};
    require_rect(r.inset(2), 2, 2, 6, 6);              // 2 off each side
    require_rect(r.inset(1, 2), 1, 2, 8, 6);           // horizontal, vertical
    require_rect(r.reduced(2), 2, 2, 6, 6);            // reduced == inset
    require_rect(r.expanded(2), -2, -2, 14, 14);       // expanded == inset(-d)

    // Insetting past the middle clamps the extent to zero, never negative.
    require_rect(r.inset(10), 10, 10, 0, 0);
}

TEST_CASE("translated and the with_* family change one thing each",
          "[view][geometry]") {
    const Rect r{1, 2, 3, 4};
    require_rect(r.translated(10, 20), 11, 22, 3, 4);
    require_rect(r.with_x(9), 9, 2, 3, 4);
    require_rect(r.with_y(9), 1, 9, 3, 4);
    require_rect(r.with_width(9), 1, 2, 9, 4);
    require_rect(r.with_height(9), 1, 2, 3, 9);
    require_rect(r.with_size(7, 8), 1, 2, 7, 8);
    require_rect(r.with_position(5, 6), 5, 6, 3, 4);
}

TEST_CASE("with_size_keeping_center holds the center still", "[view][geometry]") {
    // A 10x10 at the origin resized to 4x4 keeps center (5,5): the new origin is
    // (5 - 2, 5 - 2) = (3, 3).
    const Rect r{0, 0, 10, 10};
    const Rect shrunk = r.with_size_keeping_center(4, 4);
    require_rect(shrunk, 3, 3, 4, 4);
    REQUIRE_THAT(shrunk.center_x(), Catch::Matchers::WithinAbs(r.center_x(), 1e-4));
    REQUIRE_THAT(shrunk.center_y(), Catch::Matchers::WithinAbs(r.center_y(), 1e-4));
}

TEST_CASE("remove_from_* returns the slice and shrinks the source",
          "[view][geometry]") {
    // Taking a header off the top returns the top strip and leaves the remainder.
    Rect r{0, 0, 100, 50};
    const Rect header = r.remove_from_top(20);
    require_rect(header, 0, 0, 100, 20);
    require_rect(r, 0, 20, 100, 30);

    Rect b{0, 0, 100, 50};
    require_rect(b.remove_from_bottom(10), 0, 40, 100, 10);
    require_rect(b, 0, 0, 100, 40);

    Rect l{0, 0, 100, 50};
    require_rect(l.remove_from_left(30), 0, 0, 30, 50);
    require_rect(l, 30, 0, 70, 50);

    Rect rt{0, 0, 100, 50};
    require_rect(rt.remove_from_right(30), 70, 0, 30, 50);
    require_rect(rt, 0, 0, 70, 50);
}

TEST_CASE("remove_from_* guards negatives and overflow", "[view][geometry]") {
    // A negative amount takes nothing and changes nothing -- never a negative
    // slice.
    Rect r{0, 0, 100, 50};
    const Rect none = r.remove_from_top(-5);
    require_rect(none, 0, 0, 100, 0);
    require_rect(r, 0, 0, 100, 50);

    // Asking for more than exists takes everything and leaves an empty rect.
    Rect all{0, 0, 100, 50};
    const Rect taken = all.remove_from_top(1000);
    require_rect(taken, 0, 0, 100, 50);
    require_rect(all, 0, 50, 100, 0);
    REQUIRE(all.is_empty());
}

TEST_CASE("intersects and intersection agree on overlap", "[view][geometry]") {
    const Rect a{0, 0, 10, 10};
    REQUIRE(a.intersects({5, 5, 10, 10}));
    require_rect(a.intersection({5, 5, 10, 10}), 5, 5, 5, 5);

    // Edge-touching does not count as intersecting, and the intersection is empty.
    REQUIRE_FALSE(a.intersects({10, 0, 5, 5}));
    REQUIRE(a.intersection({10, 0, 5, 5}).is_empty());

    // Fully disjoint.
    REQUIRE_FALSE(a.intersects({20, 20, 5, 5}));
}

TEST_CASE("union_with spans both and ignores an empty operand", "[view][geometry]") {
    const Rect a{0, 0, 10, 10};
    require_rect(a.union_with({20, 20, 10, 10}), 0, 0, 30, 30);

    // An empty operand contributes nothing -- without that rule an empty
    // accumulator at the origin would drag the union back to (0,0).
    const Rect empty{0, 0, 0, 0};
    require_rect(empty.union_with(a), 0, 0, 10, 10);
    require_rect(a.union_with(empty), 0, 0, 10, 10);
}

TEST_CASE("to_nearest_int rounds the edges, not the extent", "[view][geometry]") {
    // Span [0.6, 2.4]: edges round to 1 and 2, so width is 1 -- NOT round(1.8)=2.
    const IntRect r = Rect{0.6f, 0.6f, 1.8f, 1.8f}.to_nearest_int();
    REQUIRE(r == IntRect{1, 1, 1, 1});
}

TEST_CASE("to_enclosing_int floors the origin and ceils the far edge",
          "[view][geometry]") {
    // Span [0.6, 2.4]: floor to 0, ceil to 3, so a 3x3 box fully contains it.
    const IntRect r = Rect{0.6f, 0.6f, 1.8f, 1.8f}.to_enclosing_int();
    REQUIRE(r == IntRect{0, 0, 3, 3});
}

TEST_CASE("the integer rect center truncates the half pixel away",
          "[view][geometry]") {
    // The whole reason IntRect is a separate type: 7/2 == 3, not 3.5.
    const IntRect r{0, 0, 7, 7};
    REQUIRE(r.center_x() == 3);
    REQUIRE(r.center_y() == 3);

    // Resizing about the center truncates too: (7-4)/2 == 1, so x == 1 where the
    // float rect would sit at 1.5.
    REQUIRE(r.with_size_keeping_center(4, 4) == IntRect{1, 1, 4, 4});
}

TEST_CASE("integer rect set operations use integer arithmetic", "[view][geometry]") {
    const IntRect a{0, 0, 10, 10};
    REQUIRE(a.contains(0, 0));
    REQUIRE_FALSE(a.contains(10, 0));               // right edge excluded
    REQUIRE(a.contains(IntRect{2, 2, 3, 3}));       // whole-rect containment
    REQUIRE_FALSE(a.contains(IntRect{8, 8, 5, 5})); // pokes out

    REQUIRE(a.intersects({5, 5, 10, 10}));
    REQUIRE(a.intersection({5, 5, 10, 10}) == IntRect{5, 5, 5, 5});
    REQUIRE(a.union_with({20, 20, 10, 10}) == IntRect{0, 0, 30, 30});

    // Widening to float is exact.
    REQUIRE(a.to_float() == Rect{0, 0, 10, 10});
}
