#include "support/sample_interpolation_render.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using namespace pulp::audio;
using namespace pulp::test::audio;

namespace {

constexpr int kAnalysisFrames = 16384;
constexpr int kWantedBin = 521;
constexpr double kWantedCycles =
    static_cast<double>(kWantedBin) / static_cast<double>(kAnalysisFrames);
constexpr std::array<int, 3> kPassbandBins = {127, kWantedBin, 1021};
constexpr std::array<double, 9> kRatios = {
    0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0,
};
constexpr std::array<SampleInterpolationPolicy, 6> kPolicies = {
    SampleInterpolationPolicy::Hold,
    SampleInterpolationPolicy::Nearest,
    SampleInterpolationPolicy::Linear,
    SampleInterpolationPolicy::CubicHermite,
    SampleInterpolationPolicy::CubicLagrange,
    SampleInterpolationPolicy::RatioTrackingSinc,
};

Buffer<float> to_buffer(std::span<const double> samples) {
    Buffer<float> buffer(1, samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
        buffer.channel(0)[i] = static_cast<float>(samples[i]);
    return buffer;
}

double component_db(std::span<const double> samples, double cycles_per_sample,
                    double reference_amplitude = kSamplerQualityAmplitude) {
    const auto amplitude = fit_tone(samples, cycles_per_sample).amplitude;
    if (!(amplitude > 0.0)) return kSilenceFloorDb;
    return 20.0 * std::log10(amplitude / reference_amplitude);
}

double worst_unexpected_db(std::span<const double> samples,
                           std::span<const double> expected_cycles,
                           double* worst_cycles = nullptr) {
    auto buffer = to_buffer(samples);
    ResponseOptions options;
    options.fft_length = static_cast<int>(samples.size());
    options.window = Window::rectangular;
    const auto curve = magnitude_spectrum_curve(
        std::as_const(buffer).view(), 1.0, {}, options);

    double worst = kSilenceFloorDb;
    double location = 0.0;
    for (std::size_t bin = 4; bin + 4 < curve.full.size(); ++bin) {
        bool excluded = false;
        for (const auto cycles : expected_cycles) {
            const auto expected_bin = cycles * static_cast<double>(samples.size());
            if (std::abs(static_cast<double>(bin) - expected_bin) <= 2.0) {
                excluded = true;
                break;
            }
        }
        if (!excluded && curve.full[bin].magnitude_db > worst) {
            worst = curve.full[bin].magnitude_db;
            location = curve.full[bin].hz;
        }
    }
    if (worst_cycles) *worst_cycles = location;
    return worst;
}

double first_image_cycles(double ratio, double source_cycles) {
    return fold_frequency(ratio * (1.0 - source_cycles), 1.0);
}

double downsample_stopband_cycles(double ratio) {
    if (ratio == 1.25) return 0.48;             // raw output 0.60 -> 0.40
    if (ratio == 1.5) return 7.0 / 15.0;        // raw output 0.70 -> 0.30
    if (ratio == 2.0) return 0.40;              // raw output 0.80 -> 0.20
    if (ratio == 3.0) return 7.0 / 15.0;        // raw output 1.40 -> 0.40
    return 0.45;                                // ratio 4: 1.80 -> 0.20
}

} // namespace

TEST_CASE("Sampler interpolation reports swept fixed-ratio tone quality",
          "[audio][sampler][interpolation][quality]") {
    for (const auto policy : kPolicies) {
        for (const auto ratio : kRatios) {
            for (const auto wanted_bin : kPassbandBins) {
                DYNAMIC_SECTION(
                    pulp::audio::sample_interpolation_policy_id(policy)
                    << " ratio=" << ratio << " wanted-bin=" << wanted_bin) {
                const auto wanted_cycles =
                    static_cast<double>(wanted_bin) / kAnalysisFrames;
                const auto source_cycles = wanted_cycles / ratio;
                const auto rendered = render_interpolated_tone(
                    policy, ratio, source_cycles, kAnalysisFrames, 64);
                const auto gain_db = tone_gain_db(
                    rendered, wanted_cycles, kSamplerQualityAmplitude);
                const auto residual_db = tone_residual_db(rendered, wanted_cycles);
                double unexpected_cycles = 0.0;
                const std::array expected = {wanted_cycles};
                const auto unexpected_db = worst_unexpected_db(
                    rendered, expected, &unexpected_cycles);

                CAPTURE(pulp::audio::sample_interpolation_policy_id(policy),
                        ratio,
                        source_cycles, wanted_cycles, gain_db, residual_db,
                        unexpected_db, unexpected_cycles);
                REQUIRE(std::isfinite(gain_db));
                REQUIRE(std::isfinite(residual_db));
                REQUIRE(std::isfinite(unexpected_db));

                // The polynomial tiers are characterized here, not advertised
                // as anti-alias filters. Only the ratio-tracking sinc owns the
                // strict raw-interpolator quality contract.
                if (policy == SampleInterpolationPolicy::RatioTrackingSinc) {
                    CHECK(std::abs(gain_db) < 0.10);
                    CHECK(residual_db < -70.0);
                    CHECK(unexpected_db < -75.0);
                }
              }
            }
        }
    }
}

TEST_CASE("Sampler sinc reports first image or downsample alias across ratios",
          "[audio][sampler][interpolation][quality][image]") {
    for (const auto ratio : kRatios) {
        DYNAMIC_SECTION("ratio=" << ratio) {
            if (ratio == 1.0) {
                const auto rendered = render_interpolated_tone(
                    SampleInterpolationPolicy::RatioTrackingSinc,
                    ratio, kWantedCycles, kAnalysisFrames, 64);
                CHECK(tone_residual_db(rendered, kWantedCycles) < -100.0);
                continue;
            }

            const bool downsampling = ratio > 1.0;
            const double source_cycles = downsampling
                ? downsample_stopband_cycles(ratio)
                : 0.2;
            const double wanted_cycles = fold_frequency(source_cycles * ratio, 1.0);
            const double image_cycles = downsampling
                ? wanted_cycles
                : first_image_cycles(ratio, source_cycles);
            const auto rendered = render_interpolated_tone(
                SampleInterpolationPolicy::RatioTrackingSinc,
                ratio, source_cycles, kAnalysisFrames, 64);
            const auto image_db = component_db(rendered, image_cycles);

            CAPTURE(ratio, source_cycles, wanted_cycles, image_cycles, image_db);
            REQUIRE(std::isfinite(image_db));
            if (downsampling) {
                // This tone is above the ratio-scaled cutoff and the measured
                // output component is its fold-back alias.
                CHECK(image_db < -55.0);
            } else {
                // The wanted tone remains in band while the first reconstructed
                // sampling image is independently measurable.
                const auto wanted_db = component_db(rendered, wanted_cycles);
                CAPTURE(wanted_db);
                CHECK(std::abs(wanted_db) < 0.10);
                CHECK(image_db < -55.0);
            }
        }
    }
}

TEST_CASE("Dense sampler sinc bank closes the fractional-ratio rejection hole",
          "[audio][sampler][interpolation][quality][image][negative-control]") {
    constexpr double ratio = 1.25;
    constexpr double source_cycles = 0.48;
    constexpr double image_cycles = 0.40;

    SampleSincKernelBank legacy_bank;
    REQUIRE(legacy_bank.build_for_maximum_consumption(4.0, 16));
    const PreparedSampleInterpolation legacy{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = legacy_bank.view().select(ratio),
    };
    const auto legacy_render = render_interpolated_tone_prepared(
        legacy, ratio, source_cycles, kAnalysisFrames);
    const auto legacy_image_db = component_db(legacy_render, image_cycles);

    SampleSincKernelBank dense_bank;
    REQUIRE(dense_bank.build_dense_for_maximum_consumption(4.0));
    const PreparedSampleInterpolation dense{
        .policy = SampleInterpolationPolicy::RatioTrackingSinc,
        .sinc = dense_bank.view().select(ratio),
    };
    const auto dense_render = render_interpolated_tone_prepared(
        dense, ratio, source_cycles, kAnalysisFrames);
    const auto dense_image_db = component_db(dense_render, image_cycles);

    CAPTURE(legacy_image_db, dense_image_db);
    CHECK(legacy_image_db > -20.0); // same analyzer must expose the old failure
    CHECK(dense_image_db < -55.0);  // and pass on the production-quality bank
}

TEST_CASE("Sampler quality analyzer controls prove its local floor",
          "[audio][sampler][interpolation][quality][analyzer-control]") {
    std::vector<double> pure(kAnalysisFrames);
    for (int frame = 0; frame < kAnalysisFrames; ++frame) {
        pure[static_cast<std::size_t>(frame)] = kSamplerQualityAmplitude *
            std::sin(2.0 * std::numbers::pi * kWantedCycles * frame + 0.37);
    }

    const auto pure_residual_db = tone_residual_db(pure, kWantedCycles);
    double pure_worst_cycles = 0.0;
    const std::array expected = {kWantedCycles};
    const auto pure_unexpected_db = worst_unexpected_db(
        pure, expected, &pure_worst_cycles);
    CAPTURE(pure_residual_db, pure_unexpected_db, pure_worst_cycles);
    CHECK(pure_residual_db < -140.0);
    CHECK(pure_unexpected_db < -120.0);

    constexpr int injected_bin = 3101;
    constexpr double injected_cycles =
        static_cast<double>(injected_bin) / kAnalysisFrames;
    for (const double injected_db : {-80.0, -100.0}) {
        auto injected = pure;
        const auto injected_amplitude = kSamplerQualityAmplitude *
            std::pow(10.0, injected_db / 20.0);
        for (int frame = 0; frame < kAnalysisFrames; ++frame) {
            injected[static_cast<std::size_t>(frame)] += injected_amplitude *
                std::sin(2.0 * std::numbers::pi * injected_cycles * frame + 1.1);
        }
        const auto measured_db = component_db(injected, injected_cycles);
        double worst_cycles = 0.0;
        const auto unexpected_db = worst_unexpected_db(
            injected, expected, &worst_cycles);
        CAPTURE(injected_db, measured_db, unexpected_db, worst_cycles);
        CHECK(std::abs(measured_db - injected_db) < 0.5);
        CHECK(std::abs(unexpected_db - injected_db) < 0.5);
        CHECK(std::abs(worst_cycles - injected_cycles) <= 1.0 / kAnalysisFrames);
    }

    auto quieter = pure;
    constexpr double attenuation_db = -0.5;
    const auto scale = std::pow(10.0, attenuation_db / 20.0);
    for (auto& sample : quieter) sample *= scale;
    CHECK(std::abs(tone_gain_db(quieter, kWantedCycles,
                               kSamplerQualityAmplitude) - attenuation_db) < 1e-6);
}

TEST_CASE("Fixed-ratio interpolation is invariant to render block partition",
          "[audio][sampler][interpolation][quality][blocks]") {
    constexpr std::array<std::size_t, 3> blocks = {1, 64, 511};
    for (const auto policy : kPolicies) {
        for (const auto ratio : kRatios) {
            DYNAMIC_SECTION(pulp::audio::sample_interpolation_policy_id(policy)
                            << " ratio=" << ratio) {
                const auto source_cycles = kWantedCycles / ratio;
                const auto reference = render_interpolated_tone(
                    policy, ratio, source_cycles, 4096, blocks.front());
                for (const auto block : std::span(blocks).subspan(1)) {
                    const auto candidate = render_interpolated_tone(
                        policy, ratio, source_cycles, 4096, block);
                    REQUIRE(candidate == reference);
                }
            }
        }
    }
}
