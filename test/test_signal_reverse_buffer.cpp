#include <pulp/signal/reverse_buffer.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

using Catch::Approx;
using pulp::signal::ReverseBuffer;
using pulp::signal::ReverseBuffer64;
using pulp::signal::ReverseBufferState;

namespace {

template <std::size_t Size>
std::array<double, Size> hand_oracle(const std::array<double, Size>& input,
                                     std::size_t window_samples, int latency_adjustment = 0,
                                     bool off_by_one = false) {
    std::array<double, Size> output{};
    const auto latency = static_cast<long long>(window_samples) + latency_adjustment;
    for (std::size_t frame = 0; frame < Size; ++frame) {
        if (static_cast<long long>(frame) < latency)
            continue;
        const auto emitted = static_cast<std::size_t>(static_cast<long long>(frame) - latency);
        const auto segment = emitted / window_samples;
        const auto position = emitted % window_samples;
        const auto reverse_position = off_by_one ? (window_samples - position) % window_samples
                                                 : window_samples - 1u - position;
        const auto source = segment * window_samples + reverse_position;
        if (source < input.size())
            output[frame] = input[source];
    }
    return output;
}

template <std::size_t Size>
bool matches(const std::array<double, Size>& actual, const std::array<double, Size>& expected,
             double tolerance = 1.0e-14) {
    for (std::size_t i = 0; i < Size; ++i)
        if (std::abs(actual[i] - expected[i]) > tolerance)
            return false;
    return true;
}

} // namespace

TEST_CASE("reverse buffer validates and publishes lifecycle state transactionally",
          "[signal][reverse-buffer][contract][transactional]") {
    ReverseBuffer64 reverse;
    REQUIRE(reverse.state() == ReverseBufferState::unprepared);
    REQUIRE(reverse.latency_samples() == 0);
    REQUIRE(reverse.tail_samples() == 0);
    REQUIRE_FALSE(reverse.prepare(1));
    REQUIRE(reverse.configure({.window_samples = 4, .boundary_fade_samples = 0}));
    REQUIRE_FALSE(reverse.configure({.window_samples = 4, .boundary_fade_samples = 1}));
    REQUIRE(reverse.config().window_samples == 4);
    REQUIRE(reverse.prepare(8));
    REQUIRE(reverse.prepared());
    REQUIRE(reverse.maximum_window_samples() == 8);
    REQUIRE(reverse.retained_bytes() == 16 * sizeof(double));
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    REQUIRE(reverse.startup_latency_samples() == 4);
    REQUIRE(reverse.latency_samples() == 4);
    REQUIRE(reverse.tail_samples() == 7);

    for (double input : {1.0, 2.0, 3.0, 4.0})
        REQUIRE(reverse.process_sample(input) == 0.0);
    REQUIRE(reverse.state() == ReverseBufferState::running);
    REQUIRE(reverse.process_sample(5.0) == 4.0);

    REQUIRE_FALSE(reverse.configure({.window_samples = 9, .boundary_fade_samples = 0}));
    REQUIRE(reverse.config().window_samples == 4);
    REQUIRE(reverse.process_sample(6.0) == 3.0);
    REQUIRE_FALSE(reverse.prepare(2));
    REQUIRE(reverse.maximum_window_samples() == 8);
    REQUIRE(reverse.process_sample(7.0) == 2.0);

    REQUIRE(reverse.configure({.window_samples = 6, .boundary_fade_samples = 2}));
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    REQUIRE(reverse.position() == 0);
    REQUIRE(reverse.latency_samples() == 6);
    REQUIRE(reverse.tail_samples() == 11);
}

TEST_CASE("reverse buffer matches a hand sequence and rejects timing mutations",
          "[signal][reverse-buffer][oracle][negative-control][latency]") {
    constexpr std::size_t kWindow = 4;
    const std::array<double, 16> input{1.0, 2.0,  3.0,  4.0,  5.0,  6.0,  7.0,  8.0,
                                       9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0};
    ReverseBuffer64 reverse;
    REQUIRE(reverse.configure({.window_samples = kWindow}));
    REQUIRE(reverse.prepare(kWindow));

    std::array<double, input.size()> actual{};
    reverse.process_block(input.data(), actual.data(), actual.size());
    const auto expected = hand_oracle(input, kWindow);
    REQUIRE(matches(actual, expected));
    REQUIRE_FALSE(matches(actual, hand_oracle(input, kWindow, -1, false)));
    REQUIRE_FALSE(matches(actual, hand_oracle(input, kWindow, 0, true)));

    REQUIRE(actual[0] == 0.0);
    REQUIRE(actual[3] == 0.0);
    REQUIRE(actual[4] == 4.0);
    REQUIRE(actual[7] == 1.0);
    REQUIRE(actual[8] == 8.0);
}

TEST_CASE("raised-cosine reverse boundaries remove the full-scale splice",
          "[signal][reverse-buffer][boundary][click-safe]") {
    constexpr std::size_t kWindow = 8;
    std::array<double, 24> input{};
    std::fill_n(input.begin(), kWindow, 1.0);
    std::fill_n(input.begin() + kWindow, kWindow, -1.0);

    auto render = [&](std::size_t fade) {
        ReverseBuffer64 reverse;
        REQUIRE(reverse.configure({.window_samples = kWindow, .boundary_fade_samples = fade}));
        REQUIRE(reverse.prepare(kWindow));
        std::array<double, input.size()> output{};
        reverse.process_block(input.data(), output.data(), output.size());
        return output;
    };

    const auto faded = render(2);
    const auto raw = render(0);
    const std::array expected_gain{0.0, 0.5, 1.0, 1.0, 1.0, 1.0, 0.5, 0.0};
    for (std::size_t i = 0; i < kWindow; ++i) {
        REQUIRE(faded[kWindow + i] == Approx(expected_gain[i]).margin(1.0e-14));
        REQUIRE(faded[2 * kWindow + i] == Approx(-expected_gain[i]).margin(1.0e-14));
    }
    REQUIRE(std::abs(faded[2 * kWindow] - faded[2 * kWindow - 1]) < 1.0e-14);
    REQUIRE(std::abs(faded[2 * kWindow + 1] - faded[2 * kWindow]) == Approx(0.5).margin(1.0e-14));
    REQUIRE(std::abs(raw[2 * kWindow] - raw[2 * kWindow - 1]) == Approx(2.0));
}

TEST_CASE("reverse bypass is exact and every transition restarts priming",
          "[signal][reverse-buffer][bypass][state]") {
    ReverseBuffer64 reverse;
    REQUIRE(reverse.process_sample(0.25) == 0.0);
    reverse.set_bypassed(true);
    REQUIRE(reverse.state() == ReverseBufferState::unprepared);
    REQUIRE(reverse.latency_samples() == 0);
    REQUIRE(reverse.process_sample(-0.25) == -0.25);
    REQUIRE(reverse.configure({.window_samples = 4}));
    REQUIRE(reverse.prepare(4));
    REQUIRE(reverse.state() == ReverseBufferState::bypassed);
    REQUIRE(reverse.startup_latency_samples() == 4);
    REQUIRE(reverse.latency_samples() == 0);
    REQUIRE(reverse.tail_samples() == 0);
    REQUIRE(reverse.process_sample(0.75) == 0.75);

    reverse.set_bypassed(false);
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    REQUIRE(reverse.latency_samples() == 4);
    REQUIRE(reverse.process_sample(1.0) == 0.0);
    REQUIRE(reverse.process_sample(2.0) == 0.0);
    reverse.set_bypassed(false); // An idempotent setter must not discard progress.
    REQUIRE(reverse.process_sample(3.0) == 0.0);
    REQUIRE(reverse.process_sample(4.0) == 0.0);
    REQUIRE(reverse.process_sample(5.0) == 4.0);

    reverse.set_bypassed(true);
    REQUIRE(reverse.state() == ReverseBufferState::bypassed);
    REQUIRE(reverse.process_sample(0.125) == 0.125);
    reverse.set_bypassed(false);
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    for (double input : {6.0, 7.0, 8.0, 9.0})
        REQUIRE(reverse.process_sample(input) == 0.0);
    REQUIRE(reverse.process_sample(10.0) == 9.0);
}

TEST_CASE("discard handles seeks and non-finite input without stale playback",
          "[signal][reverse-buffer][discard][seek][fault]") {
    ReverseBuffer64 reverse;
    REQUIRE(reverse.configure({.window_samples = 4}));
    REQUIRE(reverse.prepare(4));
    for (double input : {1.0, 2.0, 3.0, 4.0})
        REQUIRE(reverse.process_sample(input) == 0.0);
    REQUIRE(reverse.process_sample(5.0) == 4.0);

    reverse.discard_history();
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    for (double input : {10.0, 11.0, 12.0, 13.0})
        REQUIRE(reverse.process_sample(input) == 0.0);
    REQUIRE(reverse.process_sample(14.0) == 13.0);

    REQUIRE(reverse.process_sample(std::numeric_limits<double>::infinity()) == 0.0);
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    for (double input : {20.0, 21.0, 22.0, 23.0})
        REQUIRE(reverse.process_sample(input) == 0.0);
    REQUIRE(reverse.process_sample(24.0) == 23.0);

    reverse.reset();
    REQUIRE(reverse.state() == ReverseBufferState::priming);
    std::array<double, 2> buffer{};
    reverse.process_block(nullptr, buffer.data(), buffer.size());
    reverse.process_block(buffer.data(), nullptr, buffer.size());
}

TEST_CASE("reverse buffer tail bound reaches a capture window's first sample",
          "[signal][reverse-buffer][tail]") {
    ReverseBuffer64 reverse;
    REQUIRE(reverse.configure({.window_samples = 4}));
    REQUIRE(reverse.prepare(4));
    std::array<double, 8> input{};
    input[0] = 1.0;
    std::array<double, input.size()> output{};
    reverse.process_block(input.data(), output.data(), output.size());
    for (std::size_t i = 0; i < output.size(); ++i)
        REQUIRE(output[i] == (i == 7 ? 1.0 : 0.0));
    REQUIRE(reverse.tail_samples() == 7);
}

TEST_CASE("reverse blocks are arbitrary-partition and in-place invariant",
          "[signal][reverse-buffer][partition][in-place]") {
    constexpr std::size_t kFrames = 1001;
    std::array<double, kFrames> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.6 * std::sin(0.037 * static_cast<double>(i));

    ReverseBuffer64 whole;
    ReverseBuffer64 partitioned;
    for (auto* reverse : {&whole, &partitioned}) {
        REQUIRE(reverse->configure({.window_samples = 17, .boundary_fade_samples = 3}));
        REQUIRE(reverse->prepare(64));
    }

    std::array<double, kFrames> expected{};
    auto in_place = input;
    whole.process_block(input.data(), expected.data(), input.size());
    std::size_t offset = 0;
    for (const std::size_t requested : {1u, 64u, 7u, 255u, 3u, 511u, 160u}) {
        const auto count = std::min(requested, input.size() - offset);
        partitioned.process_block(in_place.data() + offset, in_place.data() + offset, count);
        offset += count;
        if (offset == input.size())
            break;
    }
    if (offset < input.size())
        partitioned.process_block(in_place.data() + offset, in_place.data() + offset,
                                  input.size() - offset);
    REQUIRE(in_place == expected);
}

TEST_CASE("reverse realtime paths allocate no memory", "[signal][reverse-buffer][rt-safety]") {
    ReverseBuffer reverse;
    REQUIRE(reverse.configure({.window_samples = 31, .boundary_fade_samples = 4}));
    REQUIRE(reverse.prepare(128));
    std::array<float, 257> buffer{};

    pulp::test::RtAllocationProbe probe;
    reverse.process_block(buffer.data(), buffer.data(), buffer.size());
    reverse.reset();
    reverse.set_bypassed(true);
    reverse.process_block(buffer.data(), buffer.data(), buffer.size());
    reverse.set_bypassed(false);
    reverse.discard_history();
    REQUIRE_FALSE(probe.saw_allocation());
}
