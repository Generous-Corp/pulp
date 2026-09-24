// PathMeasure — arc length, pose lookup, and trimming over a Path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

#include <pulp/canvas/path.hpp>
#include <pulp/canvas/path_measure.hpp>

using pulp::canvas::Path;
using pulp::canvas::PathMeasure;
using pulp::canvas::Point2D;

namespace {

Path line(float x0, float y0, float x1, float y1) {
    Path p;
    p.move_to(x0, y0);
    p.line_to(x1, y1);
    return p;
}

float span(Point2D a, Point2D b) {
    return std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
}

/// Walk a path's own verbs and sum the straight-line distance between the
/// on-path points. For a polyline this is the exact length, which makes it an
/// independent check on the measure rather than a restatement of it.
float polyline_length(const Path& path) {
    float total = 0.0f;
    Point2D cursor{};
    Point2D start{};
    bool have = false;
    for (Path::Element el : path) {
        switch (el.verb) {
        case Path::Verb::move:
            cursor = el.points[0];
            start = cursor;
            have = true;
            break;
        case Path::Verb::line:
            if (have)
                total += span(cursor, el.points[0]);
            cursor = el.points[0];
            break;
        case Path::Verb::quad:
            cursor = el.points[1];
            break;
        case Path::Verb::cubic:
            cursor = el.points[2];
            break;
        case Path::Verb::close:
            if (have)
                total += span(cursor, start);
            cursor = start;
            break;
        }
    }
    return total;
}

} // namespace

TEST_CASE("PathMeasure measures straight lines exactly", "[canvas][path-measure]") {
    const PathMeasure m{line(0.0f, 0.0f, 3.0f, 4.0f)};
    REQUIRE(m.contour_count() == 1);
    CHECK_THAT(m.length(), Catch::Matchers::WithinAbs(5.0f, 1e-4f));

    // The 3-4-5 triangle makes the midpoint exact, so this pins interpolation
    // rather than just "somewhere along the line".
    const auto pose = m.pose_at(2.5f);
    REQUIRE(pose.has_value());
    CHECK_THAT(pose->position.x, Catch::Matchers::WithinAbs(1.5f, 1e-4f));
    CHECK_THAT(pose->position.y, Catch::Matchers::WithinAbs(2.0f, 1e-4f));
    CHECK_THAT(pose->tangent.x, Catch::Matchers::WithinAbs(0.6f, 1e-4f));
    CHECK_THAT(pose->tangent.y, Catch::Matchers::WithinAbs(0.8f, 1e-4f));
}

TEST_CASE("PathMeasure clamps out-of-range distances to the endpoints", "[canvas][path-measure]") {
    const PathMeasure m{line(0.0f, 0.0f, 10.0f, 0.0f)};

    // A meter driven past its maximum should pin, not vanish.
    const auto over = m.pose_at(1000.0f);
    REQUIRE(over.has_value());
    CHECK_THAT(over->position.x, Catch::Matchers::WithinAbs(10.0f, 1e-4f));

    const auto under = m.pose_at(-50.0f);
    REQUIRE(under.has_value());
    CHECK_THAT(under->position.x, Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("PathMeasure reports nothing for a path with no length", "[canvas][path-measure]") {
    Path empty;
    CHECK(PathMeasure{empty}.length() == 0.0f);
    CHECK_FALSE(PathMeasure{empty}.pose_at(0.0f).has_value());
    CHECK(PathMeasure{empty}.contour_count() == 0);

    // A lone move_to, and a close on a contour that never moved, are both
    // zero-length: they must not manufacture a contour.
    Path moved;
    moved.move_to(5.0f, 5.0f);
    CHECK(PathMeasure{moved}.contour_count() == 0);
    CHECK_FALSE(PathMeasure{moved}.position_at(0.0f).has_value());

    Path closed_without_length;
    closed_without_length.move_to(1.0f, 1.0f);
    closed_without_length.close();
    CHECK(PathMeasure{closed_without_length}.contour_count() == 0);

    // Control: the same class DOES report a contour once there is length, so
    // the zeros above are absence, not a broken measure.
    CHECK(PathMeasure{line(0.0f, 0.0f, 1.0f, 0.0f)}.contour_count() == 1);
}

TEST_CASE("PathMeasure counts contours and their lengths separately", "[canvas][path-measure]") {
    Path p;
    p.move_to(0.0f, 0.0f);
    p.line_to(4.0f, 0.0f);
    p.move_to(0.0f, 10.0f);
    p.line_to(6.0f, 10.0f);

    const PathMeasure m{p};
    REQUIRE(m.contour_count() == 2);
    CHECK_THAT(m.contour_length(0), Catch::Matchers::WithinAbs(4.0f, 1e-4f));
    CHECK_THAT(m.contour_length(1), Catch::Matchers::WithinAbs(6.0f, 1e-4f));
    CHECK_THAT(m.length(), Catch::Matchers::WithinAbs(10.0f, 1e-4f));
    CHECK_THAT(m.contour_start(0), Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(m.contour_start(1), Catch::Matchers::WithinAbs(4.0f, 1e-4f));

    // Distance keeps running across the contour break rather than restarting.
    const auto pose = m.pose_at(7.0f);
    REQUIRE(pose.has_value());
    CHECK_THAT(pose->position.x, Catch::Matchers::WithinAbs(3.0f, 1e-4f));
    CHECK_THAT(pose->position.y, Catch::Matchers::WithinAbs(10.0f, 1e-4f));
}

TEST_CASE("PathMeasure closes a contour with the closing edge", "[canvas][path-measure]") {
    Path square;
    square.move_to(0.0f, 0.0f);
    square.line_to(10.0f, 0.0f);
    square.line_to(10.0f, 10.0f);
    square.line_to(0.0f, 10.0f);
    square.close();

    const PathMeasure m{square};
    REQUIRE(m.contour_count() == 1);
    CHECK(m.contour_is_closed(0));
    // 40, not 30 — the closing edge counts.
    CHECK_THAT(m.length(), Catch::Matchers::WithinAbs(40.0f, 1e-4f));
}

TEST_CASE("PathMeasure approximates a circle's circumference", "[canvas][path-measure]") {
    Path circle;
    circle.add_circle(0.0f, 0.0f, 100.0f);

    const PathMeasure m{circle, 0.01f};
    const float circumference = 2.0f * std::numbers::pi_v<float> * 100.0f;

    // `add_circle` flattens to four cubics at build time, and those cubics are
    // very slightly longer than the circle they approximate. So the measure
    // converges to the CUBICS' length (~628.40), not to 2*pi*r (~628.32).
    // Asserting `<= circumference` here would be asserting that Path stores a
    // true circle, which it does not.
    CHECK_THAT(m.length(), Catch::Matchers::WithinRel(circumference, 0.001f));

    // Every sampled point must sit on the circle.
    for (int i = 0; i <= 16; ++i) {
        const float d = m.length() * static_cast<float>(i) / 16.0f;
        const auto point = m.position_at(d);
        REQUIRE(point.has_value());
        CHECK_THAT(std::sqrt(point->x * point->x + point->y * point->y),
                   Catch::Matchers::WithinAbs(100.0f, 0.05f));
    }
}

TEST_CASE("PathMeasure measures the path, not the shape it approximates",
          "[canvas][path-measure]") {
    Path circle;
    circle.add_circle(0.0f, 0.0f, 100.0f);

    // Path's verb set is closed and has no conic, so add_circle stores four
    // cubics. Those cubics are marginally longer than the circle, and a fine
    // measure converges to THEM — crossing 2*pi*r on the way. Pinning this
    // stops a future reader from "fixing" the measure to match the ideal.
    int cubics = 0;
    for (Path::Element el : circle) {
        if (el.verb == Path::Verb::cubic)
            ++cubics;
    }
    CHECK(cubics == 4);

    const float circumference = 2.0f * std::numbers::pi_v<float> * 100.0f;
    const PathMeasure converged{circle, 0.001f};
    CHECK(converged.length() > circumference);
    // ...but only just: well under a tenth of a percent.
    CHECK(converged.length() - circumference < 0.001f * circumference);
}

TEST_CASE("PathMeasure trims a polyline to a prefix", "[canvas][path-measure]") {
    Path p;
    p.move_to(0.0f, 0.0f);
    p.line_to(10.0f, 0.0f);
    p.line_to(10.0f, 10.0f);

    const PathMeasure m{p};
    REQUIRE_THAT(m.length(), Catch::Matchers::WithinAbs(20.0f, 1e-4f));

    // A value arc at 0.75 of the way round.
    const Path trimmed = m.segment(0.0f, 15.0f);
    CHECK_FALSE(trimmed.is_empty());
    CHECK_THAT(polyline_length(trimmed), Catch::Matchers::WithinAbs(15.0f, 1e-3f));

    // The trim ends where the measure says it should.
    const auto expected_end = m.position_at(15.0f);
    REQUIRE(expected_end.has_value());
    const PathMeasure trimmed_measure{trimmed};
    const auto actual_end = trimmed_measure.position_at(trimmed_measure.length());
    REQUIRE(actual_end.has_value());
    CHECK_THAT(actual_end->x, Catch::Matchers::WithinAbs(expected_end->x, 1e-3f));
    CHECK_THAT(actual_end->y, Catch::Matchers::WithinAbs(expected_end->y, 1e-3f));
}

TEST_CASE("PathMeasure trims an interior run", "[canvas][path-measure]") {
    const PathMeasure m{line(0.0f, 0.0f, 100.0f, 0.0f)};
    const Path middle = m.segment(25.0f, 75.0f);
    REQUIRE_FALSE(middle.is_empty());

    const PathMeasure trimmed{middle};
    CHECK_THAT(trimmed.length(), Catch::Matchers::WithinAbs(50.0f, 1e-3f));
    const auto start = trimmed.position_at(0.0f);
    REQUIRE(start.has_value());
    CHECK_THAT(start->x, Catch::Matchers::WithinAbs(25.0f, 1e-3f));
}

TEST_CASE("PathMeasure returns an empty trim for an inverted or empty range",
          "[canvas][path-measure]") {
    const PathMeasure m{line(0.0f, 0.0f, 10.0f, 0.0f)};
    CHECK(m.segment(8.0f, 2.0f).is_empty());
    CHECK(m.segment(5.0f, 5.0f).is_empty());

    // Control: a well-ordered range on the same measure is NOT empty, so the
    // emptiness above is the range, not a broken trim.
    CHECK_FALSE(m.segment(2.0f, 8.0f).is_empty());
}

TEST_CASE("PathMeasure keeps curve verbs when trimming a curve", "[canvas][path-measure]") {
    Path curve;
    curve.move_to(0.0f, 0.0f);
    curve.cubic_to(0.0f, 100.0f, 100.0f, 100.0f, 100.0f, 0.0f);

    const PathMeasure m{curve};
    const Path trimmed = m.segment(m.length() * 0.25f, m.length() * 0.75f);
    REQUIRE_FALSE(trimmed.is_empty());

    // A trimmed cubic must come back as a cubic — degrading to a polyline
    // would lose the curve the caller is about to stroke.
    bool saw_cubic = false;
    int line_verbs = 0;
    for (Path::Element el : trimmed) {
        if (el.verb == Path::Verb::cubic)
            saw_cubic = true;
        if (el.verb == Path::Verb::line)
            ++line_verbs;
    }
    CHECK(saw_cubic);
    CHECK(line_verbs == 0);

    // The trimmed curve's own length matches the range that produced it.
    const PathMeasure trimmed_measure{trimmed};
    CHECK_THAT(trimmed_measure.length(), Catch::Matchers::WithinAbs(m.length() * 0.5f, 0.5f));
}

TEST_CASE("PathMeasure tangent follows direction around a corner", "[canvas][path-measure]") {
    Path corner;
    corner.move_to(0.0f, 0.0f);
    corner.line_to(10.0f, 0.0f);
    corner.line_to(10.0f, 10.0f);

    const PathMeasure m{corner};
    const auto before = m.tangent_at(5.0f);
    const auto after = m.tangent_at(15.0f);
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());

    CHECK_THAT(before->x, Catch::Matchers::WithinAbs(1.0f, 1e-3f));
    CHECK_THAT(before->y, Catch::Matchers::WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(after->x, Catch::Matchers::WithinAbs(0.0f, 1e-3f));
    CHECK_THAT(after->y, Catch::Matchers::WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("PathMeasure tolerance trades accuracy for sample count", "[canvas][path-measure]") {
    Path circle;
    circle.add_circle(0.0f, 0.0f, 100.0f);

    const PathMeasure coarse{circle, 4.0f};
    const PathMeasure fine{circle, 0.01f};
    // The converged length of the path's own geometry, approached from below.
    const PathMeasure reference{circle, 0.001f};

    // Flattening always under-estimates the path, and a finer tolerance is
    // strictly closer. This monotonicity is what makes the default safe: it
    // bounds the error on one side rather than merely being "small".
    CHECK(coarse.length() < fine.length());
    CHECK(fine.length() <= reference.length() + 1e-3f);
    CHECK(std::abs(reference.length() - fine.length()) <
          std::abs(reference.length() - coarse.length()));
}

TEST_CASE("PathMeasure survives the source path being modified", "[canvas][path-measure]") {
    Path p = line(0.0f, 0.0f, 10.0f, 0.0f);
    const PathMeasure m{p};

    // The measure holds its own geometry, so mutating the source must not
    // move the measurement underneath a caller holding it.
    p.line_to(10.0f, 100.0f);
    CHECK_THAT(m.length(), Catch::Matchers::WithinAbs(10.0f, 1e-4f));
}
