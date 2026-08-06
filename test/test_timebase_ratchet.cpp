#include <pulp/timebase/ratchet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace pulp::timebase;

TEST_CASE("ratchet projection validates before writing output") {
    constexpr TickPosition sentinel{777};
    std::array<TickPosition, 1> output{sentinel};

    auto result = project_ratchet_interval<4>({10}, {10}, 1, {0}, {20}, output);
    REQUIRE(result.error == RatchetError::InvalidInterval);
    REQUIRE(output[0] == sentinel);

    result = project_ratchet_interval<4>({10}, {9}, 1, {0}, {20}, output);
    REQUIRE(result.error == RatchetError::InvalidInterval);
    REQUIRE(output[0] == sentinel);

    result = project_ratchet_interval<4>({0}, {10}, 0, {0}, {20}, output);
    REQUIRE(result.error == RatchetError::InvalidHitCount);
    REQUIRE(output[0] == sentinel);

    result = project_ratchet_interval<4>({0}, {10}, 5, {0}, {20}, output);
    REQUIRE(result.error == RatchetError::CapacityExceeded);
    REQUIRE(output[0] == sentinel);

    result = project_ratchet_interval<4>({0}, {3}, 4, {0}, {20}, output);
    REQUIRE(result.error == RatchetError::InsufficientResolution);
    REQUIRE(output[0] == sentinel);

    result = project_ratchet_interval<4>({0}, {10}, 1, {20}, {19}, output);
    REQUIRE(result.error == RatchetError::InvalidWindow);
    REQUIRE(output[0] == sentinel);

    result = project_ratchet_interval<4>({0}, {10}, 2, {0}, {10}, output);
    REQUIRE(result.error == RatchetError::OutputTooSmall);
    REQUIRE(result.event_count == 0);
    REQUIRE(output[0] == sentinel);
}

TEST_CASE("ratchet projection subdivides divisible and non-divisible intervals exactly") {
    std::array<TickPosition, 8> output{};

    auto result = project_ratchet_interval<8>({0}, {16}, 4, {0}, {16}, output);
    REQUIRE(result);
    REQUIRE(result.event_count == 4);
    REQUIRE(output[0] == TickPosition{0});
    REQUIRE(output[1] == TickPosition{4});
    REQUIRE(output[2] == TickPosition{8});
    REQUIRE(output[3] == TickPosition{12});

    result = project_ratchet_interval<8>({0}, {10}, 4, {0}, {10}, output);
    REQUIRE(result.event_count == 4);
    REQUIRE(output[0] == TickPosition{0});
    REQUIRE(output[1] == TickPosition{2});
    REQUIRE(output[2] == TickPosition{5});
    REQUIRE(output[3] == TickPosition{7});

    result = project_ratchet_interval<8>({-10}, {0}, 4, {-10}, {0}, output);
    REQUIRE(result.event_count == 4);
    REQUIRE(output[0] == TickPosition{-10});
    REQUIRE(output[1] == TickPosition{-8});
    REQUIRE(output[2] == TickPosition{-5});
    REQUIRE(output[3] == TickPosition{-3});
}

TEST_CASE("ratchet intervals assign their shared boundary to the later interval") {
    std::array<TickPosition, 4> left{};
    std::array<TickPosition, 4> right{};
    const auto left_result = project_ratchet_interval<4>({0}, {10}, 3, {0}, {10}, left);
    const auto right_result = project_ratchet_interval<4>({10}, {20}, 3, {10}, {20}, right);

    REQUIRE(left_result.event_count == 3);
    REQUIRE(right_result.event_count == 3);
    REQUIRE(left[0] == TickPosition{0});
    REQUIRE(left[1] == TickPosition{3});
    REQUIRE(left[2] == TickPosition{6});
    REQUIRE(right[0] == TickPosition{10});
    REQUIRE(right[1] == TickPosition{13});
    REQUIRE(right[2] == TickPosition{16});
}

TEST_CASE("ratchet projection is invariant under half-open window partition") {
    std::array<TickPosition, 7> complete{};
    std::array<TickPosition, 7> first{};
    std::array<TickPosition, 7> second{};
    const auto complete_result = project_ratchet_interval<7>({-11}, {20}, 7, {-11}, {20}, complete);
    const auto first_result = project_ratchet_interval<7>({-11}, {20}, 7, {-11}, {4}, first);
    const auto second_result = project_ratchet_interval<7>({-11}, {20}, 7, {4}, {20}, second);

    REQUIRE(complete_result.event_count == 7);
    REQUIRE(first_result.event_count + second_result.event_count == complete_result.event_count);
    for (std::size_t index = 0; index < first_result.event_count; ++index)
        REQUIRE(first[index] == complete[index]);
    for (std::size_t index = 0; index < second_result.event_count; ++index)
        REQUIRE(second[index] == complete[first_result.event_count + index]);

    const auto empty = project_ratchet_interval<7>({-11}, {20}, 7, {4}, {4}, second);
    REQUIRE(empty);
    REQUIRE(empty.event_count == 0);
}

TEST_CASE("ratchet subdivision remains exact across the full signed tick domain") {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    std::array<TickPosition, 4> output{};

    const auto result =
        project_ratchet_interval<4>({minimum}, {maximum}, 4, {minimum}, {maximum}, output);
    REQUIRE(result.event_count == 4);
    REQUIRE(output[0] == TickPosition{minimum});
    REQUIRE(output[1] == TickPosition{-4'611'686'018'427'387'905LL});
    REQUIRE(output[2] == TickPosition{-1});
    REQUIRE(output[3] == TickPosition{4'611'686'018'427'387'903LL});
}

#if defined(__SIZEOF_INT128__)
TEST_CASE("ratchet cursor carries a near-maximum subdivision remainder without overflow") {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto hit_count = (std::uint64_t{1} << 63U) + 1;
    constexpr auto span = std::numeric_limits<std::uint64_t>::max();
    detail::RatchetSubdivisionCursor cursor({minimum}, {maximum}, hit_count);

    for (std::uint64_t index = 0; index < 4; ++index) {
        const auto expected_coordinate =
            static_cast<std::uint64_t>((static_cast<unsigned __int128>(span) * index) / hit_count);
        REQUIRE(cursor.position() == detail::tick_from_ordered_coordinate(expected_coordinate));
        cursor.advance();
    }
}

TEST_CASE("ratchet positions agree with a wide integer subdivision oracle") {
    auto state = std::uint64_t{0x243f'6a88'85a3'08d3ULL};
    std::array<TickPosition, 32> output{};

    for (std::size_t trial = 0; trial < 4096; ++trial) {
        state = state * 0x9e37'79b9'7f4a'7c15ULL + 0xbf58'476d'1ce4'e5b9ULL;
        const auto begin_coordinate = state;
        state = state * 0x9e37'79b9'7f4a'7c15ULL + 0xbf58'476d'1ce4'e5b9ULL;
        const auto available = std::numeric_limits<std::uint64_t>::max() - begin_coordinate;
        const auto span = available == 0 ? std::uint64_t{1} : std::uint64_t{1} + state % available;
        if (begin_coordinate > std::numeric_limits<std::uint64_t>::max() - span)
            continue;
        const auto end_coordinate = begin_coordinate + span;
        const auto begin = detail::tick_from_ordered_coordinate(begin_coordinate);
        const auto end = detail::tick_from_ordered_coordinate(end_coordinate);
        const auto hit_count = std::size_t{1} + trial % output.size();

        const auto result = project_ratchet_interval<32>(begin, end, hit_count, begin, end, output);
        if (span < hit_count) {
            REQUIRE(result.error == RatchetError::InsufficientResolution);
            continue;
        }
        REQUIRE(result.event_count == hit_count);
        for (std::size_t index = 0; index < hit_count; ++index) {
            const auto expected_coordinate =
                begin_coordinate + static_cast<std::uint64_t>(
                                       (static_cast<unsigned __int128>(span) * index) / hit_count);
            REQUIRE(output[index] == detail::tick_from_ordered_coordinate(expected_coordinate));
        }
    }
}
#endif

TEST_CASE("ratchet projection exposes a compile-time amplification bound") {
    constexpr auto exact_at_compile_time = [] {
        std::array<TickPosition, 3> positions{};
        const auto result = project_ratchet_interval<3>({-4}, {5}, 3, {-4}, {5}, positions);
        return result.event_count == 3 && positions[0] == TickPosition{-4} &&
               positions[1] == TickPosition{-1} && positions[2] == TickPosition{2};
    }();
    STATIC_REQUIRE(exact_at_compile_time);

    std::array<TickPosition, 8> output{};
    const auto result = project_ratchet_interval<8>({0}, {80}, 8, {0}, {80}, output);
    REQUIRE(result.event_count == 8);
}
