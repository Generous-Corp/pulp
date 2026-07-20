#include <pulp/timeline/automation_lane.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using namespace pulp;

namespace {

timeline::AutomationCurve curve(std::vector<timeline::AutomationPoint> points = {}) {
    auto result = timeline::AutomationCurve::create(std::move(points));
    REQUIRE(result);
    return std::move(result.value());
}

timeline::AutomationPoint point(std::uint64_t id, std::int64_t tick, float value) {
    return {{id}, timebase::TickPosition{tick}, value};
}

bool same_points(const timeline::AutomationCurve& lhs, const timeline::AutomationCurve& rhs) {
    return lhs.points().size() == rhs.points().size() &&
           std::equal(lhs.points().begin(), lhs.points().end(), rhs.points().begin());
}

} // namespace

static_assert(noexcept(std::declval<const timeline::AutomationLane&>().id()));
static_assert(noexcept(std::declval<const timeline::AutomationLane&>().target()));
static_assert(noexcept(std::declval<const timeline::AutomationLane&>().curve()));
static_assert(noexcept(std::declval<const timeline::AutomationLane&>().with_curve(
    std::declval<timeline::AutomationCurve>())));

TEST_CASE("AutomationLane preserves identity target and authored curve") {
    auto authored = curve({point(10, 0, 0.25f), point(11, 100, 0.75f)});
    auto lane =
        timeline::AutomationLane::create({1}, timeline::DeviceParameterTarget{{2}, 7}, authored);
    REQUIRE(lane);
    REQUIRE(lane.value().id() == timeline::ItemId{1});
    REQUIRE(std::get<timeline::DeviceParameterTarget>(lane.value().target()) ==
            timeline::DeviceParameterTarget{{2}, 7});
    REQUIRE(same_points(lane.value().curve(), authored));
}

TEST_CASE("AutomationLane rejects invalid document identities") {
    auto invalid_lane =
        timeline::AutomationLane::create({}, timeline::DeviceParameterTarget{{2}, 7}, curve());
    REQUIRE_FALSE(invalid_lane);
    REQUIRE(invalid_lane.error().code == timeline::AutomationLaneErrorCode::InvalidLaneId);
    REQUIRE(invalid_lane.error().lane == timeline::ItemId{});

    auto exhausted_lane =
        timeline::AutomationLane::create({std::numeric_limits<std::uint64_t>::max()},
                                         timeline::DeviceParameterTarget{{2}, 7}, curve());
    REQUIRE_FALSE(exhausted_lane);
    REQUIRE(exhausted_lane.error().code == timeline::AutomationLaneErrorCode::InvalidLaneId);
    REQUIRE(exhausted_lane.error().lane ==
            timeline::ItemId{std::numeric_limits<std::uint64_t>::max()});

    auto invalid_device =
        timeline::AutomationLane::create({1}, timeline::DeviceParameterTarget{{}, 7}, curve());
    REQUIRE_FALSE(invalid_device);
    REQUIRE(invalid_device.error().code ==
            timeline::AutomationLaneErrorCode::InvalidDevicePlacementId);
    REQUIRE(invalid_device.error().lane == timeline::ItemId{1});
    REQUIRE(invalid_device.error().related_item == timeline::ItemId{});

    auto exhausted_device = timeline::AutomationLane::create(
        {1}, timeline::DeviceParameterTarget{{std::numeric_limits<std::uint64_t>::max()}, 7},
        curve());
    REQUIRE_FALSE(exhausted_device);
    REQUIRE(exhausted_device.error().code ==
            timeline::AutomationLaneErrorCode::InvalidDevicePlacementId);
    REQUIRE(exhausted_device.error().related_item ==
            timeline::ItemId{std::numeric_limits<std::uint64_t>::max()});
}

TEST_CASE("AutomationLane keeps parameter identifiers opaque") {
    auto zero =
        timeline::AutomationLane::create({1}, timeline::DeviceParameterTarget{{2}, 0}, curve());
    auto maximum = timeline::AutomationLane::create(
        {1}, timeline::DeviceParameterTarget{{2}, std::numeric_limits<std::uint32_t>::max()},
        curve());
    REQUIRE(zero);
    REQUIRE(maximum);
    REQUIRE(std::get<timeline::DeviceParameterTarget>(zero.value().target()).param_id == 0);
    REQUIRE(std::get<timeline::DeviceParameterTarget>(maximum.value().target()).param_id ==
            std::numeric_limits<std::uint32_t>::max());
}

TEST_CASE("AutomationLane accepts an empty curve") {
    auto lane =
        timeline::AutomationLane::create({1}, timeline::DeviceParameterTarget{{2}, 7}, curve());
    REQUIRE(lane);
    REQUIRE(lane.value().curve().points().empty());
}

TEST_CASE("AutomationLane replaces curves without mutating its source") {
    auto original_curve = curve({point(10, 0, 0.25f), point(11, 100, 0.75f)});
    auto lane = timeline::AutomationLane::create({1}, timeline::DeviceParameterTarget{{2}, 7},
                                                 original_curve);
    REQUIRE(lane);

    auto replacement = curve({point(20, 0, 0.5f), point(21, 50, 0.75f), point(22, 100, 1.0f)});
    auto changed = lane.value().with_curve(replacement);
    REQUIRE(changed.id() == lane.value().id());
    REQUIRE(changed.target() == lane.value().target());
    REQUIRE(same_points(changed.curve(), replacement));
    REQUIRE(same_points(lane.value().curve(), original_curve));

    auto copied = lane.value();
    REQUIRE(copied.curve().points().data() == lane.value().curve().points().data());
}
