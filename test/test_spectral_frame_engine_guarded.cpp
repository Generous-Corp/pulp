#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
using pulp::signal::SpectralFrameEngine;
using pulp::signal::SpectralFrameEngineConfig;
using pulp::signal::SpectralSplitResult;

SpectralFrameEngineConfig config() {
    SpectralFrameEngineConfig c;
    c.fft_size = 256;
    c.analysis_hop = 63;
    c.channels = 2;
    c.max_block = 4096;
    c.defer_synthesis = false;
    c.guarded_split = true;
    return c;
}

std::array<std::vector<float>, 2> render(int partition) {
    const auto c = config();
    SpectralFrameEngine analysis, synthesis;
    analysis.prepare(c);
    synthesis.prepare(c);
    constexpr int length = 8192;
    std::array<std::vector<float>, 2> input, output;
    for (int ch = 0; ch < 2; ++ch) {
        input[ch].resize(length);
        output[ch].resize(length);
        for (int t = 0; t < 6000; ++t)
            input[ch][t] = static_cast<float>(0.15 * std::sin(t * (0.071 + ch * 0.017)));
        input[ch][0] = 0.75f;
    }
    for (int pos = 0; pos < length;) {
        const int count =
            std::min(length - pos, partition == 0 ? 1 + (pos * 17 % 4096) : partition);
        const float* in[] = {input[0].data() + pos, input[1].data() + pos};
        REQUIRE(analysis.analyze_guarded(in, count, [&](std::complex<float>* const* frames, int) {
            const std::complex<float>* ptrs[] = {frames[0], frames[1]};
            REQUIRE(synthesis.synthesize_frame_guarded(ptrs) == SpectralSplitResult::Ok);
        }) == SpectralSplitResult::Ok);
        const int prefix = std::clamp(synthesis.latency_samples() - pos, 0, count);
        float* out[] = {output[0].data() + pos + prefix, output[1].data() + pos + prefix};
        int consumed = -1;
        REQUIRE(synthesis.read_output_guarded(out, count - prefix, consumed) ==
                SpectralSplitResult::Ok);
        REQUIRE(consumed == count - prefix);
        pos += count;
    }
    for (int ch = 0; ch < 2; ++ch) {
        double signal = 0, error = 0;
        const int latency = c.fft_size + c.analysis_hop;
        for (int t = 0; t < latency; ++t)
            REQUIRE(output[ch][t] == 0.0f);
        for (int t = 0; t < length - latency; ++t) {
            const double delta = output[ch][t + latency] - input[ch][t];
            signal += input[ch][t] * input[ch][t];
            error += delta * delta;
        }
        REQUIRE(error / signal < 1e-10);
        REQUIRE(std::abs(output[ch][latency] - 0.75f) < 1e-3f);
    }
    return output;
}
} // namespace

TEST_CASE("Guarded split reconstructs with fixed latency across callback partitions",
          "[signal][spectral][guarded-split]") {
    const auto reference = render(64);
    REQUIRE(render(4096) == reference);
    REQUIRE(render(1) == reference);
    REQUIRE(render(0) == reference);
}

TEST_CASE("Guarded synthesis rejects an entire frame without changing prior history",
          "[signal][spectral][guarded-split][transaction]") {
    auto c = config();
    SpectralFrameEngine control, subject;
    control.prepare(c);
    subject.prepare(c);
    std::vector<std::complex<float>> bins(129, {0.0f, 0.0f});
    bins[3] = {4.0f, 2.0f};
    const std::complex<float>* frames[] = {bins.data(), bins.data()};
    for (int i = 0; i < 10; ++i) {
        REQUIRE(control.synthesize_frame_guarded(frames) == SpectralSplitResult::Ok);
        REQUIRE(subject.synthesize_frame_guarded(frames) == SpectralSplitResult::Ok);
    }
    auto bad = bins;
    bad[0] = {0.0f, 1.0f};
    const std::complex<float>* invalid[] = {bins.data(), bad.data()};
    REQUIRE(subject.synthesize_frame_guarded(invalid) == SpectralSplitResult::InvalidFrame);
    bad[0] = {0.0f, 0.0f};
    bad[5] = {std::numeric_limits<float>::infinity(), 0.0f};
    REQUIRE(subject.synthesize_frame_guarded(invalid) == SpectralSplitResult::InvalidFrame);
    std::fill(bad.begin(), bad.end(), std::complex<float>{std::numeric_limits<float>::max(), 0.0f});
    REQUIRE(subject.synthesize_frame_guarded(invalid) == SpectralSplitResult::NonfiniteSynthesis);
    REQUIRE(subject.available_output() == control.available_output());
    REQUIRE(subject.synthesize_frame_guarded(frames) == SpectralSplitResult::Ok);
    REQUIRE(control.synthesize_frame_guarded(frames) == SpectralSplitResult::Ok);
    const int available = control.available_output();
    std::array<std::vector<float>, 2> actual, expected;
    for (int ch = 0; ch < 2; ++ch) {
        actual[ch].resize(available + 1, 123.0f);
        expected[ch].resize(available + 1, 123.0f);
    }
    float* a[] = {actual[0].data(), actual[1].data()};
    float* b[] = {expected[0].data(), expected[1].data()};
    int read = 0;
    REQUIRE(subject.read_output_guarded(a, available + 1, read) ==
            SpectralSplitResult::OutputUnderrun);
    REQUIRE(read == available);
    REQUIRE(control.read_output_guarded(b, available, read) == SpectralSplitResult::Ok);
    REQUIRE(actual == expected);
    subject.reset();
    REQUIRE(subject.available_output() == 0);
}

TEST_CASE("Guarded split bounds storage and refuses invalid input before ingestion",
          "[signal][spectral][guarded-split][capacity]") {
    auto c = config();
    const auto guarded = pulp::signal::checked_spectral_frame_engine_geometry<float>(c);
    c.guarded_split = false;
    const auto legacy = pulp::signal::checked_spectral_frame_engine_geometry<float>(c);
    REQUIRE(guarded);
    REQUIRE(legacy);
    REQUIRE(guarded->retained_bytes - legacy->retained_bytes == 3 * 256 * sizeof(float));
    c.guarded_split = true;
    c.defer_synthesis = true;
    REQUIRE_FALSE(pulp::signal::checked_spectral_frame_engine_geometry<float>(c));
    c.defer_synthesis = false;
    SpectralFrameEngine engine;
    engine.prepare(c);
    int frames_seen = 0;
    auto observe = [&](auto, int) { ++frames_seen; };
    REQUIRE(engine.analyze_guarded(nullptr, 4097, observe) == SpectralSplitResult::InvalidBlock);
    REQUIRE(engine.analyze_guarded(nullptr, -1, observe) == SpectralSplitResult::InvalidBlock);
    REQUIRE(frames_seen == 0);
    REQUIRE(engine.samples_until_next_frame() == 4);
    std::vector<std::complex<float>> zeros(129);
    const std::complex<float>* frames[] = {zeros.data(), zeros.data()};
    int accepted = 0;
    while (engine.synthesize_frame_guarded(frames) == SpectralSplitResult::Ok)
        ++accepted;
    REQUIRE(accepted > 0);
    REQUIRE(accepted < guarded->ring_size);
    REQUIRE(engine.synthesize_frame_guarded(frames) == SpectralSplitResult::CapacityExceeded);
}
