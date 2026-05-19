#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/view/animator_set.hpp>
#include <cmath>

using namespace pulp::view;

TEST_CASE("AnimatorSetBuilder then creates sequential tween", "[view][animation]") {
    float value = 0.0f;
    auto runner = AnimatorSetBuilder()
        .then(0.0f, 1.0f, 0.5f, [&](float v) { value = v; })
        .build_runner();

    REQUIRE_FALSE(runner.finished());
    runner.advance(0.25f);
    REQUIRE(value > 0.0f);
    REQUIRE(value < 1.0f);

    runner.advance(0.3f);
    REQUIRE(value == Catch::Approx(1.0f));
    REQUIRE(runner.finished());
}

TEST_CASE("AnimatorSetBuilder sequential tweens run in order", "[view][animation]") {
    float a = 0, b = 0;
    auto runner = AnimatorSetBuilder()
        .then(0.0f, 10.0f, 0.2f, [&](float v) { a = v; })
        .then(0.0f, 20.0f, 0.3f, [&](float v) { b = v; })
        .build_runner();

    runner.advance(0.2f);
    REQUIRE(a == Catch::Approx(10.0f));

    runner.advance(0.3f);
    REQUIRE(b == Catch::Approx(20.0f));
}

TEST_CASE("AnimatorSetBuilder delay", "[view][animation]") {
    float v = 0;
    auto runner = AnimatorSetBuilder()
        .delay(0.5f)
        .then(0.0f, 1.0f, 0.1f, [&](float val) { v = val; })
        .build_runner();

    runner.advance(0.3f);
    REQUIRE(v == Catch::Approx(0.0f));  // still in delay

    runner.advance(0.3f);  // past delay, into tween
    REQUIRE(v > 0.0f);
}

TEST_CASE("AnimatorSetBuilder reset", "[view][animation]") {
    float v = 0;
    auto runner = AnimatorSetBuilder()
        .then(0.0f, 1.0f, 0.3f, [&](float val) { v = val; })
        .build_runner();

    runner.advance(0.3f);
    REQUIRE(runner.finished());

    runner.reset();
    REQUIRE_FALSE(runner.finished());
}

TEST_CASE("AnimatorSetBuilder empty and ignored parallel inputs are safe",
          "[view][animation][coverage][phase3]") {
    auto empty = AnimatorSetBuilder().build_runner();
    REQUIRE(empty.finished());
    REQUIRE(empty.advance(0.016f));
    empty.reset();
    REQUIRE(empty.finished());

    float value = 0.0f;
    auto ignored = AnimatorSetBuilder()
        .with(0.0f, 1.0f, 0.1f, [&](float v) { value = v; })
        .end_parallel()
        .build_runner();
    REQUIRE(ignored.finished());
    REQUIRE(ignored.advance(0.1f));
    REQUIRE(value == Catch::Approx(0.0f));
}

TEST_CASE("AnimatorSetBuilder parallel group updates members until all finish",
          "[view][animation][coverage][phase3]") {
    float fast = 0.0f;
    float slow = 0.0f;
    auto runner = AnimatorSetBuilder()
        .begin_parallel()
        .with(0.0f, 1.0f, 0.1f, [&](float v) { fast = v; })
        .with(10.0f, 20.0f, 0.3f, [&](float v) { slow = v; })
        .end_parallel()
        .build_runner();

    REQUIRE_FALSE(runner.advance(0.1f));
    REQUIRE(fast == Catch::Approx(1.0f));
    REQUIRE(slow > 10.0f);
    REQUIRE(slow < 20.0f);

    REQUIRE(runner.advance(0.3f));
    REQUIRE(fast == Catch::Approx(1.0f));
    REQUIRE(slow == Catch::Approx(20.0f));

    runner.reset();
    REQUIRE_FALSE(runner.finished());
    fast = 0.0f;
    slow = 0.0f;
    REQUIRE_FALSE(runner.advance(0.1f));
    REQUIRE(fast == Catch::Approx(1.0f));
    REQUIRE(slow > 10.0f);
}

// ── Vector3D ────────────────────────────────────────────────────────────

TEST_CASE("Vector3D arithmetic", "[view][3d]") {
    Vector3D a{1, 2, 3};
    Vector3D b{4, 5, 6};

    auto sum = a + b;
    REQUIRE(sum.x == Catch::Approx(5.0f));
    REQUIRE(sum.y == Catch::Approx(7.0f));

    auto scaled = a * 2.0f;
    REQUIRE(scaled.z == Catch::Approx(6.0f));

    REQUIRE(a.length() == Catch::Approx(std::sqrt(14.0f)));

    auto n = a.normalized();
    REQUIRE(n.length() == Catch::Approx(1.0f).margin(0.001f));
}

TEST_CASE("Vector3D subtraction and zero normalization are stable",
          "[view][3d][coverage][phase3]") {
    Vector3D a{3, 2, 1};
    Vector3D b{1, 5, -2};

    auto diff = a - b;
    REQUIRE(diff.x == Catch::Approx(2.0f));
    REQUIRE(diff.y == Catch::Approx(-3.0f));
    REQUIRE(diff.z == Catch::Approx(3.0f));

    auto zero = Vector3D{}.normalized();
    REQUIRE(zero.x == Catch::Approx(0.0f));
    REQUIRE(zero.y == Catch::Approx(0.0f));
    REQUIRE(zero.z == Catch::Approx(0.0f));
}

TEST_CASE("Vector3D dot and cross product", "[view][3d]") {
    Vector3D x{1, 0, 0};
    Vector3D y{0, 1, 0};

    REQUIRE(x.dot(y) == Catch::Approx(0.0f));
    REQUIRE(x.dot(x) == Catch::Approx(1.0f));

    auto cross = x.cross(y);
    REQUIRE(cross.z == Catch::Approx(1.0f));
}

// ── Quaternion ──────────────────────────────────────────────────────────

TEST_CASE("Quaternion identity", "[view][3d]") {
    auto q = Quaternion::identity();
    REQUIRE(q.w == Catch::Approx(1.0f));
    REQUIRE(q.length() == Catch::Approx(1.0f));
}

TEST_CASE("Quaternion from axis angle", "[view][3d]") {
    auto q = Quaternion::from_axis_angle({0, 1, 0}, 3.14159f);
    REQUIRE(q.length() == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("Quaternion multiply identity", "[view][3d]") {
    auto q = Quaternion::from_axis_angle({1, 0, 0}, 0.5f);
    auto id = Quaternion::identity();
    auto result = q * id;
    REQUIRE(result.w == Catch::Approx(q.w).margin(0.001f));
}

TEST_CASE("Quaternion rotates vectors and normalizes zero quaternions",
          "[view][3d][coverage][phase3]") {
    auto quarter_turn = Quaternion::from_axis_angle({0, 0, 1}, 3.14159265f * 0.5f);
    auto rotated = quarter_turn.rotate({1, 0, 0});
    REQUIRE(rotated.x == Catch::Approx(0.0f).margin(0.001f));
    REQUIRE(rotated.y == Catch::Approx(1.0f).margin(0.001f));
    REQUIRE(rotated.z == Catch::Approx(0.0f).margin(0.001f));

    auto normalized = Quaternion{0, 0, 0, 0}.normalized();
    REQUIRE(normalized.w == Catch::Approx(1.0f));
    REQUIRE(normalized.x == Catch::Approx(0.0f));
    REQUIRE(normalized.y == Catch::Approx(0.0f));
    REQUIRE(normalized.z == Catch::Approx(0.0f));
}

TEST_CASE("Quaternion slerp handles near and opposite hemispheres",
          "[view][3d][coverage][phase3]") {
    const auto identity = Quaternion::identity();
    const auto near = Quaternion::from_axis_angle({0, 1, 0}, 0.001f);
    auto blended_near = Quaternion::slerp(identity, near, 0.5f);
    REQUIRE(blended_near.length() == Catch::Approx(1.0f).margin(0.001f));

    const Quaternion opposite{-identity.w, -identity.x, -identity.y, -identity.z};
    auto blended_opposite = Quaternion::slerp(identity, opposite, 0.5f);
    REQUIRE(blended_opposite.w == Catch::Approx(1.0f).margin(0.001f));
    REQUIRE(blended_opposite.length() == Catch::Approx(1.0f).margin(0.001f));
}

TEST_CASE("Draggable3DOrientation drag lifecycle updates and resets orientation",
          "[view][3d][coverage][phase3]") {
    Draggable3DOrientation orientation;
    const auto initial = orientation.orientation();

    orientation.update_drag(0.5f, 0.0f);
    REQUIRE(orientation.orientation().w == Catch::Approx(initial.w));
    REQUIRE(orientation.orientation().x == Catch::Approx(initial.x));
    REQUIRE(orientation.orientation().y == Catch::Approx(initial.y));
    REQUIRE(orientation.orientation().z == Catch::Approx(initial.z));

    orientation.begin_drag(0.0f, 0.0f);
    orientation.update_drag(0.5f, 0.0f);
    REQUIRE(orientation.orientation().length() == Catch::Approx(1.0f).margin(0.001f));
    REQUIRE(orientation.orientation().y != Catch::Approx(0.0f));

    orientation.end_drag();
    orientation.update_drag(-0.5f, 0.0f);
    REQUIRE(orientation.orientation().y != Catch::Approx(0.0f));

    orientation.reset();
    REQUIRE(orientation.orientation().w == Catch::Approx(1.0f));
    REQUIRE(orientation.orientation().x == Catch::Approx(0.0f));
    REQUIRE(orientation.orientation().y == Catch::Approx(0.0f));
    REQUIRE(orientation.orientation().z == Catch::Approx(0.0f));
}
