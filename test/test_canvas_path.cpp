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
    const Point2D center{50, 50};

    REQUIRE(p.contains(center, FillRule::nonzero));
    REQUIRE_FALSE(p.contains(center, FillRule::evenodd));

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

TEST_CASE("scale_to_fit centers the slack on the axis that did not bind",
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
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(50.0, 1e-3));   // free axis: centerd
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

    REQUIRE(c.contains(50, 50));            // center
    REQUIRE_FALSE(c.contains(50, 10));      // outside the top edge
    REQUIRE_FALSE(c.contains(30, 30));      // in the corner the circle misses
}

// ── AffineTransform ─────────────────────────────────────────────────────────
//
// Every expected value below is derived from the transform's definition
// ([a c e; b d f] with x' = a*x + c*y + e, y' = b*x + d*y + f), never from the
// implementation's own output, so "it matches" means "it is correct".

namespace {
constexpr float kPi = 3.14159265358979323846f;

void require_point(Point2D p, float x, float y, float tol = 1e-4f) {
    REQUIRE_THAT(p.x, Catch::Matchers::WithinAbs(x, tol));
    REQUIRE_THAT(p.y, Catch::Matchers::WithinAbs(y, tol));
}
} // namespace

TEST_CASE("identity leaves a point untouched and reports itself", "[canvas][affine]") {
    const auto id = AffineTransform::identity();
    REQUIRE(id.is_identity());
    require_point(id.transform_point({3, 7}), 3, 7);
    REQUIRE_THAT(id.determinant(), Catch::Matchers::WithinAbs(1.0, 1e-6));
    REQUIRE_FALSE(id.is_singular());
    // A default-constructed transform IS the identity.
    REQUIRE(AffineTransform{} == id);
}

TEST_CASE("translation shifts a point by the offset", "[canvas][affine]") {
    const auto t = AffineTransform::translation(10, -5);
    require_point(t.transform_point({0, 0}), 10, -5);
    require_point(t.transform_point({3, 4}), 13, -1);
    REQUIRE_FALSE(t.is_identity());
    REQUIRE_THAT(t.determinant(), Catch::Matchers::WithinAbs(1.0, 1e-6));  // area-preserving
    // translation(0, 0) is indistinguishable from identity.
    REQUIRE(AffineTransform::translation(0, 0).is_identity());
}

TEST_CASE("scaling multiplies each axis and its determinant is the area factor",
          "[canvas][affine]") {
    const auto s = AffineTransform::scaling(2, 3);
    require_point(s.transform_point({4, 5}), 8, 15);
    REQUIRE_THAT(s.determinant(), Catch::Matchers::WithinAbs(6.0, 1e-5));  // 2 * 3
}

TEST_CASE("scaling about a pivot keeps the pivot fixed", "[canvas][affine]") {
    // The pivot maps to itself; a point one unit right of it maps sx units right.
    const auto s = AffineTransform::scaling(2, 3, 10, 20);
    require_point(s.transform_point({10, 20}), 10, 20);   // pivot is a fixed point
    require_point(s.transform_point({12, 20}), 14, 20);   // 2 * (12 - 10) + 10
    require_point(s.transform_point({10, 22}), 10, 26);   // 3 * (22 - 20) + 20
}

TEST_CASE("rotation by 90 degrees sends +x to +y in Pulp's y-down space",
          "[canvas][affine]") {
    // Matrix {cos, sin, -sin, cos, 0, 0}; at 90 deg cos=0, sin=1, so
    // (1,0) -> (0,1) and (0,1) -> (-1,0).
    const auto r = AffineTransform::rotation(kPi * 0.5f);
    require_point(r.transform_point({1, 0}), 0, 1);
    require_point(r.transform_point({0, 1}), -1, 0);
    REQUIRE_THAT(r.determinant(), Catch::Matchers::WithinAbs(1.0, 1e-5));  // rotations preserve area
}

TEST_CASE("rotation about a pivot keeps the pivot fixed", "[canvas][affine]") {
    const auto r = AffineTransform::rotation(kPi * 0.5f, 5, 5);
    require_point(r.transform_point({5, 5}), 5, 5);   // pivot fixed
    require_point(r.transform_point({6, 5}), 5, 6);   // (1,0) about pivot -> (0,1)
}

TEST_CASE("shear slants one axis by the other", "[canvas][affine]") {
    // shear(shx, shy) = {1, shy, shx, 1, 0, 0}; x' = x + shx*y, y' = shy*x + y.
    const auto h = AffineTransform::shear(2, 0);
    require_point(h.transform_point({0, 1}), 2, 1);   // x picks up 2 * y
    require_point(h.transform_point({1, 0}), 1, 0);   // y unchanged (shy = 0)
    REQUIRE_THAT(h.determinant(), Catch::Matchers::WithinAbs(1.0, 1e-5));  // 1 - shx*shy = 1
}

TEST_CASE("both point-transform spellings agree", "[canvas][affine]") {
    const auto t = AffineTransform::scaling(2, 3).translated(1, 1);
    const Point2D pv = t.transform_point({4, 5});
    float x = 4, y = 5;
    t.transform_point(x, y);                    // the in-place overload
    REQUIRE_THAT(x, Catch::Matchers::WithinAbs(pv.x, 1e-5));
    REQUIRE_THAT(y, Catch::Matchers::WithinAbs(pv.y, 1e-5));
}

TEST_CASE("followed_by applies this transform first, then the argument",
          "[canvas][affine]") {
    // scale by 2, THEN translate by (10, 0): (1,1) -> (2,2) -> (12,2).
    const auto composed =
        AffineTransform::scaling(2, 2).followed_by(AffineTransform::translation(10, 0));
    require_point(composed.transform_point({1, 1}), 12, 2);

    // The opposite order gives a different result: translate first (1,1)->(11,1),
    // then scale -> (22,2). Order matters, and followed_by reads left-to-right.
    const auto other =
        AffineTransform::translation(10, 0).followed_by(AffineTransform::scaling(2, 2));
    require_point(other.transform_point({1, 1}), 22, 2);
}

TEST_CASE("operator* is followed_by with the operands reversed", "[canvas][affine]") {
    // (m1 * m2) applies m2 first, then m1 -- the matrix-product convention.
    const auto m1 = AffineTransform::translation(10, 0);
    const auto m2 = AffineTransform::scaling(2, 2);
    require_point((m1 * m2).transform_point({1, 1}), 12, 2);   // scale then translate
    REQUIRE((m1 * m2) == m2.followed_by(m1));
}

TEST_CASE("the fluent builders compose in call order", "[canvas][affine]") {
    // translate then scale: (1,1) -> (11,1) -> (22,2).
    const auto t = AffineTransform::identity().translated(10, 0).scaled(2, 2);
    require_point(t.transform_point({1, 1}), 22, 2);

    // scaled/sheared/rotated are each followed_by(factory); pin sheared here.
    const auto sh = AffineTransform::identity().sheared(2, 0);
    require_point(sh.transform_point({0, 1}), 2, 1);
}

TEST_CASE("transform_rect is the AABB of the four transformed corners",
          "[canvas][affine]") {
    using pulp::canvas::Rect2D;
    // A pure scale just scales the rect.
    const Rect2D scaled = AffineTransform::scaling(2, 3).transform_rect({1, 1, 4, 2});
    REQUIRE_THAT(scaled.x, Catch::Matchers::WithinAbs(2.0, 1e-4));       // 1 * 2
    REQUIRE_THAT(scaled.y, Catch::Matchers::WithinAbs(3.0, 1e-4));       // 1 * 3
    REQUIRE_THAT(scaled.width, Catch::Matchers::WithinAbs(8.0, 1e-4));   // 4 * 2
    REQUIRE_THAT(scaled.height, Catch::Matchers::WithinAbs(6.0, 1e-4));  // 2 * 3

    // A 45-degree rotation of a 10x10 box about the origin: the diagonal (10*sqrt2)
    // becomes the axis-aligned width AND height, and the box straddles x = 0.
    const Rect2D rot = AffineTransform::rotation(kPi * 0.25f).transform_rect({0, 0, 10, 10});
    const float diag = 10.0f * std::sqrt(2.0f);
    REQUIRE_THAT(rot.width, Catch::Matchers::WithinAbs(diag, 1e-3));
    REQUIRE_THAT(rot.height, Catch::Matchers::WithinAbs(diag, 1e-3));
    REQUIRE_THAT(rot.x, Catch::Matchers::WithinAbs(-diag * 0.5, 1e-3));
    REQUIRE_THAT(rot.y, Catch::Matchers::WithinAbs(0.0, 1e-3));
}

TEST_CASE("is_singular flags the transforms that collapse area", "[canvas][affine]") {
    REQUIRE(AffineTransform::scaling(0, 1).is_singular());   // determinant 0
    REQUIRE(AffineTransform::scaling(1, 0).is_singular());
    REQUIRE_FALSE(AffineTransform::identity().is_singular());
    REQUIRE_FALSE(AffineTransform::scaling(2, 3).is_singular());
}

TEST_CASE("inverse of a factory transform is its analytic opposite", "[canvas][affine]") {
    const auto tinv = AffineTransform::translation(10, 5).inverted();
    REQUIRE(tinv.has_value());
    REQUIRE(*tinv == AffineTransform::translation(-10, -5));

    const auto sinv = AffineTransform::scaling(2, 4).inverted();
    REQUIRE(sinv.has_value());
    require_point(sinv->transform_point({8, 8}), 4, 2);   // 1/2 and 1/4

    // A singular transform has no inverse.
    REQUIRE_FALSE(AffineTransform::scaling(0, 1).inverted().has_value());
}

TEST_CASE("a transform composed with its inverse is the identity", "[canvas][affine]") {
    const auto t = AffineTransform::translation(7, -3)
                       .scaled(2, 0.5f)
                       .rotated(0.6f);
    const auto inv = t.inverted();
    REQUIRE(inv.has_value());
    // Round-trip an arbitrary point back to itself.
    require_point(inv->transform_point(t.transform_point({12, -4})), 12, -4, 1e-3f);
}

// ── Path: arcs, pie, polygon, rounded rects, ellipse ────────────────────────

TEST_CASE("add_arc is an open subpath from the start angle to the end angle",
          "[canvas][path]") {
    // A quarter arc of radius 10 centered at the origin, sweeping 0 -> 90 deg.
    // Start point: (10*cos0, 10*sin0) = (10, 0). End point: (10*cos90, 10*sin90)
    // = (0, 10). The arc is NOT closed (no move back to any center).
    Path p;
    p.add_arc(0, 0, 10, 10, 0.0f, kPi * 0.5f);

    REQUIRE_FALSE(p.is_empty());
    REQUIRE(verbs_of(p).front() == Path::Verb::move);
    REQUIRE(verbs_of(p).back() != Path::Verb::close);   // open

    require_point(*p.current_point(), 0, 10, 0.02f);

    // The arc bulges out to x=10 (at 0 deg) and y=10 (at 90 deg), touching the
    // axes at the far end, so its box is exactly the quarter's unit-radius box.
    const auto b = p.bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(0.0, 0.02));
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(0.0, 0.02));
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(10.0, 0.02));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(10.0, 0.02));
}

TEST_CASE("add_pie is a closed wedge from the center", "[canvas][path]") {
    // center -> arc-start -> arc -> close. A quarter pie of radius 10.
    Path p;
    p.add_pie(0, 0, 10, 10, 0.0f, kPi * 0.5f);

    const auto verbs = verbs_of(p);
    REQUIRE(verbs.front() == Path::Verb::move);   // to the center
    REQUIRE(verbs[1] == Path::Verb::line);        // out to the arc start
    REQUIRE(verbs.back() == Path::Verb::close);

    // A point just inside the wedge near the center fills; a point past the
    // radius, and a point on the wrong side of the center, do not.
    REQUIRE(p.contains(2, 2));            // radius ~2.8, angle 45 deg: inside
    REQUIRE_FALSE(p.contains(9, 9));      // radius ~12.7 > 10: outside
    REQUIRE_FALSE(p.contains(-5, -5));    // opposite quadrant: outside the wedge
}

TEST_CASE("add_polygon closes a ring through its points", "[canvas][path]") {
    const Point2D tri[] = {{0, 0}, {10, 0}, {5, 10}};
    Path p;
    p.add_polygon(tri, 3);

    REQUIRE(verbs_of(p) == std::vector<Path::Verb>{
        Path::Verb::move, Path::Verb::line, Path::Verb::line, Path::Verb::close});

    // At y = 3 the triangle spans x in (1.5, 8.5): the left edge (0,0)->(5,10)
    // is at x = 1.5, the right edge (10,0)->(5,10) at x = 8.5.
    REQUIRE(p.contains(5, 3));
    REQUIRE_FALSE(p.contains(1, 3));

    // Fewer than two points, or a null pointer, draws nothing.
    Path degenerate;
    const Point2D one[] = {{1, 1}};
    degenerate.add_polygon(one, 1);
    REQUIRE(degenerate.is_empty());
    degenerate.add_polygon(nullptr, 5);
    REQUIRE(degenerate.is_empty());
}

TEST_CASE("add_rounded_rect fills its box but rounds its corners away",
          "[canvas][path]") {
    // The straight edges still reach the full extent, so the bounds are the full
    // rect; but the corner (0,0) is outside the r=10 fillet whose center is at
    // (10,10): distance sqrt(200) ~ 14.1 > 10.
    Path p;
    p.add_rounded_rect(0, 0, 100, 50, 10);

    const auto b = p.bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(b.y, Catch::Matchers::WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(100.0, 1e-3));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(50.0, 1e-3));

    REQUIRE(p.contains(50, 25));          // center: inside
    REQUIRE(p.contains(2, 25));           // mid-height on the left edge: inside
    REQUIRE_FALSE(p.contains(0.5f, 0.5f)); // rounded-away corner: outside
}

TEST_CASE("a uniform radius larger than half the side clamps to an inscribed circle",
          "[canvas][path]") {
    // The CSS overlap rule scales every radius down until the corners just touch.
    // On a 20x20 box a radius of 1000 clamps to 10 -- an inscribed circle of
    // radius 10 centered at (10,10), which never reaches the square's corners.
    Path p;
    p.add_rounded_rect(0, 0, 20, 20, 1000);

    const auto b = p.bounds();
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(20.0, 1e-2));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(20.0, 1e-2));

    REQUIRE(p.contains(10, 10));          // center of the inscribed circle
    REQUIRE(p.contains(10, 1));           // top-middle: radius 9 < 10, inside
    REQUIRE_FALSE(p.contains(0, 0));      // corner: radius sqrt(200) > 10, outside
}

TEST_CASE("per-corner rounded rect rounds only the specified corner",
          "[canvas][path]") {
    // Only the top-left is rounded (r=10); the other three stay square. So the
    // bottom-right corner (60,40) is inside, but the top-left corner (0,0) is
    // rounded away (outside the r=10 fillet centered at (10,10)).
    Path p;
    p.add_rounded_rect(0, 0, 60, 40, /*tl=*/10, /*tr=*/0, /*br=*/0, /*bl=*/0);

    REQUIRE(p.contains(59, 39));          // square bottom-right corner: inside
    REQUIRE(p.contains(1, 39));           // square bottom-left corner: inside
    REQUIRE_FALSE(p.contains(0.5f, 0.5f)); // rounded top-left corner: outside
}

TEST_CASE("add_ellipse fills an axis-aligned ellipse", "[canvas][path]") {
    // rx=20, ry=10 centered at origin. Inside iff (x/20)^2 + (y/10)^2 <= 1.
    Path p;
    p.add_ellipse(0, 0, 20, 10);

    const auto b = p.bounds();
    REQUIRE_THAT(b.x, Catch::Matchers::WithinAbs(-20.0, 0.05));
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(40.0, 0.05));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(20.0, 0.05));

    REQUIRE(p.contains(0, 0));            // center
    REQUIRE(p.contains(19, 0));           // (19/20)^2 = 0.9 < 1: inside
    REQUIRE_FALSE(p.contains(15, 8));     // 0.5625 + 0.64 = 1.2 > 1: outside
}

TEST_CASE("arc_to lays a fillet between two tangent lines", "[canvas][path]") {
    // Canvas2D arcTo: from (0,0), corner at (10,0), on toward (10,10), radius 5.
    // The interior angle is 90 deg, so each tangent point sits radius/tan(45) = 5
    // from the corner: the first at (5,0) on the incoming line, the last at
    // (10,5) on the outgoing line -- which is where the path ends.
    Path p;
    p.move_to(0, 0).arc_to(10, 0, 10, 10, 5);

    REQUIRE(verbs_of(p).front() == Path::Verb::move);
    require_point(*p.current_point(), 10, 5, 0.02f);
}

TEST_CASE("arc_to degrades to a line when the geometry is degenerate",
          "[canvas][path]") {
    // Collinear points cannot define a tangent arc: Canvas2D says draw a line to
    // the first control point instead.
    Path collinear;
    collinear.move_to(0, 0).arc_to(10, 0, 20, 0, 5);
    require_point(*collinear.current_point(), 10, 0);

    // With no current point, arcTo is just a moveTo to the first control point.
    Path fresh;
    fresh.arc_to(3, 4, 9, 9, 5);
    REQUIRE(verbs_of(fresh) == std::vector<Path::Verb>{Path::Verb::move});
    require_point(*fresh.current_point(), 3, 4);
}

// ── Path: iterator, flatten, SVG round-trip, self-append ────────────────────

TEST_CASE("the iterator exposes each verb's points at the right offset",
          "[canvas][path]") {
    Path p;
    p.move_to(1, 2).line_to(3, 4).quad_to(5, 6, 7, 8)
     .cubic_to(9, 10, 11, 12, 13, 14).close();

    auto it = p.begin();

    Path::Element move = *it;
    REQUIRE(move.verb == Path::Verb::move);
    REQUIRE(move.count == 1);
    require_point(move.points[0], 1, 2);

    ++it;
    Path::Element line = *it;
    REQUIRE(line.verb == Path::Verb::line);
    require_point(line.points[0], 3, 4);

    ++it;
    Path::Element quad = *it;
    REQUIRE(quad.verb == Path::Verb::quad);
    REQUIRE(quad.count == 2);
    require_point(quad.points[0], 5, 6);
    require_point(quad.points[1], 7, 8);

    ++it;
    Path::Element cubic = *it;
    REQUIRE(cubic.verb == Path::Verb::cubic);
    REQUIRE(cubic.count == 3);
    require_point(cubic.points[0], 9, 10);
    require_point(cubic.points[1], 11, 12);
    require_point(cubic.points[2], 13, 14);

    ++it;
    Path::Element close = *it;
    REQUIRE(close.verb == Path::Verb::close);
    REQUIRE(close.count == 0);
    REQUIRE(close.points == nullptr);

    ++it;
    REQUIRE(it == p.end());
}

TEST_CASE("replay drives a sink through every verb kind", "[canvas][path]") {
    // A sink that records which builder call it received, in order -- so this
    // pins that replay dispatches move/line/quad/cubic/close correctly, not just
    // that a rebuilt path compares equal.
    struct TraceSink {
        std::vector<char> calls;
        void move_to(float, float) { calls.push_back('M'); }
        void line_to(float, float) { calls.push_back('L'); }
        void quad_to(float, float, float, float) { calls.push_back('Q'); }
        void cubic_to(float, float, float, float, float, float) { calls.push_back('C'); }
        void close_path() { calls.push_back('Z'); }
    };

    Path p;
    p.move_to(0, 0).line_to(10, 0).quad_to(15, 5, 10, 10)
     .cubic_to(8, 12, 2, 12, 0, 10).close();

    TraceSink sink;
    p.replay(sink);
    REQUIRE(sink.calls == std::vector<char>{'M', 'L', 'Q', 'C', 'Z'});
}

TEST_CASE("tight bounds solve a quadratic's interior extremum", "[canvas][path]") {
    // A quad from (0,0) via control (10,20) to (20,0). Its y peaks at t=0.5 at
    // 40*t*(1-t) = 10, so the tight box is only 10 tall while the control hull is
    // 20 tall. This exercises the quadratic extrema path, distinct from the cubic.
    Path p;
    p.move_to(0, 0).quad_to(10, 20, 20, 0);

    const auto tight = p.bounds();
    const auto hull  = p.control_bounds();
    REQUIRE_THAT(tight.height, Catch::Matchers::WithinAbs(10.0, 1e-3));
    REQUIRE_THAT(hull.height, Catch::Matchers::WithinAbs(20.0, 1e-3));
    REQUIRE_THAT(tight.width, Catch::Matchers::WithinAbs(20.0, 1e-3));
}

TEST_CASE("flatten turns a rectilinear subpath into its own corners",
          "[canvas][path]") {
    const auto subs = unit_square().flatten();
    REQUIRE(subs.size() == 1);
    REQUIRE(subs[0].closed);
    REQUIRE(subs[0].points.size() == 4);
    require_point(subs[0].points[0], 0, 0);
    require_point(subs[0].points[1], 10, 0);
    require_point(subs[0].points[2], 10, 10);
    require_point(subs[0].points[3], 0, 10);
}

TEST_CASE("flatten stays within tolerance of a curve", "[canvas][path]") {
    // Every flattened vertex of a cubic must lie within the tolerance band of the
    // true curve. Check the sampled polyline never bulges past the known peak of
    // this symmetric arch (control height 100 -> curve peak 75, see the tight-
    // bounds case) by more than the tolerance.
    Path p;
    p.move_to(0, 0).cubic_to(0, 100, 100, 100, 100, 0);

    const float tol = 0.25f;
    const auto subs = p.flatten(tol);
    REQUIRE(subs.size() == 1);
    REQUIRE(subs[0].points.size() > 2);   // actually subdivided
    for (const Point2D& pt : subs[0].points)
        REQUIRE(pt.y <= 75.0f + tol + 1e-3f);
}

TEST_CASE("a path round-trips through its SVG string", "[canvas][path]") {
    Path source;
    source.move_to(0, 0).line_to(10, 0).quad_to(15, 5, 10, 10)
          .cubic_to(8, 12, 2, 12, 0, 10).close();

    const Path reparsed = Path::from_svg_string(source.to_svg_string());
    REQUIRE(reparsed == source);
}

TEST_CASE("from_svg_string builds the geometry the commands describe",
          "[canvas][path]") {
    // A unit square in absolute commands.
    const Path p = Path::from_svg_string("M0 0 L10 0 L10 10 L0 10 Z");
    REQUIRE(verbs_of(p) == std::vector<Path::Verb>{
        Path::Verb::move, Path::Verb::line, Path::Verb::line,
        Path::Verb::line, Path::Verb::close});
    const auto b = p.bounds();
    REQUIRE_THAT(b.width, Catch::Matchers::WithinAbs(10.0, 1e-4));
    REQUIRE_THAT(b.height, Catch::Matchers::WithinAbs(10.0, 1e-4));

    // Relative commands accumulate from the current point: m + l is the same
    // square-corner start as M + L.
    const Path rel = Path::from_svg_string("m1 1 l10 0");
    require_point(*rel.current_point(), 11, 1);

    // An elliptical arc lands on its declared endpoint.
    const Path arc = Path::from_svg_string("M0 0 A 10 10 0 0 1 10 10");
    REQUIRE_FALSE(arc.is_empty());
    require_point(*arc.current_point(), 10, 10, 0.02f);

    // Malformed input yields the clean prefix, never a throw.
    const Path partial = Path::from_svg_string("M0 0 L5 5 GARBAGE");
    REQUIRE(verbs_of(partial) == std::vector<Path::Verb>{
        Path::Verb::move, Path::Verb::line});
}

TEST_CASE("add_path appends a snapshot of the path onto itself", "[canvas][path]") {
    // The self-append branch must read a snapshot before its own buffer is
    // reallocated. Appending a square to itself doubles the verbs and leaves the
    // bounds unchanged (the copy lands exactly on top under the identity).
    Path p = unit_square();
    const size_t before = p.verb_count();
    const auto bounds_before = p.bounds();

    p.add_path(p);

    REQUIRE(p.verb_count() == before * 2);
    const auto after = p.bounds();
    REQUIRE_THAT(after.x, Catch::Matchers::WithinAbs(bounds_before.x, 1e-4));
    REQUIRE_THAT(after.width, Catch::Matchers::WithinAbs(bounds_before.width, 1e-4));
}

TEST_CASE("transform_to_fit exposes the fit transform without applying it",
          "[canvas][path]") {
    // transform_to_fit returns the transform scale_to_fit would apply, leaving
    // the path untouched -- the "expose the pipeline" seam. A 10x10 square into a
    // 200x50 box with free proportions scales x by 20 and y by 5 about the origin.
    const Path p = unit_square();
    const AffineTransform t = p.transform_to_fit(0, 0, 200, 50, /*preserve=*/false);

    REQUIRE(p == unit_square());              // the query did not mutate
    require_point(t.transform_point({0, 0}), 0, 0);
    require_point(t.transform_point({10, 10}), 200, 50);

    // On a degenerate (zero-width) path it returns identity, never a NaN factory.
    Path line;
    line.move_to(5, 0).line_to(5, 10);
    REQUIRE(line.transform_to_fit(0, 0, 100, 100, false).is_identity());
}
