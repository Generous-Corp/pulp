#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/spectral_cross_synthesis.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <limits>

using namespace pulp::signal;

namespace {
constexpr int kFftSize = 256;
constexpr int kBins = kFftSize / 2 + 1;
constexpr double kPi = 3.14159265358979323846;

SpectralCrossSynthesis64 prepared(int order = 8, int channels = 1) {
    SpectralCrossSynthesis64 processor;
    SpectralCrossSynthesisPrepareConfig64 prepare;
    prepare.channels = channels;
    prepare.fft_size = kFftSize;
    prepare.lifter_order = order;
    prepare.true_envelope_iterations = 0;
    REQUIRE(processor.prepare(prepare));
    SpectralCrossSynthesisConfig64 config;
    config.normalization = SpectralCrossSynthesisNormalization::none;
    REQUIRE(processor.set_config(config));
    return processor;
}

double magnitude(std::complex<double> value) {
    return std::abs(value);
}
} // namespace

TEST_CASE("Default float cross synthesis processes without realtime allocation",
          "[signal][spectral-cross-synthesis][float][rt]") {
    SpectralCrossSynthesis processor;
    SpectralCrossSynthesisPrepareConfig prepare;
    prepare.fft_size = kFftSize;
    prepare.lifter_order = 0;
    prepare.true_envelope_iterations = 0;
    REQUIRE(processor.prepare(prepare));
    auto config = processor.config();
    config.normalization = SpectralCrossSynthesisNormalization::none;
    REQUIRE(processor.set_config(config));

    std::array<std::complex<float>, kBins> carrier{}, modulator{}, output{};
    carrier.fill({2.0f, 0.0f});
    modulator.fill({4.0f, 0.0f});
    const std::complex<float>* carrier_view[] = {carrier.data()};
    const std::complex<float>* modulator_view[] = {modulator.data()};
    std::complex<float>* output_view[] = {output.data()};

    bool processed = false;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        processed = processor.process(carrier_view, modulator_view, output_view, 1, kBins);
        allocations = probe.allocation_count();
    }
    REQUIRE(processed);
    REQUIRE(allocations == 0);
    REQUIRE(std::abs(output[17]) == Catch::Approx(4.0f).margin(2e-5f));
}

TEST_CASE("Cross synthesis matches an analytic DFT and cepstral envelope oracle",
          "[signal][spectral-cross-synthesis][oracle][formant]") {
    auto processor = prepared();
    std::array<std::complex<double>, kBins> carrier{}, modulator{}, output{};
    for (int bin = 0; bin < kBins; ++bin) {
        const double angle = 2.0 * kPi * static_cast<double>(bin) / kFftSize;
        const double carrier_log = 0.2 * std::cos(2.0 * angle) + 0.1 * std::cos(21.0 * angle);
        const double modulator_log =
            -0.1 + 0.35 * std::cos(3.0 * angle) + 0.08 * std::cos(25.0 * angle);
        const bool endpoint = bin == 0 || bin == kBins - 1;
        carrier[bin] = std::polar(std::exp(carrier_log), endpoint ? 0.0 : 0.013 * bin);
        modulator[bin] = std::polar(std::exp(modulator_log), endpoint ? 0.0 : -0.021 * bin);
    }
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    std::complex<double>* output_view[] = {output.data()};
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));

    for (int bin = 0; bin < kBins; ++bin) {
        const double angle = 2.0 * kPi * static_cast<double>(bin) / kFftSize;
        const double expected_log =
            -0.1 + 0.35 * std::cos(3.0 * angle) + 0.1 * std::cos(21.0 * angle);
        REQUIRE(magnitude(output[bin]) == Catch::Approx(std::exp(expected_log)).margin(2e-12));
        if (bin > 0 && bin < kBins - 1) {
            const auto gain = output[bin] / carrier[bin];
            REQUIRE(gain.real() ==
                    Catch::Approx(magnitude(output[bin]) / magnitude(carrier[bin])).margin(2e-12));
            REQUIRE(gain.imag() == Catch::Approx(0.0).margin(2e-12));
        }
    }
    // Raw modulator magnitudes retain q=25; a wrong-source transfer retains the
    // carrier q=2 envelope. Both deliberately disagree with this oracle.
}

TEST_CASE("Cross synthesis channel partitions and corresponding in-place aliases agree",
          "[signal][spectral-cross-synthesis][partition][in-place]") {
    auto grouped = prepared(8, 2);
    auto carrier_alias = prepared(8, 2);
    auto modulator_alias = prepared(8, 2);
    auto channel_zero = prepared();
    auto channel_one = prepared();
    std::array<std::array<std::complex<double>, kBins>, 2> carrier{}, modulator{}, output{};
    for (int ch = 0; ch < 2; ++ch) {
        for (int bin = 0; bin < kBins; ++bin) {
            const bool endpoint = bin == 0 || bin == kBins - 1;
            const double carrier_magnitude = 0.25 + 0.006 * (bin + 3 * ch);
            const double modulator_magnitude = 0.4 + 0.004 * ((5 * bin + 7 * ch) % 37);
            carrier[ch][bin] =
                std::polar(carrier_magnitude, endpoint ? 0.0 : 0.013 * (bin + ch));
            modulator[ch][bin] =
                std::polar(modulator_magnitude, endpoint ? 0.0 : -0.017 * (bin + 2 * ch));
        }
    }

    const std::complex<double>* carrier_view[] = {carrier[0].data(), carrier[1].data()};
    const std::complex<double>* modulator_view[] = {modulator[0].data(), modulator[1].data()};
    std::complex<double>* output_view[] = {output[0].data(), output[1].data()};
    REQUIRE(grouped.process(carrier_view, modulator_view, output_view, 2, kBins));

    for (int ch = 0; ch < 2; ++ch) {
        std::array<std::complex<double>, kBins> partitioned{};
        const std::complex<double>* partition_carrier[] = {carrier[ch].data()};
        const std::complex<double>* partition_modulator[] = {modulator[ch].data()};
        std::complex<double>* partition_output[] = {partitioned.data()};
        auto& processor = ch == 0 ? channel_zero : channel_one;
        REQUIRE(processor.process(partition_carrier, partition_modulator, partition_output, 1,
                                  kBins));
        REQUIRE(partitioned == output[ch]);
    }

    auto carrier_in_place = carrier;
    const std::complex<double>* carrier_in_place_view[] = {
        carrier_in_place[0].data(), carrier_in_place[1].data()};
    std::complex<double>* carrier_output_view[] = {
        carrier_in_place[0].data(), carrier_in_place[1].data()};
    REQUIRE(carrier_alias.process(carrier_in_place_view, modulator_view, carrier_output_view, 2,
                                  kBins));
    REQUIRE(carrier_in_place == output);

    auto modulator_in_place = modulator;
    const std::complex<double>* modulator_in_place_view[] = {
        modulator_in_place[0].data(), modulator_in_place[1].data()};
    std::complex<double>* modulator_output_view[] = {
        modulator_in_place[0].data(), modulator_in_place[1].data()};
    REQUIRE(modulator_alias.process(carrier_view, modulator_in_place_view, modulator_output_view,
                                    2, kBins));
    REQUIRE(modulator_in_place == output);
}

TEST_CASE("Cross synthesis rejects invalid frame boundaries without mutation",
          "[signal][spectral-cross-synthesis][bounds]") {
    auto processor = prepared();
    std::array<std::complex<double>, kBins> carrier{}, modulator{}, output{};
    carrier.fill({1.0, 0.0});
    modulator.fill({2.0, 0.0});
    output.fill({7.0, -3.0});
    const auto sentinel = output;
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    const std::complex<double>* null_input[] = {nullptr};
    std::complex<double>* output_view[] = {output.data()};
    std::complex<double>* null_output[] = {nullptr};

    REQUIRE_FALSE(processor.process(nullptr, modulator_view, output_view, 1, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, nullptr, output_view, 1, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, nullptr, 1, kBins));
    REQUIRE_FALSE(processor.process(null_input, modulator_view, output_view, 1, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, null_input, output_view, 1, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, null_output, 1, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, output_view, 0, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, output_view, 2, kBins));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, output_view, 1, 0));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, output_view, 1, kBins - 1));
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, output_view, 1, kBins + 1));
    REQUIRE(output == sentinel);
}

TEST_CASE("Cross synthesis amount mix normalization and carrier phase are independent",
          "[signal][spectral-cross-synthesis][controls][phase]") {
    auto processor = prepared(0);
    std::array<std::complex<double>, kBins> carrier{}, modulator{}, output{};
    carrier.fill({2.0, 0.0});
    modulator.fill({8.0, 0.0});
    carrier[17] = {1.2, 1.6};
    modulator[17] = {-8.0, 0.0};
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    std::complex<double>* output_view[] = {output.data()};

    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[17]) == Catch::Approx(8.0).margin(1e-12));
    REQUIRE(output[17].imag() / output[17].real() == Catch::Approx(4.0 / 3.0));

    auto config = processor.config();
    config.mix = 0.25;
    REQUIRE(processor.set_config(config));
    processor.reset();
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[17]) == Catch::Approx(3.5).margin(1e-12));

    config.mix = 1.0;
    config.normalization = SpectralCrossSynthesisNormalization::match_carrier_rms;
    REQUIRE(processor.set_config(config));
    processor.reset();
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[17]) == Catch::Approx(2.0).margin(1e-12));

    config.amount = 0.0;
    config.normalization = SpectralCrossSynthesisNormalization::none;
    config.magnitude_floor = 0.5;
    REQUIRE(processor.set_config(config));
    processor.reset();
    carrier.fill({1.0e-9, 0.0});
    modulator.fill({1.0, 0.0});
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[17]) == Catch::Approx(1.0e-9).margin(1e-20));
}

TEST_CASE("Cross synthesis normalization keeps representable blends finite",
          "[signal][spectral-cross-synthesis][normalization][fault]") {
    auto processor = prepared(kFftSize / 2);
    auto config = processor.config();
    config.mix = 0.01;
    config.maximum_transfer_db = 120.0;
    config.normalization = SpectralCrossSynthesisNormalization::match_carrier_rms;
    REQUIRE(processor.set_config(config));

    const double maximum = std::numeric_limits<double>::max();
    const double dry = maximum * 0.25;
    const double wet_peak = maximum * 0.5;
    std::array<std::complex<double>, kBins> carrier{}, modulator{}, output{};
    carrier.fill({dry, 0.0});
    modulator.fill({1.0, 0.0});
    modulator[17] = {wet_peak, 0.0};
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    std::complex<double>* output_view[] = {output.data()};
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));

    const double low_wet_over_peak = 0.5e-6;
    const double normalization =
        0.5 * std::sqrt(static_cast<double>(kBins) /
                        (1.0 + (kBins - 1) * low_wet_over_peak * low_wet_over_peak));
    const double expected_over_maximum = 0.99 * 0.25 + 0.01 * 0.5 * normalization;
    REQUIRE(std::isfinite(output[17].real()));
    REQUIRE(output[17].real() / maximum ==
            Catch::Approx(expected_over_maximum).epsilon(2e-12));

    config.mix = 0.0;
    REQUIRE(processor.set_config(config));
    processor.reset();
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(output[17] == carrier[17]);
}

TEST_CASE("Cross synthesis envelope history has a deterministic frame time constant",
          "[signal][spectral-cross-synthesis][history][oracle]") {
    auto processor = prepared(0);
    auto config = processor.config();
    config.envelope_smoothing_frames = 4;
    REQUIRE(processor.set_config(config));
    std::array<std::complex<double>, kBins> carrier{}, modulator{}, output{};
    carrier.fill({1.0, 0.0});
    modulator.fill({4.0, 0.0});
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    std::complex<double>* output_view[] = {output.data()};
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[7]) == Catch::Approx(4.0).margin(1e-12));
    modulator.fill({1.0, 0.0});
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[7]) ==
            Catch::Approx(std::exp(std::exp(-0.25) * std::log(4.0))).margin(2e-12));
    processor.reset();
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(magnitude(output[7]) == Catch::Approx(1.0).margin(1e-12));
}

TEST_CASE("Cross synthesis is transactional finite and self conjugate",
          "[signal][spectral-cross-synthesis][lifecycle][fault]") {
    auto processor = prepared();
    const auto before = processor.config();
    auto invalid = before;
    invalid.amount = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(processor.set_config(invalid));
    REQUIRE(processor.config().amount == before.amount);
    auto invalid_prepare = SpectralCrossSynthesisPrepareConfig64{};
    invalid_prepare.fft_size = 130;
    REQUIRE_FALSE(processor.prepare(invalid_prepare));
    REQUIRE(processor.fft_size() == kFftSize);

    std::array<std::complex<double>, kBins> carrier{}, modulator{}, output{};
    carrier.fill({1.0, 0.0});
    modulator.fill({1.0, 0.0});
    carrier[0] = {-2.0, 9.0};
    carrier[kBins - 1] = {3.0, -7.0};
    carrier[9] = {std::numeric_limits<double>::quiet_NaN(), 1.0};
    modulator[11] = {std::numeric_limits<double>::infinity(), 0.0};
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    std::complex<double>* output_view[] = {output.data()};
    REQUIRE(processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(output[0].imag() == 0.0);
    REQUIRE(output[kBins - 1].imag() == 0.0);
    REQUIRE(output[9] == std::complex<double>{});
    for (const auto value : output) {
        REQUIRE(std::isfinite(value.real()));
        REQUIRE(std::isfinite(value.imag()));
    }

    auto overflow_processor = prepared(0);
    const double maximum = std::numeric_limits<double>::max();
    carrier.fill({1.0, 0.0});
    modulator.fill({maximum * 0.25, 0.0});
    carrier[17] = {maximum * 0.6, maximum * 0.6};
    REQUIRE(overflow_processor.process(carrier_view, modulator_view, output_view, 1, kBins));
    REQUIRE(std::isfinite(output[17].real()));
    REQUIRE(std::isfinite(output[17].imag()));
    REQUIRE(output[17].real() == Catch::Approx(output[17].imag()));
    REQUIRE(magnitude(output[17]) > maximum * 0.999999);
    // Multiplying the raw complex carrier by the target magnitude overflows;
    // the unit-phase path saturates while preserving phase.
    const auto sentinel = output;
    REQUIRE_FALSE(processor.process(carrier_view, modulator_view, output_view, 1, kBins - 1));
    REQUIRE(output == sentinel);
    REQUIRE(SpectralCrossSynthesis64::added_latency_samples() == 0);
    REQUIRE(SpectralCrossSynthesis64::added_tail_samples() == 0);
}

TEST_CASE("Cross synthesis frame sequences replay deterministically without allocation",
          "[signal][spectral-cross-synthesis][determinism][rt]") {
    auto processor = prepared(6);
    std::array<std::complex<double>, kBins> carrier{}, modulator{};
    std::array<std::array<std::complex<double>, kBins>, 3> first{}, replay{};
    const std::complex<double>* carrier_view[] = {carrier.data()};
    const std::complex<double>* modulator_view[] = {modulator.data()};
    auto fill = [&](int frame) {
        for (int bin = 0; bin < kBins; ++bin) {
            const bool endpoint = bin == 0 || bin == kBins - 1;
            carrier[bin] = std::polar(1.0 + 0.003 * bin, endpoint ? 0.0 : 0.02 * (bin + frame));
            modulator[bin] = {0.5 + 0.01 * ((bin + frame * 7) % 31), 0.0};
        }
    };
    for (int frame = 0; frame < 3; ++frame) {
        fill(frame);
        std::complex<double>* first_view[] = {first[static_cast<std::size_t>(frame)].data()};
        REQUIRE(processor.process(carrier_view, modulator_view, first_view, 1, kBins));
    }
    processor.reset();
    std::size_t allocations = 0;
    bool processed = true;
    {
        pulp::test::RtAllocationProbe probe;
        for (int frame = 0; frame < 3; ++frame) {
            fill(frame);
            std::complex<double>* replay_view[] = {replay[static_cast<std::size_t>(frame)].data()};
            processed =
                processor.process(carrier_view, modulator_view, replay_view, 1, kBins) && processed;
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(processed);
    REQUIRE(allocations == 0);
    REQUIRE(replay == first);
}
