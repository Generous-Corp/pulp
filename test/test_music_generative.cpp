#include <pulp/music/music.hpp>
#include <pulp/music/detail/random_range.hpp>

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
