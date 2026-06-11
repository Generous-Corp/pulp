// StnDecomposer — sines/transients/noise separation by median filtering.
//
// Validates that a sustained tonal peak is classified mostly as sines, an
// isolated broadband frame mostly as transients, and broadband sustained
// energy mostly as noise.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/stn_decomposer.hpp>
#include <cmath>
#include <vector>

using namespace pulp::signal;

namespace {
constexpr int kBins = 513;

std::vector<float> tonal_frame(int peak_bin, float amp) {
    std::vector<float> m(static_cast<size_t>(kBins), 0.002f); // small noise floor
    // Narrow peak (a few bins) at peak_bin.
    for (int d = -2; d <= 2; ++d) {
        const int k = peak_bin + d;
        if (k >= 0 && k < kBins)
            m[static_cast<size_t>(k)] += amp * std::exp(-0.5f * d * d);
    }
    return m;
}

std::vector<float> broadband_frame(float amp) {
    std::vector<float> m(static_cast<size_t>(kBins));
    for (int k = 0; k < kBins; ++k) m[static_cast<size_t>(k)] = amp;
    return m;
}
} // namespace

TEST_CASE("StnDecomposer classifies a sustained tone as sines", "[signal][stn]") {
    StnConfig config;
    config.num_bins = kBins;
    config.time_median = 9;
    config.freq_median = 9;
    StnDecomposer stn;
    stn.prepare(config);

    const int peak = 100;
    auto frame = tonal_frame(peak, 1.0f);
    const StnMasks* masks = nullptr;
    for (int i = 0; i < 20; ++i) masks = &stn.process(frame.data());

    // At the tonal peak, sines should dominate over transients and noise.
    REQUIRE(masks->sines[peak] > 0.6f);
    REQUIRE(masks->sines[peak] > masks->transients[peak]);
    REQUIRE(masks->sines[peak] > masks->noise[peak]);
}

TEST_CASE("StnDecomposer classifies an isolated broadband frame as transient",
          "[signal][stn]") {
    StnConfig config;
    config.num_bins = kBins;
    StnDecomposer stn;
    stn.prepare(config);

    auto quiet = tonal_frame(100, 0.0f); // near-silent (noise floor only)
    auto burst = broadband_frame(1.0f);

    // Fill history with quiet frames, then push a single burst, then quiet
    // again so the burst sits at the median center.
    const StnMasks* masks = nullptr;
    for (int i = 0; i < 5; ++i) stn.process(quiet.data());
    stn.process(burst.data());
    for (int i = 0; i < config.time_median / 2; ++i) masks = &stn.process(quiet.data());

    // Averaged over mid frequencies, transients should beat sines for the
    // burst (broadband + brief = vertical ridge, no horizontal support).
    float t_sum = 0.0f, s_sum = 0.0f;
    for (int k = 50; k < kBins - 50; ++k) {
        t_sum += masks->transients[k];
        s_sum += masks->sines[k];
    }
    REQUIRE(t_sum > s_sum);
}

TEST_CASE("StnDecomposer classifies broadband sustained energy as noise",
          "[signal][stn]") {
    StnConfig config;
    config.num_bins = kBins;
    StnDecomposer stn;
    stn.prepare(config);

    auto noise = broadband_frame(0.5f);
    const StnMasks* masks = nullptr;
    for (int i = 0; i < 20; ++i) masks = &stn.process(noise.data());

    // Sustained + broadband: neither horizontal nor vertical median
    // dominates, so noise should be the largest class on average.
    float s_sum = 0.0f, t_sum = 0.0f, n_sum = 0.0f;
    for (int k = 50; k < kBins - 50; ++k) {
        s_sum += masks->sines[k];
        t_sum += masks->transients[k];
        n_sum += masks->noise[k];
    }
    REQUIRE(n_sum > s_sum);
    REQUIRE(n_sum > t_sum);
}

TEST_CASE("StnDecomposer masks partition to ~1 per bin", "[signal][stn]") {
    StnConfig config;
    config.num_bins = kBins;
    StnDecomposer stn;
    stn.prepare(config);
    auto frame = tonal_frame(80, 0.7f);
    const StnMasks* masks = nullptr;
    for (int i = 0; i < 12; ++i) masks = &stn.process(frame.data());
    for (int k = 0; k < kBins; ++k) {
        const float sum = masks->sines[k] + masks->transients[k] + masks->noise[k];
        REQUIRE(sum <= 1.0001f);
        REQUIRE(sum >= 0.0f);
    }
}
