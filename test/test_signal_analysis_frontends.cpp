#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/analysis_frontends.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>

using pulp::signal::ChromaFrontEndT;
using pulp::signal::OnsetDetectionMethod;
using pulp::signal::OnsetNoveltyFrontEndT;
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

StreamingAnalysisConfig config(std::uint32_t fft, std::uint64_t hop, std::size_t channels = 1) {
    StreamingAnalysisConfig result;
    result.sample_rate = 48000.0;
    result.channels = channels;
    result.fft_size = fft;
    result.hop_size = hop;
    return result;
}

double direct_magnitude(const float* data, std::size_t size, std::size_t bin) {
    std::complex<double> value{};
    constexpr double two_pi = 6.283185307179586476925286766559;
    for (std::size_t frame = 0; frame < size; ++frame) {
        const auto phase = -two_pi * static_cast<double>(bin * frame) / static_cast<double>(size);
        value += static_cast<double>(data[frame]) *
                 std::complex<double>{std::cos(phase), std::sin(phase)};
    }
    return std::abs(value);
}

} // namespace

TEST_CASE("analysis cadence emits complete overlapping and gapped windows",
          "[signal][analysis-frontends]") {
    std::array<float, 24> input{};
    const float* channels[] = {input.data()};

    OnsetNoveltyFrontEndT<float, 32, 1> overlapping;
    REQUIRE(overlapping.prepare(config(8, 3), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(overlapping)::Frame, 8> overlap_frames;
    REQUIRE(overlapping.process(channels, 1, 14, std::ref(overlap_frames)));
    REQUIRE(overlap_frames.size == 3);
    CHECK(overlap_frames.frames[0].window_start_frame == 0);
    CHECK(overlap_frames.frames[0].ready_at_frame == 7);
    CHECK(overlap_frames.frames[1].window_start_frame == 3);
    CHECK(overlap_frames.frames[2].window_start_frame == 6);

    OnsetNoveltyFrontEndT<float, 32, 1> gapped;
    REQUIRE(gapped.prepare(config(8, 12), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(gapped)::Frame, 4> gap_frames;
    REQUIRE(gapped.process(channels, 1, 19, std::ref(gap_frames)));
    REQUIRE(gap_frames.size == 1); // no padded tail at frame 18
    REQUIRE(gapped.process(channels, 1, 1, std::ref(gap_frames)));
    REQUIRE(gap_frames.size == 2);
    CHECK(gap_frames.frames[1].window_start_frame == 12);
    CHECK(gap_frames.frames[1].ready_at_frame == 19);
}

TEST_CASE("block partitions and reset are deterministic", "[signal][analysis-frontends]") {
    std::array<float, 24> input{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i) * 0.01f;
    const float* all[] = {input.data()};

    OnsetNoveltyFrontEndT<float, 32, 1> one_shot;
    REQUIRE(one_shot.prepare(config(8, 4), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(one_shot)::Frame, 8> expected;
    REQUIRE(one_shot.process(all, 1, input.size(), std::ref(expected)));

    OnsetNoveltyFrontEndT<float, 32, 1> partitioned;
    REQUIRE(partitioned.prepare(config(8, 4), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(partitioned)::Frame, 8> actual;
    const float* first[] = {input.data()};
    const float* second[] = {input.data() + 5};
    const float* third[] = {input.data() + 12};
    REQUIRE(partitioned.process(first, 1, 5, std::ref(actual)));
    REQUIRE(partitioned.process(second, 1, 7, std::ref(actual)));
    REQUIRE(partitioned.process(third, 1, 12, std::ref(actual)));
    REQUIRE(actual.size == expected.size);
    for (std::size_t i = 0; i < actual.size; ++i) {
        CHECK(actual.frames[i].novelty == Catch::Approx(expected.frames[i].novelty));
        CHECK(actual.frames[i].ready_at_frame == expected.frames[i].ready_at_frame);
    }

    partitioned.reset();
    FrameCollector<decltype(partitioned)::Frame, 8> after_reset;
    REQUIRE(partitioned.process(all, 1, input.size(), std::ref(after_reset)));
    REQUIRE(after_reset.size == expected.size);
    CHECK(after_reset.frames[0].sequence == 1);
    CHECK(after_reset.frames[0].ready_at_frame == 7);
}

TEST_CASE("energy and spectral channel laws preserve anti-phase behavior",
          "[signal][analysis-frontends]") {
    std::array<float, 16> left{};
    std::array<float, 16> right{};
    for (std::size_t i = 8; i < 16; ++i) {
        left[i] = 1.0f;
        right[i] = -1.0f;
    }
    const float* channels[] = {left.data(), right.data()};

    OnsetNoveltyFrontEndT<float, 16, 2> energy;
    REQUIRE(energy.prepare(config(8, 8, 2), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(energy)::Frame, 2> energy_frames;
    REQUIRE(energy.process(channels, 2, 16, std::ref(energy_frames)));
    REQUIRE(energy_frames.size == 2);
    CHECK(energy_frames.frames[1].novelty == Catch::Approx(1.0));

    OnsetNoveltyFrontEndT<float, 16, 2> spectral;
    REQUIRE(spectral.prepare(config(8, 8, 2), OnsetDetectionMethod::SpectralFlux));
    FrameCollector<decltype(spectral)::Frame, 2> spectral_frames;
    REQUIRE(spectral.process(channels, 2, 16, std::ref(spectral_frames)));
    REQUIRE(spectral_frames.size == 2);
    CHECK(spectral_frames.frames[1].novelty == Catch::Approx(0.0));
}

TEST_CASE("spectral novelty agrees with an independent direct DFT",
          "[signal][analysis-frontends]") {
    std::array<float, 16> input{};
    for (std::size_t i = 8; i < input.size(); ++i)
        input[i] =
            static_cast<float>(std::sin(6.283185307179586 * static_cast<double>(i - 8) / 8.0));
    const float* channels[] = {input.data()};
    OnsetNoveltyFrontEndT<float, 16, 1> front_end;
    REQUIRE(front_end.prepare(config(8, 8), OnsetDetectionMethod::SpectralFlux));
    FrameCollector<decltype(front_end)::Frame, 2> frames;
    REQUIRE(front_end.process(channels, 1, input.size(), std::ref(frames)));
    REQUIRE(frames.size == 2);

    double expected = 0.0;
    for (std::size_t bin = 0; bin <= 4; ++bin)
        expected += direct_magnitude(input.data() + 8, 8, bin);
    CHECK(frames.frames[1].novelty == Catch::Approx(expected).epsilon(1e-5));

    OnsetNoveltyFrontEndT<float, 16, 1> hfc;
    REQUIRE(hfc.prepare(config(8, 8), OnsetDetectionMethod::HighFrequencyContent));
    FrameCollector<decltype(hfc)::Frame, 2> hfc_frames;
    REQUIRE(hfc.process(channels, 1, input.size(), std::ref(hfc_frames)));
    double hfc_expected = 0.0;
    for (std::size_t bin = 0; bin <= 4; ++bin)
        hfc_expected += direct_magnitude(input.data() + 8, 8, bin) * static_cast<double>(bin + 1);
    REQUIRE(hfc_frames.size == 2);
    CHECK(hfc_frames.frames[1].novelty == Catch::Approx(hfc_expected).epsilon(1e-5));
}

TEST_CASE("chroma reports raw and normalized pitch-class energy", "[signal][analysis-frontends]") {
    static_assert(
        std::is_same_v<typename decltype(pulp::signal::ChromaFrameT<float>{}.magnitude)::value_type,
                       double>);
    constexpr std::size_t size = 256;
    std::array<float, size> tone{};
    for (std::size_t i = 0; i < size; ++i)
        tone[i] =
            static_cast<float>(std::sin(6.283185307179586 * 7.0 * static_cast<double>(i) / size));
    auto chroma_config = config(size, size);
    chroma_config.sample_rate = 16384.0; // bin 7 = 448 Hz, nearest pitch A
    const float* channels[] = {tone.data()};
    ChromaFrontEndT<float, size, 1> front_end;
    REQUIRE(front_end.prepare(chroma_config));
    FrameCollector<decltype(front_end)::Frame, 1> frames;
    REQUIRE(front_end.process(channels, 1, size, std::ref(frames)));
    REQUIRE(frames.size == 1);
    REQUIRE(frames.frames[0].valid);
    const auto strongest = std::distance(
        frames.frames[0].magnitude.begin(),
        std::max_element(frames.frames[0].magnitude.begin(), frames.frames[0].magnitude.end()));
    CHECK(strongest == 9);
    double normalized_sum = 0.0;
    for (const auto value : frames.frames[0].normalized)
        normalized_sum += value;
    CHECK(normalized_sum == Catch::Approx(1.0).epsilon(1e-5));

    // Adoption parity with the previous built-in key kernel: double-authored
    // symmetric Hann, the shared float FFT, local peaks, and nearest MIDI PC.
    std::array<float, size> legacy_window{};
    std::array<std::complex<float>, size> legacy_spectrum{};
    for (std::size_t i = 0; i < size; ++i) {
        const auto phase =
            6.28318530717958647692 * static_cast<double>(i) / static_cast<double>(size - 1);
        const auto hann = 0.5 - 0.5 * std::cos(phase);
        legacy_window[i] = static_cast<float>(static_cast<double>(tone[i]) * hann);
    }
    pulp::signal::Fft legacy_fft(static_cast<int>(size));
    legacy_fft.forward_real(legacy_window.data(), legacy_spectrum.data());
    std::array<double, 12> legacy_chroma{};
    for (std::size_t bin = 1; bin < size / 2; ++bin) {
        const auto magnitude = static_cast<double>(std::abs(legacy_spectrum[bin]));
        if (magnitude < std::abs(legacy_spectrum[bin - 1]) ||
            magnitude < std::abs(legacy_spectrum[bin + 1]))
            continue;
        const auto frequency = static_cast<double>(bin) * chroma_config.sample_rate / size;
        if (frequency < 55.0 || frequency > 5000.0)
            continue;
        auto pitch_class =
            static_cast<int>(std::lround(69.0 + 12.0 * std::log2(frequency / 440.0))) % 12;
        if (pitch_class < 0)
            pitch_class += 12;
        legacy_chroma[static_cast<std::size_t>(pitch_class)] += magnitude;
    }
    for (std::size_t pc = 0; pc < legacy_chroma.size(); ++pc)
        CHECK(frames.frames[0].magnitude[pc] == Catch::Approx(legacy_chroma[pc]).margin(1e-4));
}

TEST_CASE("finite faults recover on a full clean window with absolute time",
          "[signal][analysis-frontends]") {
    std::array<float, 20> input{};
    input[2] = std::numeric_limits<float>::quiet_NaN();
    const float* channels[] = {input.data()};
    OnsetNoveltyFrontEndT<float, 16, 1> front_end;
    REQUIRE(front_end.prepare(config(8, 8), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(front_end)::Frame, 4> frames;
    CHECK_FALSE(front_end.process(channels, 1, input.size(), std::ref(frames)));
    REQUIRE(frames.size == 2);
    CHECK(frames.frames[0].window_start_frame == 3);
    CHECK(frames.frames[0].ready_at_frame == 10);
    CHECK(frames.frames[1].ready_at_frame == 18);
}

TEST_CASE("invalid channel topology makes no partial progress", "[signal][analysis-frontends]") {
    std::array<float, 8> input{};
    const float* null_channels[] = {nullptr};
    const float* channels[] = {input.data()};
    OnsetNoveltyFrontEndT<float, 8, 1> front_end;
    REQUIRE(front_end.prepare(config(8, 8), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(front_end)::Frame, 1> frames;
    CHECK_FALSE(front_end.process(null_channels, 1, 4, std::ref(frames)));
    CHECK(frames.size == 0);
    REQUIRE(front_end.process(channels, 1, input.size(), std::ref(frames)));
    REQUIRE(frames.size == 1);
    CHECK(frames.frames[0].window_start_frame == 0);
    CHECK(frames.frames[0].ready_at_frame == 7);
}

TEST_CASE("extreme finite energy remains finite on double-long-double peers",
          "[signal][analysis-frontends]") {
    std::array<float, 16> input{};
    std::fill(input.begin() + 8, input.end(), std::numeric_limits<float>::max());
    const float* channels[] = {input.data()};
    OnsetNoveltyFrontEndT<float, 16, 1> front_end;
    REQUIRE(front_end.prepare(config(8, 8), OnsetDetectionMethod::EnergyFlux));
    FrameCollector<decltype(front_end)::Frame, 2> frames;
    REQUIRE(front_end.process(channels, 1, input.size(), std::ref(frames)));
    REQUIRE(frames.size == 2);
    CHECK(std::isfinite(frames.frames[1].novelty));
    CHECK(frames.frames[1].novelty > 1.0e70);
}

TEST_CASE("extreme finite sample rates do not overflow chroma frequency math",
          "[signal][analysis-frontends]") {
    std::array<double, 8> input{};
    const double* channels[] = {input.data()};
    ChromaFrontEndT<double, 8, 1> front_end;
    auto extreme = config(8, 8);
    extreme.sample_rate = std::numeric_limits<double>::max();
    REQUIRE(front_end.prepare(extreme));
    FrameCollector<decltype(front_end)::Frame, 1> frames;
    REQUIRE(front_end.process(channels, 1, input.size(), std::ref(frames)));
    REQUIRE(frames.size == 1);
    for (const auto value : frames.frames[0].magnitude)
        CHECK(std::isfinite(value));
    for (const auto value : frames.frames[0].normalized)
        CHECK(std::isfinite(value));
}

TEST_CASE("spectral frontends mix cancelling extreme finite doubles without power overflow",
          "[signal][analysis-frontends]") {
    std::array<double, 8> positive{};
    std::array<double, 8> negative{};
    positive.fill(std::numeric_limits<double>::max());
    negative.fill(-std::numeric_limits<double>::max());
    const double* channels[] = {positive.data(), negative.data()};

    auto stereo = config(8, 8);
    stereo.channels = 2;
    ChromaFrontEndT<double, 8, 2> chroma;
    REQUIRE(chroma.prepare(stereo));
    FrameCollector<decltype(chroma)::Frame, 1> chroma_frames;
    REQUIRE(chroma.process(channels, 2, positive.size(), std::ref(chroma_frames)));
    REQUIRE(chroma_frames.size == 1);
    CHECK_FALSE(chroma_frames.frames[0].valid);

    OnsetNoveltyFrontEndT<double, 8, 2> spectral;
    REQUIRE(spectral.prepare(stereo, OnsetDetectionMethod::SpectralFlux));
    FrameCollector<decltype(spectral)::Frame, 1> spectral_frames;
    REQUIRE(spectral.process(channels, 2, positive.size(), std::ref(spectral_frames)));
    REQUIRE(spectral_frames.size == 1);
    CHECK(spectral_frames.frames[0].novelty == 0.0);
}

TEST_CASE("prepare is transactional and processing allocates nothing",
          "[signal][analysis-frontends]") {
    std::array<float, 16> input{};
    const float* channels[] = {input.data()};
    OnsetNoveltyFrontEndT<double, 16, 1> double_front_end;
    auto valid = config(8, 4);
    REQUIRE(double_front_end.prepare(valid, OnsetDetectionMethod::SpectralFlux));
    auto rejected = valid;
    rejected.max_retained_fft_bytes = 1;
    CHECK_FALSE(double_front_end.prepare(rejected, OnsetDetectionMethod::SpectralFlux));

    OnsetNoveltyFrontEndT<float, 16, 1> energy_only;
    auto non_spectral = config(10, 7);
    non_spectral.max_retained_fft_bytes = 0;
    CHECK(energy_only.prepare(non_spectral, OnsetDetectionMethod::EnergyFlux));
    CHECK(energy_only.retained_fft_bytes() == 0);

    std::array<double, 16> double_input{};
    const double* double_channels[] = {double_input.data()};
    FrameCollector<decltype(double_front_end)::Frame, 4> double_frames;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(double_front_end.process(double_channels, 1, double_input.size(),
                                         std::ref(double_frames)));
        CHECK(probe.allocation_count() == 0);
    }

    ChromaFrontEndT<float, 16, 1> chroma;
    REQUIRE(chroma.prepare(valid));
    FrameCollector<decltype(chroma)::Frame, 4> chroma_frames;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(chroma.process(channels, 1, input.size(), std::ref(chroma_frames)));
        CHECK(probe.allocation_count() == 0);
    }
}
