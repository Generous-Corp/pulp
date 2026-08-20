#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/spectral_feature_frontends.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

using pulp::signal::SpectralFeatureFrontEndT;
using pulp::signal::StreamingAnalysisConfig;

namespace {

template <typename Frame, std::size_t Capacity> struct FrameCollector {
    std::array<Frame, Capacity> frames{};
    std::size_t size = 0;

    void operator()(const Frame& frame) noexcept {
        if (size < Capacity)
            frames[size++] = frame;
    }
};

StreamingAnalysisConfig config(std::uint32_t fft, std::uint64_t hop) {
    return {.sample_rate = 25600.0, .channels = 1, .fft_size = fft, .hop_size = hop};
}

template <std::size_t Size>
std::array<float, Size> bin_tone(std::size_t bin, double amplitude = 1.0) {
    std::array<float, Size> result{};
    constexpr double two_pi = 6.283185307179586476925286766559;
    for (std::size_t i = 0; i < Size; ++i)
        result[i] =
            static_cast<float>(amplitude * std::sin(two_pi * static_cast<double>(bin * i) / Size));
    return result;
}

} // namespace

TEST_CASE("spectral feature tone has centered centroid bounded rolloff and low flatness",
          "[signal][spectral-feature-frontends]") {
    constexpr std::size_t size = 256;
    constexpr std::size_t tone_bin = 25;
    auto tone = bin_tone<size>(tone_bin);
    const float* channels[] = {tone.data()};

    SpectralFeatureFrontEndT<float, size, 1> front_end;
    REQUIRE(front_end.prepare(config(size, size)));
    FrameCollector<decltype(front_end)::Frame, 1> frames;
    REQUIRE(front_end.process(channels, 1, size, std::ref(frames)));
    REQUIRE(frames.size == 1);
    REQUIRE(frames.frames[0].valid);
    constexpr double expected_hz = 2500.0;
    constexpr double bin_width_hz = 100.0;
    CHECK(frames.frames[0].centroid_hz == Catch::Approx(expected_hz).margin(1.0));
    CHECK(frames.frames[0].rolloff_hz >= expected_hz);
    CHECK(frames.frames[0].rolloff_hz <= expected_hz + bin_width_hz);
    CHECK(frames.frames[0].flatness < 1.0e-3);
    CHECK(frames.frames[0].flux == 0.0);
    CHECK(frames.frames[0].window_start_frame == 0);
    CHECK(frames.frames[0].window_center_frame == size / 2);
    CHECK(frames.frames[0].ready_at_frame == size - 1);
    CHECK(frames.frames[0].sequence == 1);
    CHECK(front_end.algorithmic_latency_samples() == size / 2);
    CHECK(front_end.startup_samples() == size);
}

TEST_CASE("flat-spectrum noise is high-flatness and centered on the spectral mean",
          "[signal][spectral-feature-frontends]") {
    constexpr std::size_t size = 512;
    std::array<float, size> noise{};
    std::uint32_t state = 0x12345678u;
    for (auto& sample : noise) {
        state = state * 1664525u + 1013904223u;
        sample = static_cast<float>(static_cast<std::int32_t>(state)) /
                 static_cast<float>(std::numeric_limits<std::int32_t>::max());
    }
    const float* channels[] = {noise.data()};
    SpectralFeatureFrontEndT<float, size, 1> front_end;
    REQUIRE(front_end.prepare(config(size, size)));
    FrameCollector<decltype(front_end)::Frame, 1> frames;
    REQUIRE(front_end.process(channels, 1, size, std::ref(frames)));
    REQUIRE(frames.size == 1);
    REQUIRE(frames.frames[0].valid);
    CHECK(frames.frames[0].flatness > 0.70);
    CHECK(frames.frames[0].flatness <= 1.0);
    CHECK(frames.frames[0].centroid_hz == Catch::Approx(6400.0).margin(900.0));
    CHECK(frames.frames[0].rolloff_hz > frames.frames[0].centroid_hz);
    CHECK(frames.frames[0].rolloff_hz <= 12800.0);
}

TEST_CASE("identical consecutive spectra have exactly zero normalized flux",
          "[signal][spectral-feature-frontends]") {
    constexpr std::size_t size = 128;
    auto one = bin_tone<size>(11);
    std::array<float, size * 2> repeated{};
    std::copy(one.begin(), one.end(), repeated.begin());
    std::copy(one.begin(), one.end(), repeated.begin() + size);
    const float* channels[] = {repeated.data()};
    SpectralFeatureFrontEndT<float, size, 1> front_end;
    REQUIRE(front_end.prepare(config(size, size)));
    FrameCollector<decltype(front_end)::Frame, 2> frames;
    REQUIRE(front_end.process(channels, 1, repeated.size(), std::ref(frames)));
    REQUIRE(frames.size == 2);
    CHECK(frames.frames[0].flux == 0.0);
    CHECK(frames.frames[1].flux == 0.0);
}

TEST_CASE("off-bin stationary tone remains stable across overlapping windows",
          "[signal][spectral-feature-frontends]") {
    constexpr std::size_t size = 256;
    constexpr std::size_t hop = 64;
    constexpr std::size_t frame_count = 4;
    constexpr double frequency_hz = 2535.0;
    std::array<float, size + (frame_count - 1) * hop> tone{};
    constexpr double two_pi = 6.283185307179586476925286766559;
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] =
            static_cast<float>(std::sin(two_pi * frequency_hz * static_cast<double>(i) / 25600.0));
    }
    const float* channels[] = {tone.data()};

    SpectralFeatureFrontEndT<float, size, 1> front_end;
    REQUIRE(front_end.prepare(config(size, hop)));
    FrameCollector<decltype(front_end)::Frame, frame_count> frames;
    REQUIRE(front_end.process(channels, 1, tone.size(), std::ref(frames)));
    REQUIRE(frames.size == frame_count);
    for (std::size_t i = 0; i < frames.size; ++i) {
        REQUIRE(frames.frames[i].valid);
        CHECK(frames.frames[i].centroid_hz == Catch::Approx(frequency_hz).margin(3.0));
        CHECK(frames.frames[i].flatness < 1.0e-3);
        if (i > 0)
            CHECK(frames.frames[i].flux < 5.0e-5);
    }
}

TEST_CASE("silence and DC emit finite floor-bounded degenerate features",
          "[signal][spectral-feature-frontends]") {
    constexpr std::size_t size = 64;
    std::array<float, size * 2> input{};
    std::fill(input.begin() + size, input.end(), 0.5f);
    const float* channels[] = {input.data()};
    SpectralFeatureFrontEndT<float, size, 1> front_end;
    REQUIRE(front_end.prepare(config(size, size)));
    FrameCollector<decltype(front_end)::Frame, 2> frames;
    REQUIRE(front_end.process(channels, 1, input.size(), std::ref(frames)));
    REQUIRE(frames.size == 2);
    CHECK_FALSE(frames.frames[0].valid);
    CHECK(frames.frames[0].centroid_hz == 0.0);
    CHECK(frames.frames[0].flatness == 0.0);
    CHECK(frames.frames[0].rolloff_hz == 0.0);
    CHECK(frames.frames[0].flux == 0.0);
    REQUIRE(frames.frames[1].valid);
    CHECK(std::isfinite(frames.frames[1].centroid_hz));
    CHECK(std::isfinite(frames.frames[1].rolloff_hz));
    CHECK(frames.frames[1].flatness >= 0.0);
    CHECK(frames.frames[1].flatness <= 1.0);
    CHECK(std::isfinite(frames.frames[1].flux));
}

TEST_CASE("spectral feature process is partition invariant allocation-free and fault-recovering",
          "[signal][spectral-feature-frontends]") {
    constexpr std::size_t size = 64;
    std::array<float, size * 3> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(std::sin(0.17 * static_cast<double>(i)));

    SpectralFeatureFrontEndT<float, size, 1> one_shot;
    REQUIRE(one_shot.prepare(config(size, 32)));
    FrameCollector<decltype(one_shot)::Frame, 8> expected;
    const float* all[] = {input.data()};
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(one_shot.process(all, 1, input.size(), std::ref(expected)));
        CHECK(probe.allocation_count() == 0);
    }

    SpectralFeatureFrontEndT<float, size, 1> partitioned;
    REQUIRE(partitioned.prepare(config(size, 32)));
    FrameCollector<decltype(partitioned)::Frame, 8> actual;
    const float* first[] = {input.data()};
    const float* second[] = {input.data() + 17};
    const float* third[] = {input.data() + 88};
    REQUIRE(partitioned.process(first, 1, 17, std::ref(actual)));
    REQUIRE(partitioned.process(second, 1, 71, std::ref(actual)));
    REQUIRE(partitioned.process(third, 1, input.size() - 88, std::ref(actual)));
    REQUIRE(actual.size == expected.size);
    for (std::size_t i = 0; i < actual.size; ++i) {
        CHECK(actual.frames[i].centroid_hz == expected.frames[i].centroid_hz);
        CHECK(actual.frames[i].flatness == expected.frames[i].flatness);
        CHECK(actual.frames[i].rolloff_hz == expected.frames[i].rolloff_hz);
        CHECK(actual.frames[i].flux == expected.frames[i].flux);
        CHECK(actual.frames[i].ready_at_frame == expected.frames[i].ready_at_frame);
    }

    std::array<float, size * 2 + 1> faulty{};
    faulty[2] = std::numeric_limits<float>::quiet_NaN();
    const float* fault_channels[] = {faulty.data()};
    partitioned.reset();
    FrameCollector<decltype(partitioned)::Frame, 4> recovered;
    CHECK_FALSE(partitioned.process(fault_channels, 1, faulty.size(), std::ref(recovered)));
    REQUIRE(recovered.size >= 1);
    CHECK(recovered.frames[0].window_start_frame == 3);
    CHECK(recovered.frames[0].ready_at_frame == size + 2);
    CHECK(recovered.frames[0].flux == 0.0);
}
