// Focused closure tests for the multirate FDN prompt audit.
//
// Kept in a second translation unit so the main behavioral suite remains below
// 1,000 lines. These cases pin requirements that were absent from the landed
// suite: exact length rounding, narrow-mode lifetime, monotonic THD,
// intermediate non-finite recovery, and complete rate-switch state parity.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/fdn/diffusion.hpp>
#include <pulp/signal/fdn/loop_eq.hpp>
#include <pulp/signal/fdn/multirate.hpp>
#include <pulp/signal/fdn/shimmer.hpp>
#include <pulp/signal/fdn/stages.hpp>
#include <pulp/signal/fdn_reverb.hpp>
#include <pulp/signal/fft.hpp>

#include "support/fdn_reverb_fixture.hpp"
#include "support/reverb_metrics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

using pulp::signal::FdnReverb;
namespace fdn = pulp::signal::fdn;
using Param = FdnReverb::Param;
using pulp::test::audio::band_t60;
using namespace pulp::test::fdn_reverb;

namespace {

struct ModalDecaySummary {
    std::size_t compared_bins = 0;
    double worst_ratio = 0.0;
};

struct DifferenceSummary {
    std::size_t first = 0;
    double maximum = 0.0;
};

DifferenceSummary summarize_difference(const std::vector<float>& lhs,
                                       const std::vector<float>& rhs) {
    DifferenceSummary summary{lhs.size(), 0.0};
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!std::isfinite(lhs[i]) || !std::isfinite(rhs[i]))
            return {i, std::numeric_limits<double>::infinity()};
        const double difference =
            std::abs(static_cast<double>(lhs[i]) - rhs[i]);
        if (difference != 0.0 && summary.first == lhs.size())
            summary.first = i;
        summary.maximum = std::max(summary.maximum, difference);
    }
    return summary;
}

ModalDecaySummary measure_modal_decay(std::span<const float> samples,
                                      double sample_rate) {
    constexpr int fft_size = 8192;
    constexpr int hop = fft_size / 2;
    const int first = static_cast<int>(sample_rate);
    const int frames =
        (static_cast<int>(samples.size()) - first - fft_size) / hop + 1;
    const int first_bin =
        static_cast<int>(std::ceil(300.0 * fft_size / sample_rate));
    const int last_bin =
        static_cast<int>(std::floor(8000.0 * fft_size / sample_rate));
    const int bins = last_bin - first_bin + 1;

    pulp::signal::Fft fft(fft_size);
    std::vector<float> window(static_cast<std::size_t>(fft_size));
    std::vector<std::complex<float>> spectrum(
        static_cast<std::size_t>(fft_size));
    std::vector<std::vector<double>> energy(
        static_cast<std::size_t>(bins),
        std::vector<double>(static_cast<std::size_t>(frames)));

    for (int frame = 0; frame < frames; ++frame) {
        const int offset = first + frame * hop;
        for (int i = 0; i < fft_size; ++i) {
            const double phase =
                6.283185307179586 * static_cast<double>(i) /
                static_cast<double>(fft_size - 1);
            const double hann = 0.5 - 0.5 * std::cos(phase);
            window[static_cast<std::size_t>(i)] =
                samples[static_cast<std::size_t>(offset + i)] *
                static_cast<float>(hann);
        }
        fft.forward_real(window.data(), spectrum.data());
        for (int bin = first_bin; bin <= last_bin; ++bin) {
            energy[static_cast<std::size_t>(bin - first_bin)]
                  [static_cast<std::size_t>(frame)] =
                std::norm(spectrum[static_cast<std::size_t>(bin)]);
        }
    }

    // A reverse integration gives each literal FFT bin a monotonic energy
    // decay curve, avoiding modal beating changing the fitted sign.
    std::vector<double> t60(static_cast<std::size_t>(bins), 0.0);
    for (int bin = 0; bin < bins; ++bin) {
        auto& curve = energy[static_cast<std::size_t>(bin)];
        for (int frame = frames - 2; frame >= 0; --frame)
            curve[static_cast<std::size_t>(frame)] +=
                curve[static_cast<std::size_t>(frame + 1)];
        if (!(curve.front() > 0.0)) continue;

        double sum_t = 0.0;
        double sum_db = 0.0;
        double sum_tt = 0.0;
        double sum_tdb = 0.0;
        int count = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const double db =
                10.0 * std::log10(std::max(curve[static_cast<std::size_t>(frame)] /
                                               curve.front(),
                                           1e-30));
            if (db > -5.0 || db < -35.0) continue;
            const double time = static_cast<double>(frame * hop) / sample_rate;
            sum_t += time;
            sum_db += db;
            sum_tt += time * time;
            sum_tdb += time * db;
            ++count;
        }
        const double denominator = count * sum_tt - sum_t * sum_t;
        if (count >= 6 && denominator > 0.0) {
            const double slope =
                (count * sum_tdb - sum_t * sum_db) / denominator;
            if (slope < 0.0)
                t60[static_cast<std::size_t>(bin)] = -60.0 / slope;
        }
    }

    ModalDecaySummary result;
    // Compare every resolved bin with the median of its third-octave band.
    for (double centre = 400.0; centre <= 6400.0;
         centre *= 1.2599210498948732) {
        const double low = centre / std::pow(2.0, 1.0 / 6.0);
        const double high = centre * std::pow(2.0, 1.0 / 6.0);
        std::vector<double> band;
        for (int bin = first_bin; bin <= last_bin; ++bin) {
            const double hz =
                static_cast<double>(bin) * sample_rate / fft_size;
            const double decay = t60[static_cast<std::size_t>(bin - first_bin)];
            if (hz >= low && hz < high && decay > 0.0)
                band.push_back(decay);
        }
        if (band.size() < 5) continue;
        std::sort(band.begin(), band.end());
        const double median = band[band.size() / 2];
        for (double decay : band) {
            result.worst_ratio = std::max(result.worst_ratio, decay / median);
            ++result.compared_bins;
        }
    }
    return result;
}

} // namespace

TEST_CASE("fdn reverb delay lengths use the specified nearest-sample rounding",
          "[fdn][reverb][structure]") {
    FdnReverb reverb;
    reverb.prepare(kHostRate, kBlock);
    configure_neutral(reverb, 2.5, 1);

    constexpr double size = 0.5;
    const double scale =
        fdn::kSizeScaleMin + size * (fdn::kSizeScaleMax - fdn::kSizeScaleMin);
    const double dmin = fdn::kDelayMinSeconds * scale;
    const double dmax = fdn::kDelayMaxSeconds * scale;
    for (int i = 0; i < fdn::kNumChannels; ++i) {
        const double frac =
            static_cast<double>(i) / static_cast<double>(fdn::kNumChannels - 1);
        const double base =
            dmin + (dmax - dmin) * std::pow(frac, fdn::kDelayExponent);
        const double expected =
            std::round(base * reverb.tank_rate()) +
            static_cast<double>(fdn::kChannelPrimes[static_cast<std::size_t>(i)]);
        INFO("channel " << i);
        REQUIRE(reverb.tank().target_delay_samples(i) == expected);
    }
}

TEST_CASE("fdn reverb post-Hadamard guard uses the specified finite clamp",
          "[fdn][reverb][structure][stability]") {
    REQUIRE(fdn::post_hadamard_guard(5.0f) == 4.0f);
    REQUIRE(fdn::post_hadamard_guard(-5.0f) == -4.0f);
    REQUIRE(fdn::post_hadamard_guard(3.0f) == 3.0f);
    REQUIRE(fdn::post_hadamard_guard(
                std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    REQUIRE(fdn::post_hadamard_guard(
                std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("fdn reverb has no narrow mode that outlives its band",
          "[fdn][reverb][colour]") {
    FdnReverb reverb;
    reverb.prepare(kHostRate, kBlock);
    configure_neutral(reverb, 3.0, 5);
    const auto out =
        render(reverb, impulse(static_cast<std::size_t>(kHostRate * 10.0)));

    const auto decay = measure_modal_decay(out.left, kHostRate);
    INFO("compared " << decay.compared_bins
                     << " literal FFT bins; worst/third-octave-median T60 "
                     << decay.worst_ratio);
    REQUIRE(decay.compared_bins >= 1000);
    REQUIRE(decay.worst_ratio <= 2.0);
}

TEST_CASE("fdn reverb loop saturator THD rises monotonically with drive",
          "[fdn][reverb][saturation]") {
    FdnReverb public_path;
    public_path.prepare(kHostRate, kBlock);
    configure_neutral(public_path, 3.0, 5);
    REQUIRE(public_path.tank().loop_drive() == 0.0);
    public_path.set_parameter(Param::drive, 1.0);
    public_path.snap_parameters();
    REQUIRE(public_path.tank().loop_drive() == 1.0);

    constexpr int fft_length = 16384;
    constexpr int cycles = 341;
    const double tone_hz =
        static_cast<double>(cycles) * kHostRate / static_cast<double>(fft_length);
    const auto n = static_cast<std::size_t>(kHostRate * 8.0);
    std::vector<float> input(n);
    for (std::size_t i = 0; i < n; ++i)
        input[i] = 0.1f * static_cast<float>(
                               std::sin(6.283185307179586 * tone_hz *
                                        static_cast<double>(i) / kHostRate));

    std::array<double, 3> thd{};
    for (std::size_t d = 0; d < thd.size(); ++d) {
        fdn::FdnTank<float> tank;
        tank.prepare(fdn::kMaxTankRate);
        tank.configure(kHostRate);
        tank.set_flux_depth_db(0.0);
        fdn::TankControls controls;
        controls.decay = 3.0;
        controls.damp_hi = 0.0;
        controls.damp_lo = 0.0;
        controls.diffusion = 0.0;
        controls.mod = 0.0;
        controls.drive = static_cast<double>(d) * 0.5;
        tank.update_controls(controls);
        tank.reset();

        std::vector<float> output(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            float right = 0.0f;
            tank.process(input[i], input[i], output[i], right);
        }

        pulp::audio::Buffer<float> captured(1, output.size());
        std::copy(output.begin(), output.end(), captured.channel(0).begin());
        pulp::test::audio::ThdOptions options;
        options.fft_length = fft_length;
        options.analysis_offset =
            static_cast<int>(output.size()) - fft_length;
        options.num_harmonics = 8;
        const auto result = pulp::test::audio::measure_thd(
            std::as_const(captured).view(), tone_hz, kHostRate, options);
        REQUIRE(result.coherent);
        thd[d] = result.thd;
    }

    INFO("THD drive 0/0.5/1 = " << thd[0] << " / " << thd[1] << " / " << thd[2]);
    REQUIRE(thd[1] > thd[0]);
    REQUIRE(thd[2] > thd[1]);
}

TEST_CASE("fdn reverb kills non-finite state locally and recovers",
          "[fdn][reverb][stability]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();

    SECTION("allpass") {
        fdn::AllpassStage<float> stage;
        stage.prepare(64);
        stage.set_delay(8);
        REQUIRE(std::isfinite(stage.process(nan, 0.6f)));
        bool recovered = true;
        for (int i = 0; i < 32; ++i)
            recovered &= std::isfinite(stage.process(0.25f, 0.6f));
        REQUIRE(recovered);
    }

    SECTION("loop EQ and flux biquads") {
        fdn::LoopEq<double> eq;
        auto band = eq.band(4);
        band.gain_db = 12.0;
        eq.set_band(4, band);
        eq.configure(kHostRate);
        REQUIRE(std::isfinite(
            eq.process(0, std::numeric_limits<double>::quiet_NaN())));
        REQUIRE(std::isfinite(eq.process(
            0, std::numeric_limits<double>::max())));
        REQUIRE(std::isfinite(eq.process(0, 0.25)));

        fdn::FluxBank<double> flux;
        flux.configure(kHostRate, fdn::kFluxMaxDb);
        REQUIRE(std::isfinite(
            flux.process(0, std::numeric_limits<double>::quiet_NaN())));
        REQUIRE(std::isfinite(flux.process(
            0, std::numeric_limits<double>::max())));
        REQUIRE(std::isfinite(flux.process(0, 0.25)));
    }

    SECTION("damping state") {
        double state = std::numeric_limits<double>::quiet_NaN();
        REQUIRE(std::isfinite(fdn::damping_state_step(0.25, 0.9, state)));
        REQUIRE(std::isfinite(state));
        REQUIRE(std::isfinite(fdn::damping_state_step(
            std::numeric_limits<double>::infinity(), 0.9, state)));
        REQUIRE(std::isfinite(
            fdn::damping_state_step(0.25, 0.9, state)));
    }

    SECTION("shimmer tone filter") {
        fdn::ShimmerBank<float> shimmer;
        shimmer.prepare(fdn::kMaxTankRate);
        shimmer.configure(kHostRate);
        REQUIRE(std::isfinite(shimmer.process(0, nan)));
        bool recovered = true;
        for (int i = 0; i < 12000; ++i)
            recovered &= std::isfinite(shimmer.process(0, 0.1f));
        REQUIRE(recovered);
    }

    SECTION("ducker envelopes") {
        fdn::TransientDucker ducker;
        ducker.configure(kHostRate);
        REQUIRE(std::isfinite(
            ducker.process(std::numeric_limits<double>::quiet_NaN())));
        REQUIRE(std::isfinite(ducker.process(0.25)));
        REQUIRE(std::isfinite(ducker.fast_envelope()));
        REQUIRE(std::isfinite(ducker.slow_envelope()));
    }

    SECTION("Butterworth state") {
        fdn::ButterworthPair coefficients;
        coefficients.design(7000.0, kHostRate);
        fdn::ButterworthState state;
        REQUIRE(std::isfinite(state.process(
            coefficients.sections(),
            std::numeric_limits<double>::quiet_NaN())));
        REQUIRE(std::isfinite(state.process(
            coefficients.sections(), std::numeric_limits<double>::max())));
        REQUIRE(std::isfinite(state.process(coefficients.sections(), 0.25)));
    }

    SECTION("shared sanitized biquad state") {
        fdn::SanitizedBiquadState state;
        pulp::signal::BiquadCoefficientsT<double> coefficients;
        coefficients.b0 = 0.0;
        coefficients.b1 = 1e-20;
        REQUIRE(state.process(coefficients, 1.0) == 0.0);
        REQUIRE(state.process(coefficients, 0.0) == 0.0);
    }

    SECTION("complete bridge on equal, upsampled, and downsampled paths") {
        for (double rate : {16000.0, kHostRate, 96000.0}) {
            fdn::MultirateBridge<float> bridge;
            bridge.prepare(kHostRate, 64);
            bridge.configure(rate);
            std::array<float, 64> input{};
            input[0] = nan;
            const int tank_n =
                bridge.host_to_tank(input.data(), input.data(), input.size());
            REQUIRE(tank_n > 0);
            for (int rail = 0; rail < 2; ++rail) {
                REQUIRE(std::all_of(
                    bridge.tank_input(rail),
                    bridge.tank_input(rail) + tank_n,
                    [](float sample) { return std::isfinite(sample); }));
                std::fill_n(bridge.tank_output(rail), tank_n, 0.1f);
                bridge.tank_output(rail)[0] = nan;
            }
            std::array<float, 64> left{};
            std::array<float, 64> right{};
            bridge.tank_to_host(tank_n, left.data(), right.data(),
                                left.size());
            REQUIRE(std::all_of(left.begin(), left.end(), [](float sample) {
                return std::isfinite(sample);
            }));
            REQUIRE(std::all_of(right.begin(), right.end(), [](float sample) {
                return std::isfinite(sample);
            }));
        }
    }

    SECTION("complete engine") {
        for (int rate : {0, 7}) {
            FdnReverb reverb;
            reverb.prepare(kHostRate, kBlock);
            configure_neutral(reverb, 2.0, rate);
            std::vector<float> input(
                static_cast<std::size_t>(kHostRate), 0.0f);
            input[0] = nan;
            REQUIRE(all_finite(render(reverb, input)));
            const auto recovered = render(
                reverb, impulse(static_cast<std::size_t>(kHostRate * 2.0)));
            REQUIRE(all_finite(recovered));
            REQUIRE(peak(recovered) > 0.0);
        }
    }
}

TEST_CASE("fdn reverb rate switch resets every rate-domain state",
          "[fdn][reverb][tank-rate]") {
    for (const auto [from_rate, to_rate] :
         {std::pair{0, 7}, std::pair{7, 0}}) {
        FdnReverb switched;
        switched.prepare(kHostRate, kBlock);
        switched.set_mode(FdnReverb::Mode::hall);
        configure_neutral(switched, 2.0, from_rate);
        const auto dirty =
            render(switched, noise(static_cast<std::size_t>(kHostRate), 0.5f, 88u));
        REQUIRE(peak(dirty) > 0.0);
        switched.set_parameter(Param::tank_rate, static_cast<double>(to_rate));
        switched.snap_parameters();

        FdnReverb cold;
        cold.prepare(kHostRate, kBlock);
        cold.set_mode(FdnReverb::Mode::hall);
        configure_neutral(cold, 2.0, to_rate);

        for (int channel = 0; channel < fdn::kNumChannels; ++channel) {
            REQUIRE(switched.tank().target_delay_samples(channel) ==
                    cold.tank().target_delay_samples(channel));
            REQUIRE(switched.tank().applied_gain(channel) ==
                    cold.tank().applied_gain(channel));
        }
        REQUIRE(switched.tank().worst_case_boost() ==
                cold.tank().worst_case_boost());

        const auto probe = impulse(static_cast<std::size_t>(kHostRate * 6.0));
        const auto after = render(switched, probe);
        const auto reference = render(cold, probe);
        REQUIRE(all_finite(after));
        REQUIRE(all_finite(reference));
        const auto left_difference =
            summarize_difference(after.left, reference.left);
        const auto right_difference =
            summarize_difference(after.right, reference.right);
        INFO("rate " << from_rate << " -> " << to_rate);
        INFO("left first/max difference " << left_difference.first << " / "
                                          << left_difference.maximum);
        INFO("right first/max difference " << right_difference.first << " / "
                                           << right_difference.maximum);
        REQUIRE(left_difference.maximum == 0.0);
        REQUIRE(right_difference.maximum == 0.0);
        const double after_t60 = band_t60(after.left, kHostRate, kDecayProbeHz);
        const double cold_t60 = band_t60(reference.left, kHostRate, kDecayProbeHz);
        REQUIRE(after_t60 == cold_t60);
    }
}

TEST_CASE("fdn reverb reset applies pending controls before clearing state",
          "[fdn][reverb][reset]") {
    FdnReverb reset_after_activity;
    reset_after_activity.prepare(kHostRate, kBlock);
    configure_neutral(reset_after_activity, 2.0, 5);
    REQUIRE(peak(render(reset_after_activity,
                        noise(static_cast<std::size_t>(kHostRate), 0.5f, 99u))) >
            0.0);
    reset_after_activity.set_parameter(Param::size, 0.9);
    reset_after_activity.set_parameter(Param::mod, 0.7);
    reset_after_activity.reset();

    FdnReverb cold;
    cold.prepare(kHostRate, kBlock);
    configure_neutral(cold, 2.0, 5);
    cold.set_parameter(Param::size, 0.9);
    cold.set_parameter(Param::mod, 0.7);
    cold.snap_parameters();
    cold.reset();

    for (int channel = 0; channel < fdn::kNumChannels; ++channel) {
        REQUIRE(reset_after_activity.tank().target_delay_samples(channel) ==
                cold.tank().target_delay_samples(channel));
        REQUIRE(reset_after_activity.tank().applied_gain(channel) ==
                cold.tank().applied_gain(channel));
    }

    const auto probe = impulse(static_cast<std::size_t>(kHostRate * 2.0));
    const auto after = render(reset_after_activity, probe);
    const auto reference = render(cold, probe);
    REQUIRE(all_finite(after));
    REQUIRE(all_finite(reference));
    REQUIRE(summarize_difference(after.left, reference.left).maximum == 0.0);
    REQUIRE(summarize_difference(after.right, reference.right).maximum == 0.0);
}
