// Tests for pulp::canvas::Path — the retained vector value type.
//
// Path is the type every other vector surface is expressed in terms of, so its
// edges matter more than most. The cases below pin the ones that are easy to get
// subtly, silently wrong:
//
//   * Tight bounds vs control bounds. A cubic's control points usually fall
//     OUTSIDE the curve they steer, so the two answers differ for almost every
//     real curve. Fitting a shape by its control hull leaves it mysteriously
//     inset from the box it was asked to fill.
//   * Fill rule. nonzero and evenodd disagree about exactly the interesting
//     case — a region enclosed twice — and that disagreement is the whole reason
//     the rule is a parameter and not a constant.
//   * scale_to_fit centring, and its degenerate guard. Scaling a zero-width path
//     to a non-zero width is a division by zero; the guard is what stands between
//     a vertical line and a path whose coordinates are all NaN.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/canvas/path.hpp>

#include <cmath>
#include <vector>

using pulp::canvas::AffineTransform;
using pulp::canvas::FillRule;
using pulp::canvas::Path;
using pulp::canvas::Point2D;

namespace {

Path unit_square() {
    Path p;
    p.move_to(0, 0).line_to(10, 0).line_to(10, 10).line_to(0, 10).close();
    return p;
}

/// Two concentric squares wound the SAME direction. This is the shape the two
/// fill rules disagree about: the inner region is enclosed twice.
Path nested_squares_same_winding() {
    Path p;
    p.move_to(0, 0).line_to(100, 0).line_to(100, 100).line_to(0, 100).close();
    p.move_to(25, 25).line_to(75, 25).line_to(75, 75).line_to(25, 75).close();
    return p;
}

std::vector<Path::Verb> verbs_of(const Path& p) {
    std::vector<Path::Verb> out;
    for (Path::Element el : p)
        out.push_back(el.verb);
    return out;
}

} // namespace

TEST_CASE("a default-constructed path is empty", "[canvas][path]") {
    Path p;
    REQUIRE(p.is_empty());
    REQUIRE(p.verb_count() == 0);
    REQUIRE_FALSE(p.current_point().has_value());
    REQUIRE(p.begin() == p.end());
}

TEST_CASE("a path records the verbs it was built from, in order", "[canvas][path]") {
    Path p;
    p.move_to(0, 0).line_to(10, 0).quad_to(15, 5, 10, 10)
     .cubic_to(8, 12, 2, 12, 0, 10).close();

    REQUIRE(verbs_of(p) == std::vector<Path::Verb>{
        Path::Verb::move, Path::Verb::line, Path::Verb::quad,
        Path::Verb::cubic, Path::Verb::close});

    // Each verb consumes exactly the points it declares.
    for (Path::Element el : p)
        REQUIRE(el.count == Path::points_for(el.verb));
}

TEST_CASE("path equality is geometric and order-sensitive", "[canvas][path]") {
    REQUIRE(unit_square() == unit_square());

    // Same enclosed area, opposite winding: NOT equal. Equality is "same verbs,
    // same points, same order" -- it is not an area comparison.
    Path reversed;
    reversed.move_to(0, 0).line_to(0, 10).line_to(10, 10).line_to(10, 0).close();
    REQUIRE(reversed != unit_square());
}

TEST_CASE("a copied path is an independent value", "[canvas][path]") {
    // Copy is O(1) copy-on-write, which is exactly the situation where a missing
    // detach silently aliases two "independent" values together.
    Path a = unit_square();
    Path b = a;
    REQUIRE(a == b);

    b.line_to(50, 50);        // mutate the copy

    REQUIRE(b.verb_count() == a.verb_count() + 1);
    REQUIRE(a == unit_square());   // the original is untouched
    REQUIRE(a != b);
}

TEST_CASE("clearing a path empties it without disturbing its copies",
          "[canvas][path]") {
    Path a = unit_square();
    Path b = a;

    a.clear();

    REQUIRE(a.is_empty());
    REQUIRE(b == unit_square());
}

TEST_CASE("current_point is the last point placed", "[canvas][path]") {
    Path p;
    p.move_to(3, 4);
    REQUIRE(p.current_point().has_value());
    REQUIRE_THAT(p.current_point()->x, Catch::Matchers::WithinAbs(3.0, 1e-5));
    REQUIRE_THAT(p.current_point()->y, Catch::Matchers::WithinAbs(4.0, 1e-5));

    p.line_to(9, 1);
    REQUIRE_THAT(p.current_point()->x, Catch::Matchers::WithinAbs(9.0, 1e-5));
    REQUIRE_THAT(p.current_point()->y, Catch::Matchers::WithinAbs(1.0, 1e-5));
}

TEST_CASE("bounds of a rectilinear path are exactly its corners", "[canvas][path]") {
    const auto b = unit_square().bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(10.0, 1e-4));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(10.0, 1e-4));
}

TEST_CASE("tight bounds are tighter than the control hull for a real curve",
          "[canvas][path]") {
    // A cubic from (0,0) to (100,0) with both controls yanked to y = 100. The
    // curve never gets anywhere near y = 100 -- it peaks at 3/4 of the control
    // height -- so the control hull over-reports the extent by 25 units.
    //
    // This is the difference that makes scale_to_fit correct: fitting by the hull
    // would leave the shape inset from the box it was told to fill.
    Path p;
    p.move_to(0, 0).cubic_to(0, 100, 100, 100, 100, 0);

    const auto tight = p.bounds();
    const auto hull  = p.control_bounds();

    REQUIRE_THAT(hull.height, Catch::Matchers::WithinAbs(100.0, 1e-3));
    REQUIRE_THAT(tight.height, Catch::Matchers::WithinAbs(75.0, 1e-2));

    // The invariant that must hold for EVERY path: tight never exceeds the hull.
    REQUIRE(tight.height <= hull.height + 1e-4f);
    REQUIRE(tight.width  <= hull.width  + 1e-4f);
}

TEST_CASE("the two fill rules disagree about a twice-enclosed region",
          "[canvas][path]") {
    // The inner square is wound the same way as the outer, so it is enclosed
    // twice. nonzero counts 2 (inside); evenodd counts 2 (outside). If both rules
    // agree here, the rule is being ignored.
    const Path p = nested_squares_same_winding();
    const Point2D centre{50, 50};

    REQUIRE(p.contains(centre, FillRule::nonzero));
    REQUIRE_FALSE(p.contains(centre, FillRule::evenodd));

    // In the band between the two squares, both rules agree: inside.
    const Point2D band{10, 50};
    REQUIRE(p.contains(band, FillRule::nonzero));
    REQUIRE(p.contains(band, FillRule::evenodd));

    // And well outside, both agree: outside.
    const Point2D away{500, 500};
    REQUIRE_FALSE(p.contains(away, FillRule::nonzero));
    REQUIRE_FALSE(p.contains(away, FillRule::evenodd));
}

TEST_CASE("an open subpath hit-tests as though it were closed", "[canvas][path]") {
    // It fills as if closed, so it must hit-test as if closed, or a click lands
    // somewhere the user can plainly see paint.
    Path open;
    open.move_to(0, 0).line_to(10, 0).line_to(10, 10).line_to(0, 10);  // no close()

    REQUIRE(open.contains(5, 5));
    REQUIRE_FALSE(open.contains(50, 50));
}

TEST_CASE("a transformed path leaves the original alone", "[canvas][path]") {
    const Path original = unit_square();
    const Path moved = original.transformed(AffineTransform::translation(100, 0));

    REQUIRE(original == unit_square());
    REQUIRE_THAT(moved.bounds().x, Catch::Matchers::WithinAbs(100.0, 1e-4));
    REQUIRE_THAT(original.bounds().x, Catch::Matchers::WithinAbs(0.0, 1e-4));
}

TEST_CASE("apply_transform mutates in place", "[canvas][path]") {
    Path p = unit_square();
    p.apply_transform(AffineTransform::scaling(2.0f, 2.0f));

    const auto b = p.bounds();
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(20.0, 1e-4));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(20.0, 1e-4));
}

TEST_CASE("scale_to_fit fills the target rect when proportions are free",
          "[canvas][path]") {
    Path p = unit_square();                       // 10x10
    p.scale_to_fit(0, 0, 200, 50, /*preserve_proportions=*/false);

    const auto b = p.bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(200.0, 1e-3));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(50.0, 1e-3));
}

TEST_CASE("scale_to_fit centres the slack on the axis that did not bind",
          "[canvas][path]") {
    // THE case the header documents. A 10x10 square fitted into a 100x200 box
    // with proportions preserved scales by min(10, 20) = 10x, giving 100x100 --
    // exactly half the target height. The 100 units of leftover vertical space
    // must be SPLIT (50 above, 50 below), not dumped at one edge.
    Path p = unit_square();
    p.scale_to_fit(0, 0, 100, 200, /*preserve_proportions=*/true);

    const auto b = p.bounds();
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(100.0, 1e-3));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(100.0, 1e-3));

    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(0.0, 1e-3));    // bound axis: flush
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(50.0, 1e-3));   // free axis: centred
}

TEST_CASE("scale_to_fit honours a non-zero target origin", "[canvas][path]") {
    Path p = unit_square();
    p.scale_to_fit(30, 40, 100, 100, /*preserve_proportions=*/true);

    const auto b = p.bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(30.0, 1e-3));
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(40.0, 1e-3));
}

TEST_CASE("scale_to_fit on a degenerate path is a no-op, not a NaN factory",
          "[canvas][path]") {
    // A vertical line has zero width. Scaling it to a non-zero width is a
    // division by zero, and the "result" is a path whose every coordinate is NaN
    // -- which renders as nothing at all, from anywhere, forever.
    Path line;
    line.move_to(5, 0).line_to(5, 10);

    const Path before = line;
    line.scale_to_fit(0, 0, 100, 100, /*preserve_proportions=*/false);

    REQUIRE(line == before);      // deliberately unchanged

    for (Path::Element el : line)
        for (int i = 0; i < el.count; ++i) {
            REQUIRE_FALSE(std::isnan(el.points[i].x));
            REQUIRE_FALSE(std::isnan(el.points[i].y));
        }
}

TEST_CASE("scale_to_fit into a zero-sized rect is a no-op", "[canvas][path]") {
    Path p = unit_square();
    const Path before = p;

    p.scale_to_fit(0, 0, 0, 0, /*preserve_proportions=*/true);

    REQUIRE(p == before);
}

TEST_CASE("add_path appends another path under a transform", "[canvas][path]") {
    Path p = unit_square();                       // occupies 0..10
    const size_t verbs_before = p.verb_count();

    p.add_path(unit_square(), AffineTransform::translation(100, 0));

    REQUIRE(p.verb_count() == verbs_before * 2);

    // The union spans both copies.
    const auto b = p.bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(0.0, 1e-4));
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(110.0, 1e-4));
}

TEST_CASE("a path round-trips through replay", "[canvas][path]") {
    // Replay is how a path reaches every backend. If it cannot reproduce the path
    // it was given, no backend can draw it faithfully.
    struct Rebuilder {
        Path out;
        void move_to(float x, float y) { out.move_to(x, y); }
        void line_to(float x, float y) { out.line_to(x, y); }
        void quad_to(float cx, float cy, float x, float y) { out.quad_to(cx, cy, x, y); }
        void cubic_to(float a, float b, float c, float d, float x, float y) {
            out.cubic_to(a, b, c, d, x, y);
        }
        void close_path() { out.close(); }
    };

    Path source;
    source.move_to(0, 0).line_to(10, 0).quad_to(15, 5, 10, 10)
          .cubic_to(8, 12, 2, 12, 0, 10).close();

    Rebuilder sink;
    source.replay(sink);

    REQUIRE(sink.out == source);
}

TEST_CASE("a circle measures its diameter, and its hull happens to agree",
          "[canvas][path]") {
    // Worth pinning precisely BECAUSE it is the exception. A circle is four
    // cubics, and for each one the control points sit at exactly ±r on the axis
    // the segment spans -- so the control hull and the tight box coincide.
    //
    // That makes the circle useless as a hull-vs-tight test (the case above does
    // that job) but a good check that bounds() reports a diameter and not, say, a
    // radius or a control-point extent that only looks right on a circle.
    Path c;
    c.add_circle(50, 50, 25);

    const auto b = c.bounds();
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(50.0, 0.05));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(50.0, 0.05));
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(25.0, 0.05));
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(25.0, 0.05));

    const auto hull = c.control_bounds();
    REQUIRE_THAT(hull.width, Catch::Matchers::WithinAbs(b.width, 0.05));

    REQUIRE(c.contains(50, 50));            // centre
    REQUIRE_FALSE(c.contains(50, 10));      // outside the top edge
    REQUIRE_FALSE(c.contains(30, 30));      // in the corner the circle misses
}
