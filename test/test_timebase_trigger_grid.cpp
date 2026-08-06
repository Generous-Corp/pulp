#include <pulp/timebase/trigger_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

using namespace pulp::timebase;

namespace {

using TestGrid = TriggerGrid<3, 4>;

constexpr TriggerCell trigger(std::uint8_t velocity = 100, TriggerProbability probability = {},
                              std::int64_t microtiming = 0) {
    return {true, velocity, probability, {microtiming}};
}

} // namespace

TEST_CASE("trigger grid configuration is transactional and bounded") {
    TestGrid grid;
    const std::array<std::uint64_t, 0> no_draws{};
    std::array<TriggerEvent, 1> no_events{};
    REQUIRE(grid.project_window({0}, {0}, {1}, no_draws, no_events).error ==
            TriggerGridError::NotConfigured);

    REQUIRE(grid.configure(2, 3, {12}) == TriggerGridError::None);
    REQUIRE(grid.track_count() == 2);
    REQUIRE(grid.step_count() == 3);
    REQUIRE(grid.step_duration() == TickDuration{12});
    REQUIRE(grid.minimum_microtiming() == TickDuration{-6});
    REQUIRE(grid.maximum_microtiming() == TickDuration{5});

    REQUIRE(grid.set_cell(1, 2, trigger(91)) == TriggerGridError::None);
    REQUIRE(grid.configure(0, 3, {12}) == TriggerGridError::InvalidDimensions);
    REQUIRE(grid.configure(4, 3, {12}) == TriggerGridError::CapacityExceeded);
    REQUIRE(grid.configure(2, 3, {0}) == TriggerGridError::InvalidStepDuration);
    REQUIRE(grid.configure(2, 2, {std::numeric_limits<std::int64_t>::max()}) ==
            TriggerGridError::PatternSpanOutOfRange);

    REQUIRE(grid.track_count() == 2);
    REQUIRE(grid.step_count() == 3);
    REQUIRE(*grid.cell(1, 2) == trigger(91));

    REQUIRE(grid.configure(1, 1, {8}) == TriggerGridError::None);
    REQUIRE(grid.cell(0, 0) != nullptr);
    REQUIRE(*grid.cell(0, 0) == TriggerCell{});
}

TEST_CASE("trigger cell validation leaves authored data unchanged on failure") {
    TestGrid grid;
    REQUIRE(grid.configure(2, 2, {8}) == TriggerGridError::None);
    const auto original = trigger(72, {1, 3}, -4);
    REQUIRE(grid.set_cell(0, 0, original) == TriggerGridError::None);

    REQUIRE(grid.set_cell(2, 0, trigger()) == TriggerGridError::IndexOutOfRange);
    REQUIRE(grid.set_cell(0, 2, trigger()) == TriggerGridError::IndexOutOfRange);
    REQUIRE(grid.set_cell(0, 0, trigger(0)) == TriggerGridError::InvalidVelocity);
    REQUIRE(grid.set_cell(0, 0, trigger(128)) == TriggerGridError::InvalidVelocity);
    REQUIRE(grid.set_cell(0, 0, trigger(80, {1, 0})) == TriggerGridError::InvalidProbability);
    REQUIRE(grid.set_cell(0, 0, trigger(80, {4, 3})) == TriggerGridError::InvalidProbability);
    REQUIRE(grid.set_cell(0, 0, trigger(80, {}, -5)) == TriggerGridError::InvalidMicrotiming);
    REQUIRE(grid.set_cell(0, 0, trigger(80, {}, 4)) == TriggerGridError::InvalidMicrotiming);

    REQUIRE(*grid.cell(0, 0) == original);
    REQUIRE(grid.cell(3, 0) == nullptr);
}

TEST_CASE("trigger projection uses exact authored positions and stable ordering") {
    TestGrid grid;
    REQUIRE(grid.configure(2, 3, {10}) == TriggerGridError::None);
    REQUIRE(grid.set_cell(1, 0, trigger(41, {}, 2)) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 1, trigger(52, {}, -5)) == TriggerGridError::None);
    REQUIRE(grid.set_cell(1, 1, trigger(63, {}, -5)) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 2, trigger(74, {}, 4)) == TriggerGridError::None);

    const std::array<std::uint64_t, 6> draws{};
    std::array<TriggerEvent, 6> events{};
    const auto result = grid.project_window({-20}, {-100}, {100}, draws, events);

    REQUIRE(result);
    REQUIRE(result.event_count == 4);
    REQUIRE(events[0] == TriggerEvent{{-18}, 1, 0, 41});
    REQUIRE(events[1] == TriggerEvent{{-15}, 0, 1, 52});
    REQUIRE(events[2] == TriggerEvent{{-15}, 1, 1, 63});
    REQUIRE(events[3] == TriggerEvent{{4}, 0, 2, 74});
}

TEST_CASE("trigger probability consumes stable coordinate draws") {
    TestGrid grid;
    REQUIRE(grid.configure(1, 4, {10}) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 0, trigger(10, {1, 4})) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 1, trigger(20, {1, 4})) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 2, trigger(30, {0, 1})) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 3, trigger(40, {1, 1})) == TriggerGridError::None);

    const std::array draws{
        std::uint64_t{0},
        std::numeric_limits<std::uint64_t>::max(),
        std::uint64_t{0},
        std::numeric_limits<std::uint64_t>::max(),
    };
    std::array<TriggerEvent, 4> events{};
    const auto result = grid.project_window({0}, {0}, {40}, draws, events);

    REQUIRE(result);
    REQUIRE(result.event_count == 2);
    REQUIRE(events[0] == TriggerEvent{{0}, 0, 0, 10});
    REQUIRE(events[1] == TriggerEvent{{30}, 0, 3, 40});
}

#if defined(__SIZEOF_INT128__)
TEST_CASE("trigger probability range reduction agrees with a wide integer oracle") {
    auto value = std::uint64_t{0x3243f6a8885a308dULL};
    for (std::size_t index = 0; index < 65'536; ++index) {
        value = value * 0x9e3779b97f4a7c15ULL + 0xbf58476d1ce4e5b9ULL;
        const auto rhs = value ^ (value >> 29U);
        const auto expected = static_cast<std::uint64_t>(
            (static_cast<unsigned __int128>(value) * static_cast<unsigned __int128>(rhs)) >> 64U);
        REQUIRE(detail::trigger_grid_multiply_high(value, rhs) == expected);
    }
}
#endif

TEST_CASE("trigger projection errors leave caller output unchanged") {
    TestGrid grid;
    REQUIRE(grid.configure(1, 2, {10}) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 0, trigger(10)) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 1, trigger(20)) == TriggerGridError::None);

    constexpr TriggerEvent sentinel{{777}, 9, 9, 99};
    std::array<TriggerEvent, 1> output{sentinel};
    const std::array<std::uint64_t, 2> draws{};
    const std::array<std::uint64_t, 1> short_draws{};

    auto result = grid.project_window({0}, {20}, {10}, draws, output);
    REQUIRE(result.error == TriggerGridError::InvalidWindow);
    REQUIRE(output[0] == sentinel);

    result = grid.project_window({0}, {0}, {20}, short_draws, output);
    REQUIRE(result.error == TriggerGridError::DrawCountMismatch);
    REQUIRE(output[0] == sentinel);

    result = grid.project_window({0}, {0}, {20}, draws, output);
    REQUIRE(result.error == TriggerGridError::OutputTooSmall);
    REQUIRE(result.event_count == 0);
    REQUIRE(output[0] == sentinel);
}

TEST_CASE("half-open trigger windows are exactly block partition invariant") {
    TestGrid grid;
    REQUIRE(grid.configure(2, 4, {10}) == TriggerGridError::None);
    for (std::size_t step = 0; step < grid.step_count(); ++step) {
        for (std::size_t track = 0; track < grid.track_count(); ++track) {
            REQUIRE(grid.set_cell(track, step,
                                  trigger(static_cast<std::uint8_t>(20 + step * 2 + track))) ==
                    TriggerGridError::None);
        }
    }

    const std::array<std::uint64_t, 8> draws{};
    std::array<TriggerEvent, 8> full{};
    std::array<TriggerEvent, 8> left{};
    std::array<TriggerEvent, 8> right{};
    const auto full_result = grid.project_window({-7}, {-7}, {33}, draws, full);
    const auto left_result = grid.project_window({-7}, {-7}, {13}, draws, left);
    const auto right_result = grid.project_window({-7}, {13}, {33}, draws, right);

    REQUIRE(full_result.event_count == 8);
    REQUIRE(left_result.event_count + right_result.event_count == full_result.event_count);
    for (std::size_t index = 0; index < left_result.event_count; ++index)
        REQUIRE(left[index] == full[index]);
    for (std::size_t index = 0; index < right_result.event_count; ++index)
        REQUIRE(right[index] == full[left_result.event_count + index]);
}

TEST_CASE("trigger projection is ordered and total at signed tick rails") {
    TriggerGrid<1, 3> grid;
    REQUIRE(grid.configure(1, 3, {8}) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 0, trigger(10, {}, -4)) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 1, trigger(20, {}, -4)) == TriggerGridError::None);
    REQUIRE(grid.set_cell(0, 2, trigger(30, {}, 3)) == TriggerGridError::None);

    const std::array<std::uint64_t, 3> draws{};
    std::array<TriggerEvent, 3> events{};
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto result = grid.project_window({maximum - 2}, {maximum - 6}, {maximum}, draws, events);

    REQUIRE(result.event_count == 1);
    REQUIRE(events[0].position == TickPosition{maximum - 6});

    const auto inclusive_rail =
        grid.project_window({maximum - 2}, {maximum}, {maximum}, draws, events);
    REQUIRE(inclusive_rail.event_count == 0);

    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto lower_rail =
        grid.project_window({minimum + 2}, {minimum}, {minimum + 1}, draws, events);
    REQUIRE(lower_rail.event_count == 1);
    REQUIRE(events[0] == TriggerEvent{{minimum}, 0, 0, 10});
}

TEST_CASE("trigger grid has a compile-time event amplification bound") {
    STATIC_REQUIRE(TestGrid::maximum_tracks == 3);
    STATIC_REQUIRE(TestGrid::maximum_steps == 4);
    STATIC_REQUIRE(TestGrid::maximum_events == 12);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TestGrid>);
}
