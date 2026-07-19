#include <pulp/timeline/automation_curve.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp;

namespace {

timeline::AutomationPoint point(
    std::uint64_t id, std::int64_t tick, float value,
    timeline::AutomationInterpolation interpolation = timeline::AutomationInterpolation::Continuous,
    float curvature = 0.0f) {
    return {{id}, timebase::TickPosition{tick}, value, interpolation, curvature};
}

} // namespace

TEST_CASE("AutomationCurve orders points and preserves stable identity") {
    auto curve = timeline::AutomationCurve::create(
        {point(1, 30, 0.75f), point(3, 10, 0.25f), point(2, 20, 0.5f)});
    REQUIRE(curve);
    REQUIRE(curve.value().points().size() == 3);
    REQUIRE(curve.value().points()[0].id == timeline::ItemId{3});
    REQUIRE(curve.value().points()[1].id == timeline::ItemId{2});
    REQUIRE(std::is_sorted(curve.value().points().begin(), curve.value().points().end(),
                           timeline::AutomationPointPositionLess{}));
    REQUIRE(curve.value().find_point({1}) != nullptr);
    REQUIRE(curve.value().find_point({99}) == nullptr);
}

TEST_CASE("AutomationCurve rejects invalid points") {
    auto invalid_id = timeline::AutomationCurve::create({point(0, 0, 0.0f)});
    REQUIRE_FALSE(invalid_id);
    REQUIRE(invalid_id.error().code == timeline::AutomationCurveErrorCode::InvalidPointId);

    auto duplicate_id = timeline::AutomationCurve::create({point(1, 0, 0.0f), point(1, 1, 1.0f)});
    REQUIRE_FALSE(duplicate_id);
    REQUIRE(duplicate_id.error().code == timeline::AutomationCurveErrorCode::DuplicatePointId);

    auto duplicate_position =
        timeline::AutomationCurve::create({point(1, 4, 0.0f), point(2, 4, 1.0f)});
    REQUIRE_FALSE(duplicate_position);
    REQUIRE(duplicate_position.error().code ==
            timeline::AutomationCurveErrorCode::DuplicatePosition);
    REQUIRE(duplicate_position.error().point == timeline::ItemId{2});
    REQUIRE(duplicate_position.error().related_point == timeline::ItemId{1});

    auto non_finite =
        timeline::AutomationCurve::create({point(1, 0, std::numeric_limits<float>::infinity())});
    REQUIRE_FALSE(non_finite);
    REQUIRE(non_finite.error().code == timeline::AutomationCurveErrorCode::NonFiniteValue);

    auto interpolation = timeline::AutomationCurve::create(
        {point(1, 0, 0.0f, static_cast<timeline::AutomationInterpolation>(0xff))});
    REQUIRE_FALSE(interpolation);
    REQUIRE(interpolation.error().code == timeline::AutomationCurveErrorCode::InvalidInterpolation);

    auto curvature = timeline::AutomationCurve::create(
        {point(1, 0, 0.0f, timeline::AutomationInterpolation::Continuous, 1.01f)});
    REQUIRE_FALSE(curvature);
    REQUIRE(curvature.error().code == timeline::AutomationCurveErrorCode::InvalidCurvature);
}

TEST_CASE("AutomationCurve evaluates empty clamped and exact positions") {
    auto empty = timeline::AutomationCurve::create({});
    REQUIRE(empty);
    REQUIRE_FALSE(empty.value().value_at({0}).has_value());

    auto curve = timeline::AutomationCurve::create({point(1, 10, 0.25f), point(2, 20, 0.75f)});
    REQUIRE(curve);
    REQUIRE(curve.value().value_at({0}) == 0.25f);
    REQUIRE(curve.value().value_at({10}) == 0.25f);
    REQUIRE(curve.value().value_at({20}) == 0.75f);
    REQUIRE(curve.value().value_at({30}) == 0.75f);
}

TEST_CASE("AutomationCurve continuous segments honor curvature") {
    auto linear = timeline::AutomationCurve::create({point(1, 0, 0.0f), point(2, 100, 1.0f)});
    auto ease_in = timeline::AutomationCurve::create(
        {point(1, 0, 0.0f, timeline::AutomationInterpolation::Continuous, 0.5f),
         point(2, 100, 1.0f)});
    auto ease_out = timeline::AutomationCurve::create(
        {point(1, 0, 0.0f, timeline::AutomationInterpolation::Continuous, -0.5f),
         point(2, 100, 1.0f)});
    REQUIRE(linear);
    REQUIRE(ease_in);
    REQUIRE(ease_out);
    REQUIRE(*linear.value().value_at({50}) == 0.5f);
    REQUIRE(*ease_in.value().value_at({50}) == 0.375f);
    REQUIRE(*ease_out.value().value_at({50}) == 0.625f);
}

TEST_CASE("AutomationCurve evaluation has deterministic double-precision golden values") {
    auto ease_in = timeline::AutomationCurve::create(
        {point(1, -17, -0.375f, timeline::AutomationInterpolation::Continuous, 0.3f),
         point(2, 113, 0.8125f)});
    auto ease_out = timeline::AutomationCurve::create(
        {point(1, -17, -0.375f, timeline::AutomationInterpolation::Continuous, -0.65f),
         point(2, 113, 0.8125f)});
    REQUIRE(ease_in);
    REQUIRE(ease_out);
    REQUIRE(std::bit_cast<std::uint32_t>(*ease_in.value().value_at({29})) == 0xbd1485beu);
    REQUIRE(std::bit_cast<std::uint32_t>(*ease_out.value().value_at({71})) == 0x3f18ffd8u);

    const auto near_max = std::numeric_limits<std::int64_t>::max();
    auto adjacent_ticks =
        timeline::AutomationCurve::create({point(3, near_max - 2, 0.0f), point(4, near_max, 1.0f)});
    REQUIRE(adjacent_ticks);
    REQUIRE(adjacent_ticks.value().value_at({near_max - 1}) == 0.5f);
}

TEST_CASE("AutomationCurve hold segments step at the next point") {
    auto curve = timeline::AutomationCurve::create(
        {point(1, 0, 0.2f, timeline::AutomationInterpolation::Hold), point(2, 100, 0.8f)});
    REQUIRE(curve);
    REQUIRE(curve.value().value_at({99}) == 0.2f);
    REQUIRE(curve.value().value_at({100}) == 0.8f);
}

TEST_CASE("AutomationCurve edits preserve the original curve") {
    auto original = timeline::AutomationCurve::create({point(1, 0, 0.0f), point(2, 100, 1.0f)});
    REQUIRE(original);

    auto inserted = original.value().insert_point(point(3, 50, 0.25f));
    REQUIRE(inserted);
    REQUIRE(original.value().points().size() == 2);
    REQUIRE(inserted.value().points().size() == 3);
    REQUIRE(original.value().find_point({3}) == nullptr);

    auto replaced = inserted.value().replace_point(point(3, 60, 0.5f));
    REQUIRE(replaced);
    REQUIRE(replaced.value().find_point({3})->position == timebase::TickPosition{60});

    auto erased = replaced.value().erase_point({3});
    REQUIRE(erased);
    REQUIRE(erased.value().points().size() == 2);
    REQUIRE(std::equal(erased.value().points().begin(), erased.value().points().end(),
                       original.value().points().begin()));
}

TEST_CASE("AutomationCurve edit failures leave the source unchanged") {
    auto curve = timeline::AutomationCurve::create({point(1, 0, 0.0f), point(2, 100, 1.0f)});
    REQUIRE(curve);

    auto missing_replace = curve.value().replace_point(point(3, 50, 0.5f));
    REQUIRE_FALSE(missing_replace);
    REQUIRE(missing_replace.error().code == timeline::AutomationCurveErrorCode::MissingPoint);

    auto missing_erase = curve.value().erase_point({3});
    REQUIRE_FALSE(missing_erase);
    REQUIRE(missing_erase.error().code == timeline::AutomationCurveErrorCode::MissingPoint);
    REQUIRE(curve.value().points().size() == 2);
}
