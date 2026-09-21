#include <pulp/music/detail/random_range.hpp>
#include <pulp/music/music.hpp>
#include <pulp/timebase/coordinate_random.hpp>
#include <pulp/timebase/groove_kernel.hpp>
#include <pulp/timebase/quantize.hpp>
#include <pulp/timebase/trigger_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

using namespace pulp::music;

namespace {

struct TestCoordinate {
    std::int64_t cycle = 0;
    std::uint32_t step = 0;
    std::uint32_t lane = 0;
};

// Test-only mixer supplies coordinate-stable draws without creating a second
// production randomness contract in the music module.
constexpr std::uint64_t test_mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

constexpr std::uint64_t test_draw(std::uint64_t seed,
                                  TestCoordinate coordinate) noexcept {
    auto value = test_mix(seed);
    value = test_mix(value ^ static_cast<std::uint64_t>(coordinate.cycle));
    return test_mix(value ^ (static_cast<std::uint64_t>(coordinate.step) << 32u)
                    ^ coordinate.lane);
}

// Independent division oracle for ceil(bucket * 2^64 / limit). Tests only use
// small limits, so the remainder product cannot overflow uint64_t.
constexpr std::uint64_t oracle_bucket_lower(std::uint64_t bucket,
                                            std::uint64_t limit) noexcept {
    if (bucket == 0 || limit == 1)
        return 0;
    auto quotient = std::numeric_limits<std::uint64_t>::max() / limit;
    auto remainder = std::numeric_limits<std::uint64_t>::max() % limit + 1;
    if (remainder == limit) {
        ++quotient;
        remainder = 0;
    }
    const auto remainder_product = bucket * remainder;
    return bucket * quotient + remainder_product / limit
           + static_cast<std::uint64_t>(remainder_product % limit != 0);
}

std::uint64_t oracle_reduce(std::uint64_t word, std::uint64_t limit) {
    for (std::uint64_t bucket = 1; bucket < limit; ++bucket)
        if (word < oracle_bucket_lower(bucket, limit))
            return bucket - 1;
    return limit - 1;
}

std::size_t cyclic_gap_spread(std::uint64_t mask, std::size_t steps) {
    std::vector<std::size_t> onsets;
    for (std::size_t step = 0; step < steps; ++step)
        if (((mask >> step) & 1u) != 0)
            onsets.push_back(step);
    if (onsets.size() < 2)
        return 0;

    std::size_t minimum = steps;
    std::size_t maximum = 0;
    for (std::size_t index = 0; index < onsets.size(); ++index) {
        const auto next = onsets[(index + 1) % onsets.size()];
        const auto distance = next > onsets[index] ? next - onsets[index]
                                                    : steps - onsets[index] + next;
        minimum = std::min(minimum, distance);
        maximum = std::max(maximum, distance);
    }
    return maximum - minimum;
}

std::uint64_t cyclic_balance_cost(std::uint64_t mask, std::size_t steps) {
    std::uint64_t cost = 0;
    for (std::size_t width = 1; width <= steps; ++width) {
        std::size_t minimum = steps;
        std::size_t maximum = 0;
        for (std::size_t start = 0; start < steps; ++start) {
            std::size_t count = 0;
            for (std::size_t offset = 0; offset < width; ++offset)
                count += (mask >> ((start + offset) % steps)) & 1u;
            minimum = std::min(minimum, count);
            maximum = std::max(maximum, count);
        }
        cost = cost * (steps + 1) + (maximum - minimum);
    }
    return cost;
}

template <std::size_t Capacity>
std::uint64_t pattern_mask(const BinaryPattern<Capacity>& pattern) {
    std::uint64_t mask = 0;
    for (std::size_t step = 0; step < pattern.size(); ++step)
        if (*pattern.at(step))
            mask |= std::uint64_t{1} << step;
    return mask;
}

template <std::size_t Capacity>
std::vector<bool> oracle_source_projection(const BinaryPattern<Capacity>& source,
                                           std::size_t target_steps,
                                           RhythmLengthMapping mapping,
                                           std::int64_t phase) {
    std::vector<bool> projection(target_steps, false);
    if (mapping == RhythmLengthMapping::wrap) {
        for (std::size_t target = 0; target < target_steps; ++target)
            projection[target] = *source.at(target % source.size());
    } else {
        // Fill each source bucket by its independently derived half-open target
        // interval [ceil(i*T/S), ceil((i+1)*T/S)). Test dimensions are <= 8,
        // so these reference products are exactly representable.
        for (std::size_t source_step = 0; source_step < source.size(); ++source_step) {
            const auto first = (source_step * target_steps + source.size() - 1)
                               / source.size();
            const auto last = ((source_step + 1) * target_steps + source.size() - 1)
                              / source.size();
            for (auto target = first; target < last; ++target)
                projection[target] = *source.at(source_step);
        }
    }

    while (phase > 0) {
        std::rotate(projection.rbegin(), projection.rbegin() + 1, projection.rend());
        --phase;
    }
    while (phase < 0) {
        std::rotate(projection.begin(), projection.begin() + 1, projection.end());
        ++phase;
    }
    return projection;
}

} // namespace

TEST_CASE("random-word range reduction matches independent bucket boundaries",
          "[music][generative]") {
    using pulp::music::detail::reduce_random_word;
    CHECK(reduce_random_word(0, 1) == 0);
    CHECK(reduce_random_word(std::numeric_limits<std::uint64_t>::max(), 1) == 0);
    for (std::uint64_t limit = 2; limit <= 257; ++limit) {
        for (std::uint64_t bucket = 0; bucket < limit; ++bucket) {
            const auto lower = oracle_bucket_lower(bucket, limit);
            const auto upper = bucket + 1 == limit
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : oracle_bucket_lower(bucket + 1, limit) - 1;
            CHECK(reduce_random_word(lower, limit) == bucket);
            CHECK(reduce_random_word(upper, limit) == bucket);
        }
        for (const auto word : {std::uint64_t{0}, std::uint64_t{1},
                                std::uint64_t{0x0123'4567'89AB'CDEFull},
                                std::uint64_t{0xFEDC'BA98'7654'3210ull},
                                std::numeric_limits<std::uint64_t>::max()})
            CHECK(reduce_random_word(word, limit) == oracle_reduce(word, limit));
    }
}

TEST_CASE("Euclidean patterns match an independent combinatorial oracle",
          "[music][generative]") {
    for (std::size_t steps = 1; steps <= 12; ++steps) {
        std::array<std::uint64_t, 13> best_balance{};
        best_balance.fill(std::numeric_limits<std::uint64_t>::max());
        for (std::uint64_t mask = 0; mask < (std::uint64_t{1} << steps); ++mask) {
            const auto pulses = static_cast<std::size_t>(std::popcount(mask));
            best_balance[pulses] = std::min(best_balance[pulses],
                                            cyclic_balance_cost(mask, steps));
        }
        for (std::size_t pulses = 0; pulses <= steps; ++pulses) {
            const auto result = euclidean_pattern(steps, pulses);
            REQUIRE(result);
            CHECK(result.pattern.size() == steps);
            CHECK(result.pattern.onset_count() == pulses);
            const auto mask = pattern_mask(result.pattern);
            CHECK(cyclic_balance_cost(mask, steps) == best_balance[pulses]);
            CHECK(cyclic_gap_spread(mask, steps) <= 1);
            if (pulses > 0)
                CHECK(*result.pattern.at(0));
        }
    }

    const auto classic = euclidean_pattern(8, 3);
    REQUIRE(classic);
    CHECK(pattern_mask(classic.pattern) == 0x49u);
    const auto two_of_five = euclidean_pattern(5, 2);
    REQUIRE(two_of_five);
    CHECK(pattern_mask(two_of_five.pattern) == 0x09u);
    CHECK(euclidean_pattern(0, 0).error == PatternError::empty_pattern);
    CHECK(euclidean_pattern<8>(9, 1).error == PatternError::capacity_exceeded);
    CHECK(euclidean_pattern(8, 9).error == PatternError::pulses_exceed_steps);
}

TEST_CASE("Euclidean rotation has an explicit signed delay convention",
          "[music][generative]") {
    const auto canonical = euclidean_pattern(8, 3);
    const auto delayed = euclidean_pattern(8, 3, 1);
    const auto advanced = euclidean_pattern(8, 3, -1);
    REQUIRE(canonical);
    REQUIRE(delayed);
    REQUIRE(advanced);
    CHECK(pattern_mask(canonical.pattern) == 0x49u); // 10010010
    CHECK(pattern_mask(delayed.pattern) == 0x92u);   // 01001001
    CHECK(pattern_mask(advanced.pattern) == 0xA4u);  // 00100101

    CHECK(euclidean_pattern(8, 3, 9).pattern == delayed.pattern);
    CHECK(euclidean_pattern(8, 3, -9).pattern == advanced.pattern);
    CHECK(euclidean_pattern(8, 3, std::numeric_limits<std::int64_t>::min()).pattern
          == canonical.pattern);
}

TEST_CASE("Euclidean recipes materialize a versioned fixed-capacity contract",
          "[music][generative]") {
    constexpr EuclideanPatternRecipe recipe{euclidean_pattern_recipe_version, 8, 3, -1};
    STATIC_REQUIRE(std::is_trivially_copyable_v<EuclideanPatternRecipe>);
    STATIC_REQUIRE(recipe.steps == 8);
    const auto materialized = materialize_pattern<8>(recipe);
    REQUIRE(materialized);
    CHECK(pattern_mask(materialized.pattern) == 0xA4u);

    auto unsupported = recipe;
    unsupported.version = euclidean_pattern_recipe_version + 1;
    CHECK(materialize_pattern<8>(unsupported).error
          == PatternError::unsupported_recipe_version);
    auto oversized = recipe;
    oversized.steps = 9;
    CHECK(materialize_pattern<8>(oversized).error == PatternError::capacity_exceeded);
    auto impossible = recipe;
    impossible.pulses = 9;
    CHECK(materialize_pattern<8>(impossible).error
          == PatternError::pulses_exceed_steps);
}

TEST_CASE("rhythm relationships match independent phase and length oracles",
          "[music][generative]") {
    for (std::size_t source_steps = 1; source_steps <= 7; ++source_steps) {
        for (std::uint64_t source_mask = 0;
             source_mask < (std::uint64_t{1} << source_steps); ++source_mask) {
            BinaryPattern<8> source;
            REQUIRE(source.resize(source_steps) == PatternError::none);
            for (std::size_t step = 0; step < source_steps; ++step)
                REQUIRE(source.set(step, ((source_mask >> step) & 1u) != 0));

            for (std::size_t target_steps = 1; target_steps <= 8; ++target_steps) {
                for (const auto mapping : {RhythmLengthMapping::wrap,
                                           RhythmLengthMapping::proportional}) {
                    for (const auto phase : {-9, -1, 0, 1, 9}) {
                        RhythmRelationshipConfig config;
                        config.target_steps = target_steps;
                        config.relationship = RhythmRelationship::coincident;
                        config.length_mapping = mapping;
                        config.phase_steps = phase;
                        config.collision = RhythmCollisionPolicy::allow_source_overlap;
                        const auto expected = oracle_source_projection(
                            source, target_steps, mapping, phase);
                        const auto result = derive_rhythm_relationship(source, config);
                        REQUIRE(result);
                        for (std::size_t step = 0; step < target_steps; ++step)
                            CHECK(*result.pattern.at(step) == expected[step]);
                    }
                }
            }
        }
    }
}

TEST_CASE("rhythm relationship policies have explicit semantic fixtures",
          "[music][generative]") {
    BinaryPattern<8> source;
    constexpr std::array<std::uint8_t, 3> source_bits{1, 0, 1};
    REQUIRE(source.assign(source_bits) == PatternError::none);

    struct Fixture {
        RhythmLengthMapping mapping;
        std::int64_t phase;
        RhythmRelationship relationship;
        RhythmCollisionPolicy collision;
        std::uint64_t expected;
    };
    constexpr std::array fixtures{
        Fixture{RhythmLengthMapping::wrap, 0, RhythmRelationship::coincident,
                RhythmCollisionPolicy::allow_source_overlap, 0x6Du},
        Fixture{RhythmLengthMapping::proportional, 0, RhythmRelationship::coincident,
                RhythmCollisionPolicy::allow_source_overlap, 0xC7u},
        Fixture{RhythmLengthMapping::wrap, 1, RhythmRelationship::coincident,
                RhythmCollisionPolicy::allow_source_overlap, 0xDAu},
        Fixture{RhythmLengthMapping::wrap, -1, RhythmRelationship::coincident,
                RhythmCollisionPolicy::allow_source_overlap, 0xB6u},
        Fixture{RhythmLengthMapping::proportional, 1, RhythmRelationship::coincident,
                RhythmCollisionPolicy::allow_source_overlap, 0x8Fu},
        Fixture{RhythmLengthMapping::proportional, -1, RhythmRelationship::coincident,
                RhythmCollisionPolicy::allow_source_overlap, 0xE3u},
        Fixture{RhythmLengthMapping::wrap, 0, RhythmRelationship::complementary,
                RhythmCollisionPolicy::allow_source_overlap, 0x92u},
        Fixture{RhythmLengthMapping::wrap, 0, RhythmRelationship::independent,
                RhythmCollisionPolicy::allow_source_overlap, 0xFFu},
        Fixture{RhythmLengthMapping::wrap, 0, RhythmRelationship::coincident,
                RhythmCollisionPolicy::avoid_source_overlap, 0x00u},
        Fixture{RhythmLengthMapping::wrap, 0, RhythmRelationship::complementary,
                RhythmCollisionPolicy::avoid_source_overlap, 0x92u},
        Fixture{RhythmLengthMapping::wrap, 0, RhythmRelationship::independent,
                RhythmCollisionPolicy::avoid_source_overlap, 0x92u},
    };

    for (const auto& fixture : fixtures) {
        RhythmRelationshipConfig config;
        config.target_steps = 8;
        config.relationship = fixture.relationship;
        config.length_mapping = fixture.mapping;
        config.phase_steps = fixture.phase;
        config.collision = fixture.collision;
        const auto result = derive_rhythm_relationship(source, config);
        REQUIRE(result);
        CHECK(pattern_mask(result.pattern) == fixture.expected);
    }

    using pulp::music::detail::delayed_step;
    using pulp::music::detail::multiply_divide_floor;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    STATIC_REQUIRE(multiply_divide_floor(maximum - 1, maximum, maximum)
                   == maximum - 1);
    STATIC_REQUIRE(multiply_divide_floor(maximum / 2, maximum, maximum)
                   == maximum / 2);
    STATIC_REQUIRE(delayed_step(maximum - 2, maximum,
                                std::numeric_limits<std::int64_t>::min())
                   == (std::uint64_t{1} << 63u) - 2);
    STATIC_REQUIRE(delayed_step(0, maximum, 1) == maximum - 1);
}

TEST_CASE("rhythm density is exact and coordinate deterministic",
          "[music][generative]") {
    BinaryPattern<16> source;
    constexpr std::array<std::uint8_t, 8> source_bits{1, 0, 0, 1, 0, 0, 1, 0};
    REQUIRE(source.assign(source_bits) == PatternError::none);

    RhythmRelationshipConfig config;
    config.target_steps = 16;
    config.relationship = RhythmRelationship::independent;
    config.length_mapping = RhythmLengthMapping::proportional;
    config.collision = RhythmCollisionPolicy::avoid_source_overlap;
    config.density = RhythmDensityPolicy::exact_onsets;
    config.target_onsets = 5;
    config.draw = {.seed = 0xA11CEu, .cycle = -3, .lane = 4};

    const auto first = derive_rhythm_relationship(source, config);
    const auto repeated = derive_rhythm_relationship(source, config);
    REQUIRE(first);
    REQUIRE(repeated);
    CHECK(first.pattern == repeated.pattern);
    CHECK(first.pattern.onset_count() == config.target_onsets);
    const auto projected_source = oracle_source_projection(
        source, config.target_steps, config.length_mapping, config.phase_steps);
    for (std::size_t step = 0; step < config.target_steps; ++step) {
        if (*first.pattern.at(step))
            CHECK_FALSE(projected_source[step]);
    }

    config.draw.seed += 1;
    const auto different_seed = derive_rhythm_relationship(source, config);
    REQUIRE(different_seed);
    CHECK(different_seed.pattern.onset_count() == config.target_onsets);
    CHECK(different_seed.pattern != first.pattern);

    config.target_onsets = 11;
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::insufficient_candidates);
}

TEST_CASE("rhythm relationship validation fails closed", "[music][generative]") {
    BinaryPattern<4> empty;
    RhythmRelationshipConfig config;
    CHECK(derive_rhythm_relationship(empty, config).error
          == RhythmRelationshipError::empty_source);

    BinaryPattern<4> source;
    REQUIRE(source.resize(4, true) == PatternError::none);
    STATIC_REQUIRE(std::is_trivially_copyable_v<RhythmRelationshipResult<4>>);
    STATIC_REQUIRE(noexcept(derive_rhythm_relationship(source, config)));
    config.target_steps = 0;
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::empty_target);
    config.target_steps = 5;
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::capacity_exceeded);
    config.target_steps = 4;
    config.relationship = static_cast<RhythmRelationship>(99);
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::invalid_relationship);
    config.relationship = RhythmRelationship::coincident;
    config.length_mapping = static_cast<RhythmLengthMapping>(99);
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::invalid_length_mapping);
    config.length_mapping = RhythmLengthMapping::wrap;
    config.collision = static_cast<RhythmCollisionPolicy>(99);
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::invalid_collision_policy);
    config.collision = RhythmCollisionPolicy::allow_source_overlap;
    config.density = static_cast<RhythmDensityPolicy>(99);
    CHECK(derive_rhythm_relationship(source, config).error
          == RhythmRelationshipError::invalid_density_policy);
}

TEST_CASE("binary patterns fail closed at their fixed capacity", "[music][generative]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<BinaryPattern<64>>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<PatternWalker<64>>);
    STATIC_REQUIRE(noexcept(euclidean_pattern(8, 3)));
    STATIC_REQUIRE(noexcept(cellular_evolve(BinaryPattern<64>{}, 30)));
    STATIC_REQUIRE(noexcept(looping_shift_register(
        BinaryPattern<64>{}, MutationChance{}, 0)));
    BinaryPattern<4> pattern;
    constexpr std::array<std::uint8_t, 4> source{0, 2, 0, 1};
    CHECK(pattern.assign(source) == PatternError::none);
    CHECK(pattern.size() == 4);
    CHECK(pattern.onset_count() == 2);
    CHECK_FALSE(pattern.at(4));
    CHECK_FALSE(pattern.set(4, true));

    constexpr std::array<std::uint8_t, 5> oversized{};
    CHECK(pattern.assign(oversized) == PatternError::capacity_exceeded);
    CHECK(pattern.size() == 4);
    CHECK(pattern.resize(5) == PatternError::capacity_exceeded);
    CHECK(pattern.size() == 4);
}

TEST_CASE("pattern walkers expose modes and reset semantics", "[music][generative]") {
    PatternWalker<8> walker;
    CHECK(walker.configure(0, PatternWalkerMode::forward) == PatternError::empty_pattern);
    CHECK_FALSE(walker.next());
    CHECK(walker.configure(9, PatternWalkerMode::forward)
          == PatternError::capacity_exceeded);
    CHECK(walker.configure(4, static_cast<PatternWalkerMode>(99))
          == PatternError::invalid_mode);

    const auto collect = [&walker](PatternWalkerMode mode) {
        std::array<std::size_t, 8> values{};
        REQUIRE(walker.configure(4, mode) == PatternError::none);
        for (auto& value : values)
            value = *walker.next();
        walker.reset();
        CHECK(*walker.next() == values.front());
        return values;
    };
    CHECK(collect(PatternWalkerMode::forward)
          == std::array<std::size_t, 8>{0, 1, 2, 3, 0, 1, 2, 3});
    CHECK(collect(PatternWalkerMode::reverse)
          == std::array<std::size_t, 8>{3, 2, 1, 0, 3, 2, 1, 0});
    CHECK(collect(PatternWalkerMode::ping_pong)
          == std::array<std::size_t, 8>{0, 1, 2, 3, 2, 1, 0, 1});

    REQUIRE(walker.configure(7, PatternWalkerMode::random)
            == PatternError::none);
    CHECK_FALSE(walker.next());
    const auto draw = test_draw(99, {-4, 17, 8});
    const auto first = walker.next(draw);
    walker.reset();
    CHECK(walker.next(draw) == first);
    CHECK(*first < 7);
}

TEST_CASE("Markov preparation validates fixed-capacity transition tables",
          "[music][generative]") {
    PreparedMarkovModel<3> model;
    const std::array<std::uint32_t, 0> empty{};
    CHECK(model.prepare(0, empty) == MarkovError::empty_model);
    CHECK(model.state_count() == 0);
    CHECK(model.prepare(4, empty) == MarkovError::capacity_exceeded);
    constexpr std::array bad_shape{1u, 2u, 3u};
    CHECK(model.prepare(2, bad_shape) == MarkovError::weight_count_mismatch);
    constexpr std::array empty_row{1u, 0u, 0u, 0u};
    CHECK(model.prepare(2, empty_row) == MarkovError::empty_transition_row);
    CHECK(model.state_count() == 0);

    constexpr std::array weights{1u, 3u, 4u, 0u};
    REQUIRE(model.prepare(2, weights) == MarkovError::none);
    CHECK(model.state_count() == 2);
    CHECK_FALSE(model.next(2, 1));
    for (std::uint32_t step = 0; step < 64; ++step)
        CHECK(model.next(1, test_draw(44, {0, step, 0})) == 0);
}

TEST_CASE("Markov evaluation is deterministic and follows prepared weights",
          "[music][generative]") {
    PreparedMarkovModel<2> model;
    constexpr std::array weights{1u, 3u, 1u, 1u};
    REQUIRE(model.prepare(2, weights) == MarkovError::none);
    STATIC_REQUIRE(std::is_trivially_copyable_v<PreparedMarkovModel<2>>);
    STATIC_REQUIRE(noexcept(model.next(0, 0)));

    std::array<std::size_t, 128> whole{};
    std::array<std::size_t, 128> split{};
    std::size_t whole_state = 0;
    for (std::uint32_t step = 0; step < whole.size(); ++step) {
        whole_state = *model.next(whole_state, test_draw(1234, {9, step, 1}));
        whole[step] = whole_state;
    }
    std::size_t split_state = 0;
    for (std::uint32_t begin : {0u, 7u, 31u, 96u}) {
        const std::uint32_t end = begin == 0 ? 7 : begin == 7 ? 31 : begin == 31 ? 96 : 128;
        for (auto step = begin; step < end; ++step) {
            split_state = *model.next(split_state, test_draw(1234, {9, step, 1}));
            split[step] = split_state;
        }
    }
    CHECK(split == whole);

    std::size_t state_one = 0;
    constexpr std::size_t trials = 4096;
    for (std::uint32_t step = 0; step < trials; ++step)
        state_one += *model.next(0, test_draw(0xA11CEu, {0, step, 0})) == 1;
    CHECK(state_one > 2800);
    CHECK(state_one < 3350);
}

TEST_CASE("Markov selection matches every exact bucket for small totals",
          "[music][generative]") {
    for (std::size_t states = 1; states <= 16; ++states) {
        std::array<std::uint32_t, 16 * 16> weights{};
        for (std::size_t row = 0; row < states; ++row)
            for (std::size_t column = 0; column < states; ++column)
                weights[row * states + column] = 1;
        PreparedMarkovModel<16> model;
        REQUIRE(model.prepare(states, std::span(weights).first(states * states))
                == MarkovError::none);
        for (std::size_t bucket = 0; bucket < states; ++bucket) {
            const auto lower = oracle_bucket_lower(bucket, states);
            const auto upper = bucket + 1 == states
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : oracle_bucket_lower(bucket + 1, states) - 1;
            CHECK(model.next(0, lower) == bucket);
            CHECK(model.next(0, upper) == bucket);
        }
    }

    constexpr std::array weighted_rows{
        1u, 3u, 2u,
        1u, 3u, 2u,
        1u, 3u, 2u,
    };
    constexpr std::array<std::size_t, 6> expected{0, 1, 1, 1, 2, 2};
    PreparedMarkovModel<3> weighted;
    REQUIRE(weighted.prepare(3, weighted_rows) == MarkovError::none);
    for (std::size_t bucket = 0; bucket < expected.size(); ++bucket) {
        const auto lower = oracle_bucket_lower(bucket, expected.size());
        const auto upper = bucket + 1 == expected.size()
                               ? std::numeric_limits<std::uint64_t>::max()
                               : oracle_bucket_lower(bucket + 1, expected.size()) - 1;
        CHECK(weighted.next(0, lower) == expected[bucket]);
        CHECK(weighted.next(0, upper) == expected[bucket]);
    }
}

TEST_CASE("cellular evolution applies elementary rules with explicit boundaries",
          "[music][generative]") {
    BinaryPattern<8> seed;
    constexpr std::array<std::uint8_t, 5> single{0, 0, 1, 0, 0};
    REQUIRE(seed.assign(single) == PatternError::none);
    const auto rule30 = cellular_evolve(seed, 30, CellularBoundary::fixed_off);
    REQUIRE(rule30);
    CHECK(pattern_mask(rule30.pattern) == 0x0Eu);

    BinaryPattern<8> edge;
    constexpr std::array<std::uint8_t, 5> at_edge{1, 0, 0, 0, 0};
    REQUIRE(edge.assign(at_edge) == PatternError::none);
    const auto fixed = cellular_evolve(edge, 90, CellularBoundary::fixed_off);
    const auto wrapped = cellular_evolve(edge, 90, CellularBoundary::wrap);
    REQUIRE(fixed);
    REQUIRE(wrapped);
    CHECK(pattern_mask(fixed.pattern) == 0x02u);
    CHECK(pattern_mask(wrapped.pattern) == 0x12u);
    CHECK(cellular_evolve(BinaryPattern<8>{}, 30).error == PatternError::empty_pattern);
    CHECK(cellular_evolve(seed, 30, static_cast<CellularBoundary>(99)).error
          == PatternError::invalid_mode);
}

TEST_CASE("looping shift registers rotate and mutate from caller-supplied draws",
          "[music][generative]") {
    BinaryPattern<8> input;
    constexpr std::array<std::uint8_t, 4> bits{1, 0, 1, 1};
    REQUIRE(input.assign(bits) == PatternError::none);

    const auto rotated = looping_shift_register(input, {0, 1}, test_draw(77, {2, 3, 4}));
    REQUIRE(rotated);
    CHECK(rotated.output);
    CHECK_FALSE(rotated.mutated);
    CHECK(pattern_mask(rotated.pattern) == 0x0Bu);

    const auto inverted = looping_shift_register(input, {1, 1}, test_draw(77, {2, 3, 4}));
    REQUIRE(inverted);
    CHECK(inverted.output);
    CHECK(inverted.mutated);
    CHECK(pattern_mask(inverted.pattern) == 0x0Au);

    const auto seeded_a = looping_shift_register(input, {1, 3},
                                                  test_draw(77, {-2, 9, 1}));
    const auto seeded_b = looping_shift_register(input, {1, 3},
                                                  test_draw(77, {-2, 9, 1}));
    CHECK(seeded_a.pattern == seeded_b.pattern);
    CHECK(seeded_a.mutated == seeded_b.mutated);
    CHECK(looping_shift_register(input, {2, 1}, 0).error
          == PatternError::invalid_probability);
    CHECK(looping_shift_register(input, {0, 0}, 0).error
          == PatternError::invalid_probability);
    CHECK(looping_shift_register(BinaryPattern<8>{}, {0, 1}, 0).error
          == PatternError::empty_pattern);
}

namespace {

namespace tb = pulp::timebase;

// One bar of 4/4 on the shared tick resolution, divided into sixteenths.
constexpr std::int64_t kSixteenth = tb::kTicksPerQuarter / 4;
constexpr std::int64_t kEighth = tb::kTicksPerQuarter / 2;
constexpr std::int64_t kBar = tb::kTicksPerQuarter * 4;

// The per-lane displacements this worked example authors, in ticks. At 120 BPM
// a sixteenth lasts 125 ms, so these are 6.25 ms early and 12.5 ms late.
constexpr std::int64_t kSnareEarly = -kSixteenth / 20;
constexpr std::int64_t kBassLate = kSixteenth / 10;

constexpr std::size_t kHatTrack = 0;
constexpr std::size_t kSnareTrack = 1;
constexpr std::size_t kBassTrack = 2;

std::vector<std::int64_t> positions_on_track(const std::vector<tb::TriggerEvent>& events,
                                             std::size_t track) {
    std::vector<std::int64_t> result;
    for (const auto& event : events)
        if (event.track == track)
            result.push_back(event.position.value);
    return result;
}

} // namespace

TEST_CASE("sixteenth lanes take an exact per-lane feel from authored microtiming",
          "[music][timebase][rhythm]") {
    // A five-step source cycled across a sixteen-step bar is a 5-over-4
    // polymeter: the source repeats every five sixteenths, the bar does not,
    // and the two only realign after five bars.
    const auto source = euclidean_pattern<16>(5, 2);
    REQUIRE(source);
    REQUIRE(source.pattern.size() == 5);
    REQUIRE(source.pattern.onset_count() == 2);
    CHECK(*source.pattern.at(0));
    CHECK(*source.pattern.at(3));

    RhythmRelationshipConfig config;
    config.target_steps = 16;
    config.relationship = RhythmRelationship::coincident;
    config.length_mapping = RhythmLengthMapping::wrap;
    const auto bass_lane = derive_rhythm_relationship(source.pattern, config);
    REQUIRE(bass_lane);

    // Wrap mapping reads source step (target_step % 5), so the two source
    // onsets reappear wherever that residue is 0 or 3.
    std::vector<std::size_t> bass_steps;
    for (std::size_t step = 0; step < 16; ++step)
        if (*bass_lane.pattern.at(step))
            bass_steps.push_back(step);
    CHECK(bass_steps == std::vector<std::size_t>{0, 3, 5, 8, 10, 13, 15});

    tb::TriggerGrid<4, 16> grid;
    REQUIRE(grid.configure(3, 16, tb::TickDuration{kSixteenth}) == tb::TriggerGridError::None);

    // A cell may not move more than half a step, which is what keeps adjacent
    // steps from swapping; both authored offsets sit well inside that bound.
    CHECK(grid.minimum_microtiming().value == -kSixteenth / 2);
    CHECK(grid.maximum_microtiming().value == (kSixteenth - 1) / 2);
    REQUIRE(kSnareEarly > grid.minimum_microtiming().value);
    REQUIRE(kBassLate < grid.maximum_microtiming().value);

    for (std::size_t step = 0; step < 16; ++step)
        REQUIRE(grid.set_cell(kHatTrack, step,
                              tb::TriggerCell{true, 90, {1, 1}, tb::TickDuration{0}}) ==
                tb::TriggerGridError::None);
    for (const std::size_t step : {std::size_t{4}, std::size_t{12}})
        REQUIRE(grid.set_cell(kSnareTrack, step,
                              tb::TriggerCell{true, 110, {1, 1}, tb::TickDuration{kSnareEarly}}) ==
                tb::TriggerGridError::None);
    for (const std::size_t step : bass_steps)
        REQUIRE(grid.set_cell(kBassTrack, step,
                              tb::TriggerCell{true, 100, {1, 1}, tb::TickDuration{kBassLate}}) ==
                tb::TriggerGridError::None);

    // Every cell here is authored at probability 1/1, so the draws decide
    // nothing; the projection still requires exactly one word per coordinate.
    const std::vector<std::uint64_t> draws(grid.coordinate_count(), 0);
    std::vector<tb::TriggerEvent> events(grid.coordinate_count());
    const auto projected = grid.project_window(tb::TickPosition{0}, tb::TickPosition{0},
                                               tb::TickPosition{kBar}, draws, events);
    REQUIRE(projected);
    REQUIRE(projected.event_count == 16 + 2 + 7);
    events.resize(projected.event_count);

    // Each lane lands exactly where the authored offset puts it. Nothing here
    // rounds: the displacement is an integer tick added to an integer grid.
    for (const auto& event : events) {
        const auto grid_tick = static_cast<std::int64_t>(event.step) * kSixteenth;
        if (event.track == kHatTrack)
            CHECK(event.position.value == grid_tick);
        else if (event.track == kSnareTrack)
            CHECK(event.position.value == grid_tick + kSnareEarly);
        else
            CHECK(event.position.value == grid_tick + kBassLate);
    }

    // The same landings stated as absolute ticks, derived by hand from the
    // grid: sixteenth 4 is 705600, and the snare sits 8820 ticks before it.
    CHECK(positions_on_track(events, kHatTrack).front() == 0);
    CHECK(positions_on_track(events, kHatTrack).back() == 15 * kSixteenth);
    CHECK(positions_on_track(events, kSnareTrack) == std::vector<std::int64_t>{696780, 2107980});
    CHECK(positions_on_track(events, kBassTrack) ==
          std::vector<std::int64_t>{17640, 546840, 899640, 1428840, 1781640, 2310840, 2663640});

    // Emission is step-major then track-major, which is deliberately not
    // position order. At step 4 the on-grid hat is emitted before the snare
    // that sounds 8820 ticks earlier, so a consumer that needs chronological
    // order sorts the window itself.
    const auto hat_at_four = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.step == 4 && event.track == kHatTrack;
    });
    const auto snare_at_four = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.step == 4 && event.track == kSnareTrack;
    });
    REQUIRE(hat_at_four != events.end());
    REQUIRE(snare_at_four != events.end());
    CHECK(hat_at_four < snare_at_four);
    CHECK(snare_at_four->position < hat_at_four->position);
}

TEST_CASE("triplet swing quantizes straight sixteenths and round-trips within one tick",
          "[timebase][rhythm]") {
    constexpr tb::TickDuration grid{kEighth};
    REQUIRE(tb::valid_swing_grid(grid));
    REQUIRE(tb::valid_swing_ratio(tb::kTripletSwing));

    // Straight sixteenths pushed onto a triplet eighth feel: the beat's four
    // sixteenths land on 0, 1/3, 2/3 and 5/6 of the quarter.
    struct Landing {
        std::int64_t authored;
        std::int64_t swung;
    };
    const std::array<Landing, 5> landings{{
        {0, 0},
        {kSixteenth, tb::kTicksPerQuarter / 3},
        {kEighth, 2 * tb::kTicksPerQuarter / 3},
        {3 * kSixteenth, 5 * tb::kTicksPerQuarter / 6},
        {tb::kTicksPerQuarter, tb::kTicksPerQuarter},
    }};
    for (const auto& landing : landings)
        CHECK(tb::swing_position({landing.authored}, grid, tb::kTripletSwing).value ==
              landing.swung);
    CHECK(landings[1].swung == 235200);
    CHECK(landings[2].swung == 470400);
    CHECK(landings[3].swung == 588000);

    // The straight ratio is the identity on every tick of a pair, bit for bit.
    std::int64_t straight_moves = 0;
    for (std::int64_t tick = 0; tick < 2 * kEighth; ++tick)
        straight_moves += tb::swing_position({tick}, grid, tb::kStraightSwing).value != tick;
    CHECK(straight_moves == 0);

    // Round-tripping the whole pair: the expanded half of each pair recovers
    // exactly, while the compressed half has fewer ticks to land on than it
    // came from, so it recovers at most one tick late and never early.
    std::int64_t expanded_misses = 0;
    std::int64_t compressed_one_late = 0;
    std::int64_t compressed_worse = 0;
    for (std::int64_t tick = 0; tick < 2 * kEighth; ++tick) {
        const auto swung = tb::swing_position({tick}, grid, tb::kTripletSwing);
        const auto delta = tb::unswing_position(swung, grid, tb::kTripletSwing).value - tick;
        if (tick < kEighth)
            expanded_misses += delta != 0;
        else {
            compressed_one_late += delta == 1;
            compressed_worse += delta != 0 && delta != 1;
        }
    }
    CHECK(expanded_misses == 0);
    CHECK(compressed_worse == 0);
    CHECK(compressed_one_late == 117600);

    // An invalid grid or ratio is the identity: the caller validates, and a bad
    // setting must not silently move music.
    constexpr tb::SwingRatio degenerate{1, 1};
    CHECK_FALSE(tb::valid_swing_grid(tb::TickDuration{0}));
    CHECK_FALSE(tb::valid_swing_ratio(degenerate));
    CHECK(tb::swing_position({kSixteenth}, tb::TickDuration{0}, tb::kTripletSwing).value ==
          kSixteenth);
    CHECK(tb::swing_position({kSixteenth}, grid, degenerate).value == kSixteenth);
    CHECK(tb::unswing_position({kSixteenth}, grid, degenerate).value == kSixteenth);
}

TEST_CASE("order-preserving groove projection scales by strength and refuses to reorder",
          "[timebase][groove]") {
    const std::array<tb::GrooveKernelStep, 1> table{
        {{tb::TickDuration{kBassLate}, tb::kGrooveKernelUnitScale}}};

    tb::GrooveKernelInput input;
    input.table_grid = tb::TickDuration{kSixteenth};
    input.steps = table;
    input.timing_strength = tb::kGrooveKernelUnitScale;

    const auto full = tb::OrderPreservingGrooveKernel::create(input);
    REQUIRE(full.has_value());
    const auto moved = full.value().apply_timing(tb::TickPosition{0});
    REQUIRE(moved.has_value());
    CHECK(moved.value().value == kBassLate);

    // Strength scales the authored displacement exactly: half of 17640 is 8820.
    input.timing_strength = tb::kGrooveKernelUnitScale / 2;
    const auto half = tb::OrderPreservingGrooveKernel::create(input);
    REQUIRE(half.has_value());
    const auto half_moved = half.value().apply_timing(tb::TickPosition{0});
    REQUIRE(half_moved.has_value());
    CHECK(half_moved.value().value == kBassLate / 2);

    // Zero timing strength is exact identity, and that includes swing.
    input.timing_strength = 0;
    input.swing_grid = tb::TickDuration{kEighth};
    input.swing = tb::kTripletSwing;
    const auto bypass = tb::OrderPreservingGrooveKernel::create(input);
    REQUIRE(bypass.has_value());
    std::int64_t bypass_moves = 0;
    for (std::int64_t tick = 0; tick < kBar; tick += 4410) {
        const auto out = bypass.value().apply_timing(tb::TickPosition{tick});
        REQUIRE(out.has_value());
        bypass_moves += out.value().value != tick;
    }
    CHECK(bypass_moves == 0);

    // Control: the same sweep at full strength must move material, or the
    // identity assertion above could not fail.
    input.timing_strength = tb::kGrooveKernelUnitScale;
    const auto swung = tb::OrderPreservingGrooveKernel::create(input);
    REQUIRE(swung.has_value());
    std::int64_t swung_moves = 0;
    for (std::int64_t tick = 0; tick < kBar; tick += 4410) {
        const auto out = swung.value().apply_timing(tb::TickPosition{tick});
        REQUIRE(out.has_value());
        swung_moves += out.value().value != tick;
    }
    CHECK(swung_moves > 0);

    // Swing and table displacement add: the off-beat eighth is carried 117600
    // ticks by the triplet ratio and a further 17640 by the authored table.
    const auto combined = swung.value().apply_timing(tb::TickPosition{kEighth});
    REQUIRE(combined.has_value());
    CHECK(combined.value().value == kEighth + 117600 + kBassLate);

    // A table whose neighbouring steps cross is refused rather than silently
    // reordering the material it is asked to displace.
    const std::array<tb::GrooveKernelStep, 2> crossing{{
        {tb::TickDuration{kSixteenth / 2}, tb::kGrooveKernelUnitScale},
        {tb::TickDuration{-kSixteenth / 2}, tb::kGrooveKernelUnitScale},
    }};
    tb::GrooveKernelInput reordering;
    reordering.table_grid = tb::TickDuration{kSixteenth};
    reordering.steps = crossing;
    const auto rejected = tb::OrderPreservingGrooveKernel::create(reordering);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == tb::GrooveKernelError::ReordersEvents);
}

TEST_CASE("coordinate-keyed trigger probability is invariant under window partition",
          "[timebase][rhythm]") {
    tb::TriggerGrid<2, 16> grid;
    REQUIRE(grid.configure(2, 16, tb::TickDuration{kSixteenth}) == tb::TriggerGridError::None);
    for (std::size_t step = 0; step < 16; ++step) {
        REQUIRE(grid.set_cell(0, step, tb::TriggerCell{true, 90, {1, 1}, tb::TickDuration{0}}) ==
                tb::TriggerGridError::None);
        REQUIRE(grid.set_cell(1, step, tb::TriggerCell{true, 60, {1, 2}, tb::TickDuration{0}}) ==
                tb::TriggerGridError::None);
    }

    // One stable word per coordinate, derived from the musical coordinate
    // itself rather than from a callback-local stream.
    const auto make_draws = [&grid](std::uint64_t seed) {
        std::vector<std::uint64_t> draws(grid.coordinate_count());
        for (std::size_t step = 0; step < grid.step_count(); ++step)
            for (std::size_t track = 0; track < grid.track_count(); ++track)
                draws[step * grid.track_count() + track] = tb::coordinate_random(
                    seed, tb::RandomCoordinate{
                              tb::TickPosition{static_cast<std::int64_t>(step) * kSixteenth},
                              static_cast<std::uint64_t>(track), 0, 0});
        return draws;
    };

    const auto project = [&grid](const std::vector<std::uint64_t>& draws, std::int64_t begin,
                                 std::int64_t end) {
        std::vector<tb::TriggerEvent> events(grid.coordinate_count());
        const auto result = grid.project_window(tb::TickPosition{0}, tb::TickPosition{begin},
                                                tb::TickPosition{end}, draws, events);
        REQUIRE(result);
        events.resize(result.event_count);
        return events;
    };

    const auto draws = make_draws(0x5EEDu);
    const auto whole = project(draws, 0, kBar);
    const auto first = project(draws, 0, kBar / 2);
    const auto second = project(draws, kBar / 2, kBar);

    // The probabilistic lane must actually be partly selected, or the
    // comparison below would hold for an empty or a saturated lane.
    const auto selected = whole.size() - 16;
    REQUIRE(selected > 0);
    REQUIRE(selected < 16);

    std::vector<tb::TriggerEvent> joined = first;
    joined.insert(joined.end(), second.begin(), second.end());
    CHECK(joined == whole);

    // Control: the draws are genuinely consulted, so a different seed selects a
    // different set of steps.
    CHECK(project(make_draws(0xA11CEu), 0, kBar) != whole);
}

TEST_CASE("Euclidean onsets reproduce an even bell pattern but not son clave", "[music][rhythm]") {
    const auto onsets = [](const auto& pattern) {
        std::vector<std::size_t> result;
        for (std::size_t step = 0; step < pattern.size(); ++step)
            if (*pattern.at(step))
                result.push_back(step);
        return result;
    };

    // The evenly-spread three-onset figure over eight steps.
    const auto tresillo = euclidean_pattern<16>(8, 3);
    REQUIRE(tresillo);
    CHECK(onsets(tresillo.pattern) == std::vector<std::size_t>{0, 3, 6});

    // A seven-onset bell over twelve steps is reachable, but only at the
    // rotation that starts the cycle in the right place.
    const auto bell = euclidean_pattern<16>(12, 7, 5);
    REQUIRE(bell);
    CHECK(onsets(bell.pattern) == std::vector<std::size_t>{0, 2, 4, 5, 7, 9, 11});

    // The asymmetric five-onset clave figure is NOT an even distribution, so no
    // rotation of the even five-in-sixteen pattern produces it. Authoring it
    // directly is the supported route, and the two differ at step 4.
    const std::vector<std::size_t> son{0, 3, 6, 10, 12};
    for (std::int64_t rotation = 0; rotation < 16; ++rotation) {
        const auto even = euclidean_pattern<16>(16, 5, rotation);
        REQUIRE(even);
        CHECK(onsets(even.pattern) != son);
    }
    BinaryPattern<16> authored;
    constexpr std::array<std::uint8_t, 16> son_bits{1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0};
    REQUIRE(authored.assign(son_bits) == PatternError::none);
    CHECK(onsets(authored) == son);
    CHECK(authored.onset_count() == 5);
}
