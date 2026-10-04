#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/tempo_delay_wrapper.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {
using pulp::signal::TempoDelayWrapper;
using pulp::signal::TempoDelayWrapperError;
using pulp::signal::TempoDelayWrapperInterpolation;
using pulp::timebase::BeatDivision;

std::vector<float> render(TempoDelayWrapper& delay, std::size_t frames,
                          const std::vector<std::size_t>& partitions) {
    std::vector<float> left(frames, 0.0f);
    std::vector<float> right(frames, 0.0f);
    left[0] = right[0] = 1.0f;
    std::size_t offset = 0;
    for (const auto block : partitions) {
        const auto count = std::min(block, frames - offset);
        if (count == 0)
            continue;
        auto result = delay.process(left.data() + offset, right.data() + offset, count);
        REQUIRE(result.processed_frames == count);
        offset += count;
        if (offset == frames)
            break;
    }
    if (offset < frames)
        REQUIRE(delay.process(left.data() + offset, right.data() + offset, frames - offset));
    return left;
}
} // namespace

TEST_CASE("tempo delay wrapper matches the causal impulse oracle at supported sample rates",
          "[signal][tempo-delay-wrapper][audio-harness]") {
    for (const double sample_rate : {44'100.0, 48'000.0, 96'000.0}) {
        TempoDelayWrapper delay;
        REQUIRE(delay.prepare(sample_rate, 256) == TempoDelayWrapperError::none);
        REQUIRE(delay.set_delay_samples(7.0) == TempoDelayWrapperError::none);
        std::array<float, 24> left{};
        std::array<float, 24> right{};
        left[0] = right[0] = 1.0f;
        REQUIRE(delay.process(left.data(), right.data(), left.size()));
        for (std::size_t i = 0; i < left.size(); ++i)
            REQUIRE(left[i] == Catch::Approx(i == 7 ? 1.0f : 0.0f).margin(1.0e-6));
    }
}

TEST_CASE("tempo delay wrapper agrees with the steady state integer delay oracle",
          "[signal][tempo-delay-wrapper][steady-state]") {
    TempoDelayWrapper delay;
    REQUIRE(delay.prepare(48'000.0, 256) == TempoDelayWrapperError::none);
    REQUIRE(delay.set_delay_samples(13.0) == TempoDelayWrapperError::none);
    constexpr std::size_t frames = 2048;
    std::array<float, frames> left{};
    std::array<float, frames> right{};
    constexpr double frequency = 997.0;
    constexpr double pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < frames; ++i)
        left[i] = right[i] = static_cast<float>(std::sin(2.0 * pi * frequency * i / 48'000.0));
    REQUIRE(delay.process(left.data(), right.data(), frames));
    for (std::size_t i = 32; i < frames; ++i) {
        const auto expected = std::sin(2.0 * pi * frequency * (i - 13) / 48'000.0);
        REQUIRE(left[i] == Catch::Approx(expected).margin(2.0e-5));
    }
}

TEST_CASE("tempo delay wrapper is invariant to callback partitioning and reset clears history",
          "[signal][tempo-delay-wrapper][partition]") {
    TempoDelayWrapper one_block;
    TempoDelayWrapper split;
    REQUIRE(one_block.prepare(48'000.0, 256) == TempoDelayWrapperError::none);
    REQUIRE(split.prepare(48'000.0, 256) == TempoDelayWrapperError::none);
    REQUIRE(one_block.set_delay_samples(11.0) == TempoDelayWrapperError::none);
    REQUIRE(split.set_delay_samples(11.0) == TempoDelayWrapperError::none);
    const auto whole = render(one_block, 512, {512});
    const auto partitioned = render(split, 512, {1, 7, 64, 3, 128, 17, 292});
    REQUIRE(whole == partitioned);

    split.reset();
    std::array<float, 32> left{};
    std::array<float, 32> right{};
    left[0] = right[0] = 1.0f;
    REQUIRE(split.process(left.data(), right.data(), left.size()));
    for (std::size_t index = 0; index < left.size(); ++index)
        REQUIRE(left[index] == Catch::Approx(index == 11 ? 1.0f : 0.0f).margin(1.0e-6));
}

TEST_CASE("tempo changes use a dual read head crossfade rather than a pitch glide",
          "[signal][tempo-delay-wrapper][retime]") {
    TempoDelayWrapper delay;
    REQUIRE(delay.prepare(48'000.0, 48'000, TempoDelayWrapperInterpolation::lagrange3, 8) ==
            TempoDelayWrapperError::none);
    REQUIRE(delay.set_tempo(BeatDivision::Quarter, 120.0) == TempoDelayWrapperError::none);
    REQUIRE(delay.current_delay_samples() == Catch::Approx(24'000.0));
    REQUIRE(delay.set_tempo(BeatDivision::Eighth, 120.0) == TempoDelayWrapperError::none);
    REQUIRE(delay.transition_active());
    REQUIRE(delay.current_delay_samples() == Catch::Approx(24'000.0));
    std::array<float, 16> left{};
    std::array<float, 16> right{};
    REQUIRE(delay.process(left.data(), right.data(), left.size()));
    REQUIRE_FALSE(delay.transition_active());
    REQUIRE(delay.current_delay_samples() == Catch::Approx(12'000.0));
}

TEST_CASE("tempo delay wrapper refuses invalid configuration without mutating state",
          "[signal][tempo-delay-wrapper][negative]") {
    TempoDelayWrapper delay;
    REQUIRE(delay.prepare(48'000.0, 256) == TempoDelayWrapperError::none);
    REQUIRE(delay.set_delay_samples(23.0) == TempoDelayWrapperError::none);
    REQUIRE(delay.set_feedback(0.99) == TempoDelayWrapperError::invalid_feedback);
    REQUIRE(delay.feedback() == Catch::Approx(0.0));
    REQUIRE(delay.set_tempo(BeatDivision::Quarter, 0.0) == TempoDelayWrapperError::invalid_tempo);
    REQUIRE(delay.target_delay_samples() == Catch::Approx(23.0));
    REQUIRE(delay.set_tempo(static_cast<BeatDivision>(255), 120.0) ==
            TempoDelayWrapperError::invalid_division);

    TempoDelayWrapper unsupported;
    REQUIRE(unsupported.prepare(48'000.0, 256, TempoDelayWrapperInterpolation::thiran1) ==
            TempoDelayWrapperError::unsupported_interpolation);
}
