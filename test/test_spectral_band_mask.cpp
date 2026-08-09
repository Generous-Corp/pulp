#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/spectral_band_mask.hpp>

#include <atomic>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::SpectralBandEdgePolicy;
using pulp::signal::SpectralBandLayout;
using pulp::signal::SpectralBandSpacing;
using pulp::signal::SpectralFrameEngine;
using pulp::signal::SpectralFrameEngineConfig;
using pulp::signal::SpectralMaskBoundaryKernel;
using pulp::signal::SpectralMaskTable;
using pulp::signal::apply_spectral_mask;
using pulp::signal::build_spectral_mask;

extern std::atomic<long> g_alloc_count;

namespace {

constexpr float kSampleRate = 1024.0f;
constexpr int kFftSize = 256;

SpectralBandLayout linear_layout(std::uint32_t bands = 4) {
    SpectralBandLayout layout;
    layout.active_bands = bands;
    layout.min_hz = 0.0f;
    layout.max_hz = kSampleRate * 0.5f;
    layout.spacing = SpectralBandSpacing::linear;
    layout.edge_policy = SpectralBandEdgePolicy::mute_outside;
    layout.boundary_kernel = SpectralMaskBoundaryKernel::hard;
    layout.transition_fraction = 0.0f;
    return layout;
}

} // namespace

TEST_CASE("spectral mask hard boundaries own DC Nyquist and exact right edges",
          "[signal][spectral-band-mask][oracle]") {
    auto layout = linear_layout();
    layout.bands[0].gain_db = 0.0f;
    layout.bands[1].muted = true;
    layout.bands[2].gain_db = 6.0f;
    layout.bands[3].gain_db = -6.0f;
    layout.version = 17;

    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE(table.num_bins == 129);
    REQUIRE(table.version == 17);
    REQUIRE_THAT(table.band_edges_hz[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(table.band_edges_hz[1], WithinAbs(128.0f, 1.0e-6f));
    REQUIRE_THAT(table.band_edges_hz[4], WithinAbs(512.0f, 1.0e-6f));

    REQUIRE_THAT(table.gain_linear[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(table.gain_linear[31], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(table.gain_linear[32] == 0.0f); // exact boundary belongs right
    REQUIRE(table.gain_linear[63] == 0.0f);
    REQUIRE_THAT(table.gain_linear[64], WithinAbs(std::pow(10.0f, 0.3f), 1.0e-6f));
    REQUIRE_THAT(table.gain_linear[128], WithinAbs(std::pow(10.0f, -0.3f), 1.0e-6f));
}

TEST_CASE("spectral mask zoom creates exact outside mute and frequency islands",
          "[signal][spectral-band-mask][isolation]") {
    auto layout = linear_layout(8);
    layout.min_hz = 128.0f;
    layout.max_hz = 384.0f;
    for (auto& band : layout.bands) band.muted = true;
    layout.bands[1].muted = false;
    layout.bands[5].muted = false;

    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE(table.gain_linear[0] == 0.0f);
    REQUIRE(table.gain_linear[31] == 0.0f);
    REQUIRE(table.gain_linear[33] == 0.0f);
    REQUIRE_THAT(table.gain_linear[44], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(table.gain_linear[52] == 0.0f);
    REQUIRE_THAT(table.gain_linear[76], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(table.gain_linear[97] == 0.0f);
    REQUIRE(table.gain_linear[128] == 0.0f);
}

TEST_CASE("spectral mask edge policy can extend the first and last bands",
          "[signal][spectral-band-mask][edge]") {
    auto layout = linear_layout(2);
    layout.min_hz = 128.0f;
    layout.max_hz = 384.0f;
    layout.edge_policy = SpectralBandEdgePolicy::extend_edge_band;
    layout.bands[0].gain_db = -6.0f;
    layout.bands[1].gain_db = 6.0f;

    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE_THAT(table.gain_linear[0], WithinAbs(std::pow(10.0f, -0.3f), 1.0e-6f));
    REQUIRE_THAT(table.gain_linear[128], WithinAbs(std::pow(10.0f, 0.3f), 1.0e-6f));
}

TEST_CASE("muted extended edge bands remove audio outside a zoomed viewport",
          "[signal][spectral-band-mask][edge][mute]") {
    auto layout = linear_layout(4);
    layout.min_hz = 128.0f;
    layout.max_hz = 384.0f;
    layout.edge_policy = SpectralBandEdgePolicy::extend_edge_band;
    layout.bands.front().muted = true;
    layout.bands[3].muted = true;

    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE(table.gain_linear[0] == 0.0f);
    REQUIRE_THAT(table.gain_linear[48], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(table.gain_linear[79], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(table.gain_linear[128] == 0.0f);
}

TEST_CASE("raised-cosine boundary is phase-neutral and halfway at the edge",
          "[signal][spectral-band-mask][kernel]") {
    auto layout = linear_layout(2);
    layout.boundary_kernel = SpectralMaskBoundaryKernel::raised_cosine;
    layout.transition_fraction = 0.25f;
    layout.bands[0].gain_db = 0.0f;
    layout.bands[1].muted = true;

    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE_THAT(table.gain_linear[64], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(table.gain_linear[48], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE(table.gain_linear[80] == 0.0f);
}

TEST_CASE("logarithmic layout derives stable octave boundaries and clamps Nyquist",
          "[signal][spectral-band-mask][mapping]") {
    SpectralBandLayout layout;
    layout.active_bands = 4;
    layout.min_hz = 100.0f;
    layout.max_hz = 3200.0f; // clamped to 2048 Hz at this sample rate
    layout.spacing = SpectralBandSpacing::logarithmic;
    layout.boundary_kernel = SpectralMaskBoundaryKernel::hard;
    layout.transition_fraction = 0.0f;

    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, 1024, 4096.0f, table));
    REQUIRE_THAT(table.effective_max_hz, WithinAbs(2048.0f, 1.0e-5f));
    REQUIRE_THAT(table.band_edges_hz[0], WithinAbs(100.0f, 1.0e-5f));
    REQUIRE_THAT(table.band_edges_hz[4], WithinAbs(2048.0f, 1.0e-3f));
    for (std::uint32_t i = 1; i <= 4; ++i)
        REQUIRE(table.band_edges_hz[i] > table.band_edges_hz[i - 1]);
}

TEST_CASE("spectral mask builder rejects hostile controls without changing output",
          "[signal][spectral-band-mask][fault]") {
    auto layout = linear_layout();
    SpectralMaskTable table;
    table.version = 999;
    table.gain_linear[3] = 0.25f;

    layout.active_bands = 65;
    REQUIRE_FALSE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE(table.version == 999);
    REQUIRE(table.gain_linear[3] == 0.25f);

    layout = linear_layout();
    layout.bands[2].gain_db = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    layout = linear_layout();
    layout.transition_fraction = 0.51f;
    REQUIRE_FALSE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    layout = linear_layout();
    layout.transition_frames =
        pulp::signal::kSpectralBandMaskMaximumTransitionFrames + 1;
    REQUIRE_FALSE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    layout = linear_layout();
    layout.spacing = static_cast<SpectralBandSpacing>(99);
    REQUIRE_FALSE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    layout = linear_layout();
    layout.spacing = SpectralBandSpacing::logarithmic;
    REQUIRE_FALSE(build_spectral_mask(layout, kFftSize, kSampleRate, table));
    REQUIRE_FALSE(build_spectral_mask(linear_layout(), 300, kSampleRate, table));
}

TEST_CASE("spectral mask application preserves phase and stereo ratios without allocation",
          "[signal][spectral-band-mask][rt]") {
    auto layout = linear_layout();
    layout.bands[0].gain_db = -6.0f;
    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));

    std::array<std::complex<float>, 129> left{};
    std::array<std::complex<float>, 129> right{};
    for (std::size_t bin = 0; bin < left.size(); ++bin) {
        left[bin] = {1.0f, 2.0f};
        right[bin] = {-3.0f, 4.0f};
    }
    std::complex<float>* frames[] = {left.data(), right.data()};
    const long before = g_alloc_count.load();
    const bool applied = apply_spectral_mask(frames, 2, 129, table);
    const long after = g_alloc_count.load();
    REQUIRE(applied);
    REQUIRE(after == before);

    const auto gain = std::pow(10.0f, -0.3f);
    REQUIRE_THAT(left[0].real(), WithinAbs(gain, 1.0e-6f));
    REQUIRE_THAT(left[0].imag(), WithinAbs(2.0f * gain, 1.0e-6f));
    REQUIRE_THAT(right[0].real(), WithinAbs(-3.0f * gain, 1.0e-6f));
    REQUIRE_THAT(right[0].imag(), WithinAbs(4.0f * gain, 1.0e-6f));

    const auto before_failure = left;
    table.fft_size = 512;
    REQUIRE_FALSE(apply_spectral_mask(frames, 2, 129, table));
    REQUIRE(left == before_failure);
}

TEST_CASE("spectral mask application contains non-finite and overflowing bins",
          "[signal][spectral-band-mask][fault]") {
    auto layout = linear_layout(1);
    layout.edge_policy = SpectralBandEdgePolicy::extend_edge_band;
    layout.bands[0].gain_db = 48.0f;
    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, kFftSize, kSampleRate, table));

    std::array<std::complex<float>, 129> bins{};
    bins[0] = {std::numeric_limits<float>::max(),
               -std::numeric_limits<float>::max()};
    bins[1] = {std::numeric_limits<float>::infinity(), 1.0f};
    std::complex<float>* frames[] = {bins.data()};
    REQUIRE(apply_spectral_mask(frames, 1, 129, table));
    REQUIRE(std::isfinite(bins[0].real()));
    REQUIRE(std::isfinite(bins[0].imag()));
    REQUIRE(bins[0].real() == std::numeric_limits<float>::max());
    REQUIRE(bins[0].imag() == -std::numeric_limits<float>::max());
    REQUIRE(bins[1] == std::complex<float>{});
}

TEST_CASE("SpectralFrameEngine composition turns an off-bin tone into exact silence",
          "[signal][spectral-band-mask][composition]") {
    constexpr int block = 127;
    constexpr int total = 12000;
    constexpr float sample_rate = 48000.0f;

    SpectralFrameEngineConfig config;
    config.fft_size = 1024;
    config.analysis_hop = 256;
    config.channels = 2;
    config.max_block = block;
    SpectralFrameEngine engine;
    engine.prepare(config);

    SpectralBandLayout layout;
    layout.active_bands = 64;
    layout.min_hz = 20.0f;
    layout.max_hz = 20000.0f;
    layout.spacing = SpectralBandSpacing::logarithmic;
    layout.boundary_kernel = SpectralMaskBoundaryKernel::hard;
    layout.transition_fraction = 0.0f;
    for (auto& band : layout.bands) band.muted = true;
    SpectralMaskTable table;
    REQUIRE(build_spectral_mask(layout, config.fft_size, sample_rate, table));

    std::array<std::vector<float>, 2> input = {
        std::vector<float>(block), std::vector<float>(block)};
    std::array<std::vector<float>, 2> output = {
        std::vector<float>(block), std::vector<float>(block)};
    const float* input_ptrs[] = {input[0].data(), input[1].data()};
    float* output_ptrs[] = {output[0].data(), output[1].data()};
    double energy = 0.0;

    for (int position = 0; position < total; position += block) {
        const int count = std::min(block, total - position);
        for (int i = 0; i < count; ++i) {
            const auto sample = 0.5f * std::sin(
                2.0f * 3.14159265358979323846f * 997.0f
                * static_cast<float>(position + i) / sample_rate);
            input[0][i] = sample;
            input[1][i] = -sample;
        }
        engine.process(input_ptrs, output_ptrs, count,
            [&](std::complex<float>* const* frames, int bins) {
                REQUIRE(apply_spectral_mask(frames, 2, bins, table));
            });
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < count; ++i)
                energy += output[channel][i] * output[channel][i];
    }
    REQUIRE(energy == 0.0);
}
