#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/animator_set.hpp>
#include <cmath>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ── AnimatorSetBuilder ──────────────────────────────────────────────────

TEST_CASE("AnimatorSetBuilder sequence", "[animation][animator_set]") {
    float value = 0.0f;

    auto runner = AnimatorSetBuilder()
        .then(0.0f, 1.0f, 1.0f, [&](float v) { value = v; }, easing::linear)
        .then(1.0f, 0.0f, 1.0f, [&](float v) { value = v; }, easing::linear)
        .build_runner();

    // First tween
    runner.advance(0.5f);
    REQUIRE_THAT(value, WithinAbs(0.5f, 0.05f));

    runner.advance(0.5f);
    REQUIRE_THAT(value, WithinAbs(1.0f, 0.05f));

    // Second tween
    runner.advance(0.5f);
    REQUIRE_THAT(value, WithinAbs(0.5f, 0.05f));

    runner.advance(0.5f);
    REQUIRE(runner.finished());
}

TEST_CASE("AnimatorSetBuilder parallel", "[animation][animator_set]") {
    float a = 0.0f, b = 0.0f;

    auto runner = AnimatorSetBuilder()
        .begin_parallel()
        .with(0.0f, 1.0f, 1.0f, [&](float v) { a = v; })
        .with(0.0f, 2.0f, 1.0f, [&](float v) { b = v; })
        .end_parallel()
        .build_runner();

    runner.advance(1.0f);
    REQUIRE_THAT(a, WithinAbs(1.0f, 0.05f));
    REQUIRE_THAT(b, WithinAbs(2.0f, 0.05f));
    REQUIRE(runner.finished());
}

TEST_CASE("AnimatorSetBuilder delay", "[animation][animator_set]") {
    float value = 0.0f;

    auto runner = AnimatorSetBuilder()
        .delay(0.5f)
        .then(0.0f, 1.0f, 1.0f, [&](float v) { value = v; })
        .build_runner();

    runner.advance(0.4f);
    REQUIRE_THAT(value, WithinAbs(0.0f, 0.01f));  // Still in delay

    runner.advance(0.2f);  // Finishes delay (0.6 total)
    // Value still 0 because this advance just finished the delay step

    runner.advance(0.5f);  // Now in the tween
    REQUIRE(value > 0.0f);  // Tween started
}

// ── Easing functions ────────────────────────────────────────────────────

TEST_CASE("Cubic bezier easing", "[animation][easing]") {
    // CSS ease: cubic-bezier(0.25, 0.1, 0.25, 1.0)
    float v = easing::cubic_bezier(0.5f, 0.25f, 0.1f, 0.25f, 1.0f);
    REQUIRE(v > 0.0f);
    REQUIRE(v < 1.0f);

    REQUIRE_THAT(easing::cubic_bezier(0.0f, 0.25f, 0.1f, 0.25f, 1.0f), WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(easing::cubic_bezier(1.0f, 0.25f, 0.1f, 0.25f, 1.0f), WithinAbs(1.0f, 0.01f));
}

TEST_CASE("Spring easing", "[animation][easing]") {
    // Underdamped spring should overshoot
    float v = easing::spring(0.5f, 100.0f, 5.0f);
    REQUIRE(v > 0.5f);  // Overshoots

    // Should converge to 1.0
    float final_v = easing::spring(5.0f, 100.0f, 10.0f);
    REQUIRE_THAT(final_v, WithinAbs(1.0f, 0.05f));
}

TEST_CASE("Ease in-out back", "[animation][easing]") {
    REQUIRE_THAT(easing::ease_in_out_back(0.0f), WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(easing::ease_in_out_back(1.0f), WithinAbs(1.0f, 0.01f));
    // Should overshoot slightly
    REQUIRE(easing::ease_in_out_back(0.9f) > 1.0f);
}

// ── Vector3D ────────────────────────────────────────────────────────────

TEST_CASE("Vector3D operations", "[animation][3d]") {
    Vector3D a{1, 0, 0};
    Vector3D b{0, 1, 0};

    auto sum = a + b;
    REQUIRE_THAT(sum.x, WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(sum.y, WithinAbs(1.0f, 1e-5));

    REQUIRE_THAT(a.dot(b), WithinAbs(0.0f, 1e-5));

    auto cross = a.cross(b);
    REQUIRE_THAT(cross.z, WithinAbs(1.0f, 1e-5));

    REQUIRE_THAT(a.length(), WithinAbs(1.0f, 1e-5));

    Vector3D c{3, 4, 0};
    REQUIRE_THAT(c.normalized().length(), WithinAbs(1.0f, 1e-5));
}

// ── Quaternion ──────────────────────────────────────────────────────────

TEST_CASE("Quaternion identity", "[animation][3d]") {
    auto q = Quaternion::identity();
    Vector3D v{1, 2, 3};
    auto rotated = q.rotate(v);
    REQUIRE_THAT(rotated.x, WithinAbs(1.0f, 1e-4));
    REQUIRE_THAT(rotated.y, WithinAbs(2.0f, 1e-4));
    REQUIRE_THAT(rotated.z, WithinAbs(3.0f, 1e-4));
}

TEST_CASE("Quaternion 90-degree rotation", "[animation][3d]") {
    auto q = Quaternion::from_axis_angle({0, 0, 1}, 3.14159f / 2.0f);
    Vector3D v{1, 0, 0};
    auto rotated = q.rotate(v);
    REQUIRE_THAT(rotated.x, WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(rotated.y, WithinAbs(1.0f, 0.01f));
}

TEST_CASE("Quaternion slerp", "[animation][3d]") {
    auto a = Quaternion::identity();
    auto b = Quaternion::from_axis_angle({0, 0, 1}, 3.14159f / 2.0f);

    auto mid = Quaternion::slerp(a, b, 0.5f);
    Vector3D v{1, 0, 0};
    auto rotated = mid.rotate(v);

    // 45 degrees rotation should give ~(0.707, 0.707, 0)
    REQUIRE_THAT(rotated.x, WithinAbs(0.707f, 0.02f));
    REQUIRE_THAT(rotated.y, WithinAbs(0.707f, 0.02f));
}

// ── Draggable3DOrientation ──────────────────────────────────────────────

TEST_CASE("Draggable3DOrientation basic", "[animation][3d]") {
    Draggable3DOrientation drag;

    // No drag — identity orientation
    auto q = drag.orientation();
    REQUIRE_THAT(q.w, WithinAbs(1.0f, 1e-5));

    // Drag from center-left to center-right
    drag.begin_drag(-0.5f, 0.0f);
    drag.update_drag(0.5f, 0.0f);
    drag.end_drag();

    // Orientation should have changed
    auto q2 = drag.orientation();
    REQUIRE(std::abs(q2.w) < 1.0f);  // Not identity anymore
}
