#include <pulp/music/pattern_development.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using namespace pulp::music;
using pulp::timebase::RandomCoordinate;
using pulp::timebase::TickPosition;

constexpr PatternEvent make_event(std::uint64_t id, std::int64_t tick,
                                  PatternEventRole role = PatternEventRole::primary,
                                  std::uint16_t accent = 1000) {
    return {id, {tick}, accent, role};
}

template <std::size_t Capacity, std::size_t Size>
DevelopmentPattern<Capacity> pattern_from(const std::array<PatternEvent, Size>& events) {
    DevelopmentPattern<Capacity> result;
    REQUIRE(result.assign(events) == PatternDevelopmentError::none);
    return result;
}

template <std::size_t Capacity>
std::uint64_t onset_mask(const DevelopmentPattern<Capacity>& pattern) {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto event = *pattern.event(index);
        REQUIRE(event.onset.value >= 0);
        REQUIRE(event.onset.value < 64);
        result |= std::uint64_t{1} << static_cast<unsigned>(event.onset.value);
    }
    return result;
}

DevelopmentPattern<8> pattern_for_mask(std::uint8_t mask) {
    DevelopmentPattern<8> result;
    for (std::uint64_t step = 0; step < 8; ++step) {
        if ((mask & (1u << step)) != 0)
            REQUIRE(result.insert(make_event(step + 1, static_cast<std::int64_t>(step))) ==
                    PatternDevelopmentError::none);
    }
    return result;
}

} // namespace

TEST_CASE("pattern events validate stable identity and canonical onset order",
          "[music][pattern-development]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<PatternEvent>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<DevelopmentPattern<64>>);
    STATIC_REQUIRE(DevelopmentPattern<64>::capacity == 64);
    STATIC_REQUIRE(noexcept(make_pattern_event_id(0, {})));
    STATIC_REQUIRE(noexcept(select_pattern_density(DevelopmentPattern<64>{}, {})));
    STATIC_REQUIRE(
        noexcept(apply_regional_fill(DevelopmentPattern<64>{}, DevelopmentPattern<64>{}, {})));
    STATIC_REQUIRE(
        noexcept(morph_patterns(DevelopmentPattern<64>{}, DevelopmentPattern<64>{}, {})));

    const RandomCoordinate coordinate{{1234}, 9, 7, 3};
    const auto id = make_pattern_event_id(0xBEEFu, coordinate);
    CHECK(id != 0);
    CHECK(make_pattern_event_id(0xBEEFu, coordinate) == id);
    CHECK(make_pattern_event_id(0xBEEFu, {{1235}, 9, 7, 3}) != id);

    DevelopmentPattern<4> pattern;
    const std::array events{
        make_event(3, 30, PatternEventRole::fill, 300),
        make_event(1, -10, PatternEventRole::anchor, 900),
        make_event(2, 10, PatternEventRole::ornament, 700),
    };
    REQUIRE(pattern.assign(events) == PatternDevelopmentError::none);
    REQUIRE(pattern.size() == 3);
    CHECK(pattern.event(0)->id == 1);
    CHECK(pattern.event(1)->id == 2);
    CHECK(pattern.event(2)->id == 3);
    CHECK(pattern.anchor_count() == 1);
    CHECK(pattern.find_id(2) == events[2]);
    CHECK(pattern.find_onset({30}) == events[0]);
    CHECK_FALSE(pattern.event(3));
    CHECK_FALSE(pattern.find_id(99));
}

TEST_CASE("pattern validation failures are atomic and exhaustive", "[music][pattern-development]") {
    DevelopmentPattern<2> pattern;
    REQUIRE(pattern.insert(make_event(1, 0)) == PatternDevelopmentError::none);
    const auto original = pattern;

    CHECK(pattern.insert(make_event(0, 1)) == PatternDevelopmentError::zero_event_id);
    CHECK(pattern == original);
    CHECK(pattern.insert(make_event(1, 1)) == PatternDevelopmentError::duplicate_event_id);
    CHECK(pattern == original);
    CHECK(pattern.insert(make_event(2, 0)) == PatternDevelopmentError::duplicate_onset);
    CHECK(pattern == original);
    CHECK(pattern.insert(make_event(2, 1, static_cast<PatternEventRole>(99))) ==
          PatternDevelopmentError::invalid_role);
    CHECK(pattern == original);
    CHECK(pattern.insert(make_event(2, 1, PatternEventRole::fill, 1001)) ==
          PatternDevelopmentError::accent_out_of_range);
    CHECK(pattern == original);

    REQUIRE(pattern.insert(make_event(2, 1)) == PatternDevelopmentError::none);
    CHECK(pattern.insert(make_event(3, 2)) == PatternDevelopmentError::capacity_exceeded);
    const auto full = pattern;
    const std::array replacement{make_event(4, 4), make_event(4, 5)};
    CHECK(pattern.assign(replacement) == PatternDevelopmentError::duplicate_event_id);
    CHECK(pattern == full);
    const std::array oversized{make_event(4, 4), make_event(5, 5), make_event(6, 6)};
    CHECK(pattern.assign(oversized) == PatternDevelopmentError::capacity_exceeded);
    CHECK(pattern == full);
}

TEST_CASE("onset set algebra matches exhaustive eight-bit arithmetic",
          "[music][pattern-development]") {
    for (std::uint16_t lhs_mask = 0; lhs_mask < 256; ++lhs_mask) {
        const auto lhs = pattern_for_mask(static_cast<std::uint8_t>(lhs_mask));
        for (std::uint16_t rhs_mask = 0; rhs_mask < 256; ++rhs_mask) {
            const auto rhs = pattern_for_mask(static_cast<std::uint8_t>(rhs_mask));
            const auto set_union = pattern_set(lhs, rhs, PatternSetOperation::set_union);
            const auto intersection = pattern_set(lhs, rhs, PatternSetOperation::intersection);
            const auto difference = pattern_set(lhs, rhs, PatternSetOperation::difference);
            const auto symmetric = pattern_set(lhs, rhs, PatternSetOperation::symmetric_difference);
            REQUIRE(set_union);
            REQUIRE(intersection);
            REQUIRE(difference);
            REQUIRE(symmetric);
            CHECK(onset_mask(set_union.pattern) == (lhs_mask | rhs_mask));
            CHECK(onset_mask(intersection.pattern) == (lhs_mask & rhs_mask));
            CHECK(onset_mask(difference.pattern) == (lhs_mask & ~rhs_mask & 0xFFu));
            CHECK(onset_mask(symmetric.pattern) == ((lhs_mask ^ rhs_mask) & 0xFFu));
        }
    }
}

TEST_CASE("set algebra rejects payload conflicts and capacity overflow atomically",
          "[music][pattern-development]") {
    const auto a = pattern_from<2>(std::array{make_event(1, 0), make_event(2, 2)});
    const auto conflict = pattern_from<2>(std::array{make_event(9, 0)});
    CHECK(pattern_set(a, conflict, PatternSetOperation::set_union).error ==
          PatternDevelopmentError::conflicting_event);
    CHECK(pattern_set(a, conflict, PatternSetOperation::symmetric_difference).error ==
          PatternDevelopmentError::conflicting_event);
    CHECK(pattern_set(a, conflict, PatternSetOperation::intersection).pattern ==
          pattern_from<2>(std::array{make_event(1, 0)}));
    const auto extra = pattern_from<2>(std::array{make_event(3, 4)});
    CHECK(pattern_set(a, extra, PatternSetOperation::set_union).error ==
          PatternDevelopmentError::capacity_exceeded);
    CHECK(pattern_set(a, extra, static_cast<PatternSetOperation>(99)).error ==
          PatternDevelopmentError::invalid_set_operation);
}

TEST_CASE("exact density selections are nested and anchors are mandatory",
          "[music][pattern-development]") {
    std::array<PatternEvent, 16> events{};
    for (std::size_t index = 0; index < events.size(); ++index) {
        events[index] = make_event(index + 1, static_cast<std::int64_t>(index * 100),
                                   index == 0 || index == 8 ? PatternEventRole::anchor
                                                            : PatternEventRole::primary,
                                   static_cast<std::uint16_t>(1000 - index * 20));
    }
    const auto source = pattern_from<16>(events);
    DevelopmentPattern<16> previous;
    const DensitySelection recipe{0, 0xD3517u, {{999}, 4, 12, 8}};

    CHECK(select_pattern_density(source, recipe).error ==
          PatternDevelopmentError::density_below_anchor_count);
    auto oversized = recipe;
    oversized.target_onsets = 17;
    CHECK(select_pattern_density(source, oversized).error ==
          PatternDevelopmentError::density_out_of_range);

    for (std::size_t target = 2; target <= source.size(); ++target) {
        auto selection = recipe;
        selection.target_onsets = target;
        const auto selected = select_pattern_density(source, selection);
        REQUIRE(selected);
        CHECK(selected.pattern.size() == target);
        CHECK(selected.pattern.find_id(1));
        CHECK(selected.pattern.find_id(9));
        for (std::size_t index = 0; index < previous.size(); ++index)
            CHECK(selected.pattern.find_id(previous.event(index)->id));
        CHECK(select_pattern_density(source, selection).pattern == selected.pattern);
        previous = selected.pattern;
    }

    auto exact = recipe;
    exact.target_onsets = 7;
    const auto selected = select_pattern_density(source, exact);
    REQUIRE(selected);
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto event = *source.event(index);
        if (event.role == PatternEventRole::anchor)
            continue;
        auto coordinate = exact.coordinate;
        coordinate.tick = event.onset;
        coordinate.stream ^= event.id;
        const auto priority = pulp::timebase::coordinate_random(exact.seed, coordinate);
        std::size_t rank = 0;
        for (std::size_t other_index = 0; other_index < source.size(); ++other_index) {
            const auto other = *source.event(other_index);
            if (other.role == PatternEventRole::anchor)
                continue;
            auto other_coordinate = exact.coordinate;
            other_coordinate.tick = other.onset;
            other_coordinate.stream ^= other.id;
            const auto other_priority =
                pulp::timebase::coordinate_random(exact.seed, other_coordinate);
            rank +=
                other_priority < priority || (other_priority == priority && other.id < event.id);
        }
        CHECK(static_cast<bool>(selected.pattern.find_id(event.id)) == (rank < 5));
    }
}

TEST_CASE("coordinate density restore is independent of input ordering",
          "[music][pattern-development]") {
    std::array<PatternEvent, 10> events{};
    for (std::size_t index = 0; index < events.size(); ++index)
        events[index] = make_event(index + 1, static_cast<std::int64_t>(index * 100));
    const auto original = pattern_from<10>(events);
    std::array<PatternEvent, 10> reversed{};
    for (std::size_t index = 0; index < events.size(); ++index)
        reversed[index] = events[events.size() - index - 1];
    const auto restored = pattern_from<10>(reversed);
    const DensitySelection selection{5, 91, {{0}, 2, 3, 4}};
    const auto first = select_pattern_density(original, selection);
    const auto second = select_pattern_density(restored, selection);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.pattern == second.pattern);
}

TEST_CASE("regional fills preserve anchors and every event outside the region",
          "[music][pattern-development]") {
    const auto base = pattern_from<12>(std::array{
        make_event(1, 0, PatternEventRole::anchor),
        make_event(2, 100, PatternEventRole::primary),
        make_event(3, 200, PatternEventRole::anchor),
        make_event(4, 300, PatternEventRole::ornament),
        make_event(5, 400, PatternEventRole::primary),
    });
    const auto fills = pattern_from<12>(std::array{
        make_event(6, 125, PatternEventRole::fill),
        make_event(7, 150, PatternEventRole::fill),
        make_event(8, 250, PatternEventRole::fill),
        make_event(9, 275, PatternEventRole::fill),
        make_event(10, 900, PatternEventRole::fill),
    });
    const RegionalFillSelection recipe{{100}, {300}, 4, 123, {{0}, 9, 2, 1}};
    const auto result = apply_regional_fill(base, fills, recipe);
    REQUIRE(result);
    CHECK(result.pattern.size() == 7);
    CHECK(result.pattern.find_id(1) == base.find_id(1));
    CHECK(result.pattern.find_id(3) == base.find_id(3));
    CHECK(result.pattern.find_id(4) == base.find_id(4));
    CHECK(result.pattern.find_id(5) == base.find_id(5));
    CHECK_FALSE(result.pattern.find_id(10));

    for (std::size_t target = 1; target <= 6; ++target) {
        auto selection = recipe;
        selection.target_region_onsets = target;
        const auto selected = apply_regional_fill(base, fills, selection);
        if (target == 0) {
            CHECK_FALSE(selected);
        } else {
            REQUIRE(selected);
            CHECK(selected.pattern.find_id(3));
            CHECK(selected.pattern.find_id(1));
            CHECK(selected.pattern.find_id(4));
            CHECK(selected.pattern.find_id(5));
        }
    }

    auto invalid = recipe;
    invalid.end = invalid.begin;
    CHECK(apply_regional_fill(base, fills, invalid).error ==
          PatternDevelopmentError::invalid_region);
    invalid = recipe;
    invalid.target_region_onsets = 0;
    CHECK(apply_regional_fill(base, fills, invalid).error ==
          PatternDevelopmentError::density_below_anchor_count);
    invalid.target_region_onsets = 7;
    CHECK(apply_regional_fill(base, fills, invalid).error ==
          PatternDevelopmentError::density_out_of_range);
}

TEST_CASE("regional fill conflicts and result overflow fail without partial output",
          "[music][pattern-development]") {
    const auto base =
        pattern_from<3>(std::array{make_event(1, 0), make_event(2, 10), make_event(3, 20)});
    const auto conflict = pattern_from<3>(std::array{make_event(9, 10)});
    const RegionalFillSelection region{{10}, {20}, 1, 0, {}};
    const auto conflicting = apply_regional_fill(base, conflict, region);
    CHECK(conflicting.error == PatternDevelopmentError::conflicting_event);
    CHECK(conflicting.pattern.empty());

    const auto outside =
        pattern_from<3>(std::array{make_event(4, 11), make_event(5, 12), make_event(6, 13)});
    auto full = region;
    full.target_region_onsets = 3;
    const auto overflow = apply_regional_fill(base, outside, full);
    CHECK(overflow.error == PatternDevelopmentError::capacity_exceeded);
}

TEST_CASE("candidate anchor roles do not become base preservation constraints",
          "[music][pattern-development]") {
    const DevelopmentPattern<4> empty;
    const auto candidates = pattern_from<4>(std::array{make_event(1, 10, PatternEventRole::anchor),
                                                       make_event(2, 20, PatternEventRole::fill)});
    const RegionalFillSelection zero{{0}, {30}, 0, 19, {{0}, 1, 2, 3}};
    const auto none = apply_regional_fill(empty, candidates, zero);
    REQUIRE(none);
    CHECK(none.pattern.empty());

    auto one = zero;
    one.target_region_onsets = 1;
    const auto selected = apply_regional_fill(empty, candidates, one);
    REQUIRE(selected);
    REQUIRE(selected.pattern.size() == 1);
    const auto event = *selected.pattern.event(0);
    CHECK(event == *candidates.find_id(event.id));

    const auto base = pattern_from<4>(std::array{make_event(1, 40)});
    const auto moved_same_id = pattern_from<4>(std::array{make_event(1, 10)});
    const auto conflict = apply_regional_fill(base, moved_same_id, zero);
    CHECK(conflict.error == PatternDevelopmentError::conflicting_event);
    CHECK(conflict.pattern.empty());
}

TEST_CASE("regional fills rank unions larger than the output capacity",
          "[music][pattern-development]") {
    std::array<PatternEvent, 40> base_events{};
    std::array<PatternEvent, 30> candidate_events{};
    for (std::size_t index = 0; index < base_events.size(); ++index)
        base_events[index] = make_event(index + 1, static_cast<std::int64_t>(index * 2));
    for (std::size_t index = 0; index < candidate_events.size(); ++index)
        candidate_events[index] = make_event(index + 101, static_cast<std::int64_t>(index * 2 + 1),
                                             PatternEventRole::fill);
    const auto base = pattern_from<64>(base_events);
    const auto candidates = pattern_from<64>(candidate_events);
    const auto result = apply_regional_fill(base, candidates, {{0}, {100}, 40, 71, {{0}, 5, 6, 7}});
    REQUIRE(result);
    CHECK(result.pattern.size() == 40);
}

TEST_CASE("integer A B morph preserves IDs and exact endpoints", "[music][pattern-development]") {
    const auto a = pattern_from<8>(std::array{
        make_event(1, -1000, PatternEventRole::anchor, 200),
        make_event(2, 0, PatternEventRole::primary, 400),
        make_event(3, 1000, PatternEventRole::ornament, 600),
    });
    const auto b = pattern_from<8>(std::array{
        make_event(1, 1000, PatternEventRole::anchor, 800),
        make_event(2, 500, PatternEventRole::fill, 1000),
        make_event(4, 1500, PatternEventRole::primary, 700),
    });
    const PatternMorphSelection recipe{0, 0xABu, {{700}, 8, 9, 10}};
    CHECK(morph_patterns(a, b, recipe).pattern == a);
    auto endpoint_b = recipe;
    endpoint_b.amount = 1000;
    CHECK(morph_patterns(a, b, endpoint_b).pattern == b);

    auto midpoint = recipe;
    midpoint.amount = 500;
    const auto middle = morph_patterns(a, b, midpoint);
    REQUIRE(middle);
    CHECK(middle.pattern.find_id(1)->onset == TickPosition{0});
    CHECK(middle.pattern.find_id(1)->accent == 500);
    CHECK(middle.pattern.find_id(2)->onset == TickPosition{250});
    CHECK(middle.pattern.find_id(2)->accent == 700);
    CHECK(middle.pattern.find_id(2)->role == PatternEventRole::fill);
    CHECK(morph_patterns(a, b, midpoint).pattern == middle.pattern);

    auto invalid = recipe;
    invalid.amount = 1001;
    CHECK(morph_patterns(a, b, invalid).error ==
          PatternDevelopmentError::morph_amount_out_of_range);
}

TEST_CASE("morph selection is monotonic per unique stable event ID",
          "[music][pattern-development]") {
    std::array<PatternEvent, 16> a_events{};
    std::array<PatternEvent, 16> b_events{};
    for (std::size_t index = 0; index < 16; ++index) {
        a_events[index] = make_event(index + 1, static_cast<std::int64_t>(index));
        b_events[index] = make_event(index + 101, static_cast<std::int64_t>(index + 32));
    }
    const auto a = pattern_from<32>(a_events);
    const auto b = pattern_from<32>(b_events);
    std::array<bool, 16> previous_a{};
    std::array<bool, 16> previous_b{};
    previous_a.fill(true);
    for (std::uint16_t amount = 0; amount <= 1000; ++amount) {
        const auto result = morph_patterns(a, b, {amount, 77, {{0}, 3, 5, 7}});
        REQUIRE(result);
        for (std::size_t index = 0; index < 16; ++index) {
            const bool has_a = static_cast<bool>(result.pattern.find_id(index + 1));
            const bool has_b = static_cast<bool>(result.pattern.find_id(index + 101));
            CHECK_FALSE((has_a && !previous_a[index]));
            CHECK_FALSE((previous_b[index] && !has_b));
            previous_a[index] = has_a;
            previous_b[index] = has_b;
        }
    }
}

TEST_CASE("morph integer interpolation covers signed extremes and collisions",
          "[music][pattern-development]") {
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto a = pattern_from<2>(std::array{make_event(1, minimum)});
    const auto b = pattern_from<2>(std::array{make_event(1, maximum)});
    CHECK(morph_patterns(a, b, {0, 0, {}}).pattern == a);
    CHECK(morph_patterns(a, b, {1000, 0, {}}).pattern == b);
    CHECK(morph_patterns(a, b, {500, 0, {}}).pattern.find_id(1)->onset.value == -1);

    const auto collision_a = pattern_from<2>(std::array{make_event(1, 0), make_event(2, 2)});
    const auto collision_b = pattern_from<2>(std::array{make_event(1, 2), make_event(2, 0)});
    const auto collision = morph_patterns(collision_a, collision_b, {500, 0, {}});
    CHECK(collision.error == PatternDevelopmentError::duplicate_onset);
    CHECK(collision.pattern.empty());
    CHECK(collision_a.size() == 2);
    CHECK(collision_b.size() == 2);
}

TEST_CASE("all pattern development operations allocate no memory",
          "[music][pattern-development][rt-safety]") {
    const auto a = pattern_from<64>(std::array{
        make_event(1, 0, PatternEventRole::anchor),
        make_event(2, 100),
        make_event(3, 200),
        make_event(4, 300),
        make_event(5, 400),
        make_event(6, 500),
        make_event(7, 600),
        make_event(8, 700),
    });
    const auto b = pattern_from<64>(std::array{
        make_event(1, 50, PatternEventRole::anchor),
        make_event(2, 150),
        make_event(9, 250),
        make_event(10, 350),
        make_event(11, 450),
    });

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        const auto density = select_pattern_density(a, {5, 1, {{0}, 2, 3, 4}});
        const auto fill = apply_regional_fill(a, b, {{200}, {500}, 4, 2, {{0}, 2, 3, 5}});
        const auto morph = morph_patterns(a, b, {333, 3, {{0}, 2, 3, 6}});
        const auto set = pattern_set(a, b, PatternSetOperation::intersection);
        REQUIRE(density);
        REQUIRE(fill);
        REQUIRE(morph);
        REQUIRE(set);
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
}
