#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/signal/oversampling.hpp>

#include "test_oversampling_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::Oversampler;
using pulp::signal::Oversampler64;
using pulp::test::audio::tone_gain_db;
using pulp::test::audio::tone_residual_db;
using pulp::test::oversampling::injected_tone_rejection_db;
using pulp::test::oversampling::kPi;
using pulp::test::oversampling::settled_tail;

namespace {

struct Configuration {
    Oversampler::Factor factor;
    Oversampler::Quality quality;
    int expected_latency;
};

constexpr std::array<Configuration, 8> kConfigurations = {{
    {Oversampler::Factor::x2, Oversampler::Quality::standard, 64},
    {Oversampler::Factor::x4, Oversampler::Quality::standard, 76},
    {Oversampler::Factor::x8, Oversampler::Quality::standard, 80},
    {Oversampler::Factor::x16, Oversampler::Quality::standard, 82},
    {Oversampler::Factor::x2, Oversampler::Quality::pristine, 192},
    {Oversampler::Factor::x4, Oversampler::Quality::pristine, 209},
    {Oversampler::Factor::x8, Oversampler::Quality::pristine, 216},
    {Oversampler::Factor::x16, Oversampler::Quality::pristine, 219},
}};

template <typename OversamplerType>
std::vector<
    typename std::conditional_t<std::is_same_v<OversamplerType, Oversampler>, float, double>>
render_impulse(OversamplerType& oversampler, std::size_t frames) {
    using Sample =
        typename std::conditional_t<std::is_same_v<OversamplerType, Oversampler>, float, double>;
    std::vector<Sample> output(frames, Sample{0});
    auto identity = [](Sample sample) { return sample; };
    for (std::size_t i = 0; i < frames; ++i) {
        output[i] = oversampler.process(i == 0 ? Sample{1} : Sample{0}, identity);
    }
    return output;
}

template <typename Sample> std::size_t peak_index(const std::vector<Sample>& samples) {
    return static_cast<std::size_t>(std::distance(
        samples.begin(), std::max_element(samples.begin(), samples.end(), [](Sample a, Sample b) {
            return std::abs(a) < std::abs(b);
        })));
}

} // namespace

TEST_CASE("Linear-phase oversampler reports and produces its pinned latency",
          "[signal][oversampling][latency]") {
    for (const auto& config : kConfigurations) {
        Oversampler oversampler;
        oversampler.set_kind(Oversampler::Kind::linear_phase_fir);
        oversampler.set_quality(config.quality);
        oversampler.set_factor(config.factor);

        const auto latency = oversampler.latency();
        INFO("factor=" << oversampler.factor_value() << " expected=" << config.expected_latency);
        REQUIRE(latency.constant);
        REQUIRE(latency.input_samples == config.expected_latency);
        REQUIRE_THAT(latency.exact_input_samples,
                     WithinAbs(static_cast<double>(config.expected_latency), 1e-12));
        REQUIRE(oversampler.latency_samples() == config.expected_latency);

        const auto output =
            render_impulse(oversampler, static_cast<std::size_t>(config.expected_latency + 512));
        REQUIRE(peak_index(output) == static_cast<std::size_t>(config.expected_latency));
    }
}

TEST_CASE("Minimum-phase oversampler does not claim constant latency",
          "[signal][oversampling][latency]") {
    Oversampler oversampler;
    for (const auto kind : {Oversampler::Kind::fir_biquad, Oversampler::Kind::polyphase_iir}) {
        oversampler.set_kind(kind);
        REQUIRE_FALSE(oversampler.latency().constant);
        REQUIRE(oversampler.latency_samples() == 0);
    }
}

TEST_CASE("Every linear-phase factor preserves its advertised base-rate passband",
          "[signal][oversampling][passband]") {
    constexpr double amplitude = 0.5;
    constexpr std::size_t frames = 4096;
    for (const auto& config : kConfigurations) {
        const double frequency = config.quality == Oversampler::Quality::pristine ? 0.44 : 0.39;
        Oversampler64 oversampler;
        oversampler.set_kind(Oversampler64::Kind::linear_phase_fir);
        oversampler.set_quality(static_cast<Oversampler64::Quality>(config.quality));
        oversampler.set_factor(static_cast<Oversampler64::Factor>(config.factor));

        std::vector<double> output(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const double input = amplitude * std::sin(2.0 * kPi * frequency * i);
            output[i] = oversampler.process(input, [](double sample) { return sample; });
        }
        const std::size_t settled =
            static_cast<std::size_t>(2 * oversampler.latency_samples() + 256);
        const double gain_db = tone_gain_db(settled_tail(output, settled), frequency, amplitude);
        INFO("factor=" << static_cast<int>(config.factor) << " quality="
                       << static_cast<int>(config.quality) << " passband gain dB=" << gain_db);
        REQUIRE(std::abs(gain_db) < 0.01);
    }
}

TEST_CASE("Every oversampling lane supports every factor", "[signal][oversampling][factor]") {
    for (const auto kind : {Oversampler::Kind::fir_biquad, Oversampler::Kind::polyphase_iir,
                            Oversampler::Kind::linear_phase_fir}) {
        for (const auto factor : {Oversampler::Factor::x2, Oversampler::Factor::x4,
                                  Oversampler::Factor::x8, Oversampler::Factor::x16}) {
            Oversampler oversampler;
            oversampler.set_kind(kind);
            oversampler.set_factor(factor);
            int callback_count = 0;
            const float output = oversampler.process(1.0f, [&](float sample) {
                ++callback_count;
                return sample;
            });
            INFO("kind=" << static_cast<int>(kind) << " factor=" << static_cast<int>(factor));
            REQUIRE(callback_count == static_cast<int>(factor));
            REQUIRE(std::isfinite(output));
        }
    }
}

TEST_CASE("Linear-phase oversampler float and double paths align",
          "[signal][oversampling][precision]") {
    Oversampler single;
    Oversampler64 dual;
    single.set_kind(Oversampler::Kind::linear_phase_fir);
    dual.set_kind(Oversampler64::Kind::linear_phase_fir);
    single.set_quality(Oversampler::Quality::pristine);
    dual.set_quality(Oversampler64::Quality::pristine);
    single.set_factor(Oversampler::Factor::x4);
    dual.set_factor(Oversampler64::Factor::x4);

    const auto single_output = render_impulse(single, 768);
    const auto double_output = render_impulse(dual, 768);
    REQUIRE(single.latency_samples() == dual.latency_samples());
    for (std::size_t i = 0; i < single_output.size(); ++i) {
        REQUIRE_THAT(static_cast<double>(single_output[i]), WithinAbs(double_output[i], 2e-6));
    }
}

TEST_CASE("Linear-phase oversampler is invariant to caller block partitioning",
          "[signal][oversampling][block]") {
    std::vector<float> input(1024);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = 0.4f * std::sin(static_cast<float>(2.0 * kPi * 0.071 * i));
    }
    auto identity = [](float sample) { return sample; };

    Oversampler reference;
    reference.set_kind(Oversampler::Kind::linear_phase_fir);
    reference.set_quality(Oversampler::Quality::pristine);
    reference.set_factor(Oversampler::Factor::x16);
    std::vector<float> expected(input.size());
    reference.process_block(input.data(), expected.data(), input.size(), identity);

    for (const std::size_t block_size : {1u, 7u, 64u, 255u}) {
        Oversampler chunked;
        chunked.set_kind(Oversampler::Kind::linear_phase_fir);
        chunked.set_quality(Oversampler::Quality::pristine);
        chunked.set_factor(Oversampler::Factor::x16);
        std::vector<float> actual(input.size());
        for (std::size_t offset = 0; offset < input.size(); offset += block_size) {
            const std::size_t count = std::min(block_size, input.size() - offset);
            chunked.process_block(input.data() + offset, actual.data() + offset, count, identity);
        }
        REQUIRE(actual == expected);
    }
}

TEST_CASE("Pristine oversampling rejects smooth-saturation fold products near Nyquist",
          "[signal][oversampling][aliasing]") {
    constexpr double frequency = 0.445;
    constexpr std::size_t frames = 16384;
    Oversampler64 oversampler;
    oversampler.set_kind(Oversampler64::Kind::linear_phase_fir);
    oversampler.set_quality(Oversampler64::Quality::pristine);
    oversampler.set_factor(Oversampler64::Factor::x16);

    std::vector<double> output(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double input = 0.9 * std::sin(2.0 * kPi * frequency * i);
        output[i] =
            oversampler.process(input, [](double sample) { return std::tanh(2.5 * sample); });
    }

    const std::size_t settled = static_cast<std::size_t>(2 * oversampler.latency_samples() + 512);
    const double residual_db = tone_residual_db(settled_tail(output, settled), frequency);
    INFO("near-Nyquist saturation residual dB=" << residual_db);
    REQUIRE(residual_db < -100.0);
}

TEST_CASE("Linear-phase decimator meets its stopband rejection targets",
          "[signal][oversampling][stopband]") {
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        Oversampler64 oversampler;
        oversampler.set_kind(Oversampler64::Kind::linear_phase_fir);
        oversampler.set_quality(quality);
        oversampler.set_factor(Oversampler64::Factor::x2);

        std::size_t callback_frame = 0;
        double energy = 0.0;
        constexpr std::size_t frames = 8192;
        for (std::size_t i = 0; i < frames; ++i) {
            const double output = oversampler.process(0.0, [&](double) {
                const double value = std::sin(2.0 * kPi * 0.275 * callback_frame);
                ++callback_frame;
                return value;
            });
            if (i > 1024)
                energy += output * output;
        }
        const double rms_db = 10.0 * std::log10(energy / (frames - 1025));
        INFO("quality=" << static_cast<int>(quality) << " stopband RMS dBFS=" << rms_db);
        REQUIRE(rms_db < (quality == Oversampler64::Quality::standard ? -90.0 : -130.0));
    }
}

TEST_CASE("Every oversampling kind passes DC at unity gain", "[signal][oversampling][passband]") {
    // A chain of an interpolator, an identity callback and a decimator is a
    // unity-gain system. It stops being one if the anti-alias filters leave enough
    // of the interpolator's images standing, because decimation folds the image at
    // every multiple of the base rate coherently back onto DC.
    for (const auto kind : {Oversampler64::Kind::fir_biquad, Oversampler64::Kind::polyphase_iir,
                            Oversampler64::Kind::linear_phase_fir}) {
        for (const auto factor : {Oversampler64::Factor::x2, Oversampler64::Factor::x4,
                                  Oversampler64::Factor::x8, Oversampler64::Factor::x16}) {
            Oversampler64 oversampler;
            oversampler.set_kind(kind);
            oversampler.set_factor(factor);

            constexpr std::size_t frames = 4096;
            const std::size_t settled =
                static_cast<std::size_t>(2 * oversampler.latency_samples() + 512);
            double sum = 0.0;
            for (std::size_t i = 0; i < frames; ++i) {
                const double output = oversampler.process(1.0, [](double sample) { return sample; });
                if (i >= settled)
                    sum += output;
            }
            const double gain = sum / static_cast<double>(frames - settled);
            INFO("kind=" << static_cast<int>(kind) << " factor=" << static_cast<int>(factor)
                         << " DC gain=" << gain);
            REQUIRE_THAT(gain, WithinAbs(1.0, 1e-3));
        }
    }
}

TEST_CASE("Biquad lane keeps one character at every factor",
          "[signal][oversampling][passband][stopband]") {
    // The Biquad tier is deliberately cheap, so its numbers are modest: roughly
    // -1.9 dB at 0.3 of the base rate and only ~9 dB of alias rejection just above
    // base Nyquist. What must NOT vary is *which* numbers you get — raising the
    // factor buys headroom for the callback's own harmonics, it does not re-tune
    // the filter. Pinning the passband and the rejection together is what keeps
    // that honest: rejection can always be inflated by dropping the stage cutoffs,
    // but only by lowpassing away the top of the band, which the passband half of
    // this test forbids.
    constexpr std::array<double, 5> kPassband = {0.0, 0.05, 0.1, 0.2, 0.3};
    constexpr std::array<double, 3> kStopband = {0.55, 0.6, 0.75};
    constexpr double amplitude = 0.5;
    constexpr std::size_t frames = 8192;

    auto passband_gain_db = [](Oversampler64::Factor factor, double frequency) {
        Oversampler64 oversampler;
        oversampler.set_kind(Oversampler64::Kind::fir_biquad);
        oversampler.set_factor(factor);
        std::vector<double> output(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const double input = amplitude * std::sin(2.0 * kPi * frequency * i);
            output[i] = oversampler.process(input, [](double sample) { return sample; });
        }
        return tone_gain_db(settled_tail(output, 2048), frequency, amplitude);
    };

    // Anchor x2 absolutely first. Every other factor is pinned *relative* to it, so
    // without this the whole lane could be muffled in lockstep — drop the shared
    // cutoff and each factor still tracks x2 while the guide's quoted numbers
    // quietly stop being true.
    REQUIRE_THAT(passband_gain_db(Oversampler64::Factor::x2, 0.1), WithinAbs(-0.018, 0.05));
    REQUIRE_THAT(passband_gain_db(Oversampler64::Factor::x2, 0.3), WithinAbs(-1.79, 0.30));
    REQUIRE_THAT(injected_tone_rejection_db(Oversampler64::Kind::fir_biquad,
                                            Oversampler64::Quality::standard,
                                            Oversampler64::Factor::x2, 0.55),
                 WithinAbs(8.89, 1.0));

    for (const auto factor : {Oversampler64::Factor::x4, Oversampler64::Factor::x8,
                              Oversampler64::Factor::x16}) {
        for (const double frequency : kPassband) {
            if (frequency == 0.0)
                continue; // DC is covered exactly by the unity-gain case above.
            const double reference = passband_gain_db(Oversampler64::Factor::x2, frequency);
            const double actual = passband_gain_db(factor, frequency);
            INFO("factor=" << static_cast<int>(factor) << " f=" << frequency << " x2=" << reference
                           << " dB, actual=" << actual << " dB");
            REQUIRE_THAT(actual, WithinAbs(reference, 0.25));
        }
        for (const double frequency : kStopband) {
            // Quality only selects between the linear-phase FIR designs; the Biquad
            // lane ignores it.
            const auto rejection_at = [frequency](Oversampler64::Factor which) {
                return injected_tone_rejection_db(Oversampler64::Kind::fir_biquad,
                                                  Oversampler64::Quality::standard, which,
                                                  frequency);
            };
            const double reference = rejection_at(Oversampler64::Factor::x2);
            const double rejection = rejection_at(factor);
            INFO("factor=" << static_cast<int>(factor) << " f=" << frequency
                           << " rejection=" << rejection << " dB, x2=" << reference << " dB");
            REQUIRE(rejection > 8.0);
            // A higher factor may reject *better* — its extra stages also attenuate
            // content far above the fold boundary — but it must never reject worse.
            REQUIRE(rejection > reference - 0.5);
        }
    }
}

TEST_CASE("Every decimation stage holds its design floor against base-band aliases",
          "[signal][oversampling][stopband]") {
    // Each 2x decimation stage `s` folds around 2^s * Fs, so callback content in
    // (2^s - 0.5, 2^s) * Fs aliases straight into the base passband and is
    // attenuated by that stage's stopband alone — the lower stages pass the alias
    // through untouched. Probing one window per stage therefore pins every stage's
    // stopband independently; probing only near base Nyquist (window s = 0) leaves
    // the upper stages of x4/x8/x16 completely unconstrained.
    //
    // Attenuation is weakest at the low end of each window (nearest the stage's
    // stopband edge), so the offsets crowd toward zero.
    constexpr std::array<double, 9> kOffsets = {0.001, 0.005, 0.02, 0.05, 0.1,
                                                0.15,  0.25,  0.35, 0.49};

    for (const auto& config : kConfigurations) {
        const bool pristine = config.quality == Oversampler::Quality::pristine;
        // The designs target 96 dB / 140 dB; hold them to that within a 2 dB margin.
        const double floor_db = pristine ? 138.0 : 94.0;
        const int factor = static_cast<int>(config.factor);

        double worst_db = 1e9;
        double worst_frequency = 0.0;
        for (int stage = 0; (1 << stage) < factor; ++stage) {
            const double fold = static_cast<double>(1 << stage);
            for (const double offset : kOffsets) {
                const double frequency = fold - 0.5 + offset;
                const double rejection = injected_tone_rejection_db(
                    Oversampler64::Kind::linear_phase_fir,
                    static_cast<Oversampler64::Quality>(config.quality),
                    static_cast<Oversampler64::Factor>(config.factor), frequency);
                if (rejection < worst_db) {
                    worst_db = rejection;
                    worst_frequency = frequency;
                }
            }
        }
        INFO("factor=" << factor << " pristine=" << pristine << " worst rejection dB=" << worst_db
                       << " at " << worst_frequency << "*Fs (floor " << floor_db << ")");
        REQUIRE(worst_db > floor_db);
    }
}

TEST_CASE("Linear-phase oversampler reset restores a fresh instance exactly",
          "[signal][oversampling][reset]") {
    auto configure = [](Oversampler& oversampler, Oversampler::Factor factor,
                        Oversampler::Quality quality) {
        oversampler.set_kind(Oversampler::Kind::linear_phase_fir);
        oversampler.set_quality(quality);
        oversampler.set_factor(factor);
    };
    auto saturate = [](float sample) { return std::tanh(2.0f * sample); };

    for (const auto& config : kConfigurations) {
        Oversampler warmed;
        Oversampler fresh;
        configure(warmed, config.factor, config.quality);
        configure(fresh, config.factor, config.quality);

        // Push enough signal to fill every stage's delay line and leave the
        // decimators' odd-phase holdover non-zero, then reset.
        for (std::size_t i = 0; i < 777; ++i) {
            const float input = 0.7f * std::sin(static_cast<float>(2.0 * kPi * 0.13 * i));
            static_cast<void>(warmed.process(input, saturate));
        }
        warmed.reset();

        for (std::size_t i = 0; i < 512; ++i) {
            const float input = 0.4f * std::sin(static_cast<float>(2.0 * kPi * 0.071 * i));
            INFO("factor=" << static_cast<int>(config.factor)
                           << " quality=" << static_cast<int>(config.quality) << " frame=" << i);
            REQUIRE(warmed.process(input, saturate) == fresh.process(input, saturate));
        }
    }
}

TEST_CASE("Linear-phase latency is an exact sample count, not a truncation",
          "[signal][oversampling][latency]") {
    // latency() sums (taps - 1) / 2^(stage + 1) and returns the integer cast of
    // that sum while reporting `constant = true`. The tap counts are chosen so the
    // sum is integral; a tap-table edit that broke that would silently under-report
    // a fractional delay to the host. Pin the invariant itself, independent of the
    // expected-sample pins.
    for (const auto& config : kConfigurations) {
        Oversampler oversampler;
        oversampler.set_kind(Oversampler::Kind::linear_phase_fir);
        oversampler.set_quality(config.quality);
        oversampler.set_factor(config.factor);

        const auto latency = oversampler.latency();
        INFO("factor=" << oversampler.factor_value()
                       << " exact=" << latency.exact_input_samples);
        REQUIRE(latency.constant);
        REQUIRE_THAT(latency.exact_input_samples,
                     WithinAbs(static_cast<double>(latency.input_samples), 1e-9));
    }
}

TEST_CASE("Pristine float decimator keeps stopband energy below its numeric floor",
          "[signal][oversampling][stopband][precision]") {
    Oversampler oversampler;
    oversampler.set_kind(Oversampler::Kind::linear_phase_fir);
    oversampler.set_quality(Oversampler::Quality::pristine);
    oversampler.set_factor(Oversampler::Factor::x2);

    std::size_t callback_frame = 0;
    double energy = 0.0;
    constexpr std::size_t frames = 8192;
    for (std::size_t i = 0; i < frames; ++i) {
        const float output = oversampler.process(0.0f, [&](float) {
            const float value = static_cast<float>(std::sin(2.0 * kPi * 0.275 * callback_frame));
            ++callback_frame;
            return value;
        });
        if (i > 1024)
            energy += static_cast<double>(output) * output;
    }
    const double rms_db = 10.0 * std::log10(energy / (frames - 1025));
    INFO("pristine float stopband RMS dBFS=" << rms_db);
    REQUIRE(rms_db < -120.0);
}

// ── behaviour shared by every kind ──────────────────────────────────────────

namespace {

using pulp::test::audio::fit_tone;

// Phase (radians) of a unit sine at `frequency` after the identity round trip.
double round_trip_phase(Oversampler64& oversampler, double frequency) {
    oversampler.reset();
    constexpr std::size_t frames = 8192;
    std::vector<double> output(frames);
    for (std::size_t i = 0; i < frames; ++i)
        output[i] = oversampler.process(std::sin(2.0 * kPi * frequency * static_cast<double>(i)),
                                        [](double sample) { return sample; });
    const auto fit = fit_tone(std::span<const double>(output).subspan(4096), frequency);
    // The fit runs on samples [4096, frames), so its n = 0 is absolute index 4096.
    return std::atan2(fit.cos_gain, fit.sin_gain) - 2.0 * kPi * frequency * 4096.0;
}

} // namespace

TEST_CASE("Oversampler group delay function matches the measured round trip",
          "[signal][oversampling][latency]") {
    // Group delay is the slope of the measured phase across a narrow frequency
    // pair. A pure function that disagreed with the audio would be a sealed
    // stage someone re-derives wrong; this pins them together for both
    // minimum-phase lanes and the linear-phase one.
    for (const auto kind : {Oversampler64::Kind::polyphase_iir, Oversampler64::Kind::fir_biquad,
                            Oversampler64::Kind::linear_phase_fir}) {
        for (const auto factor : {Oversampler64::Factor::x2, Oversampler64::Factor::x8}) {
            Oversampler64 oversampler;
            oversampler.set_kind(kind);
            oversampler.set_factor(factor);
            for (const double frequency : {0.02, 0.15, 0.35}) {
                // The biquad lane rejects images by only ~7-20 dB, so near Nyquist
                // the decimator folds the tone's own image back onto it and the
                // round trip stops being LTI; its group delay only describes the
                // band where images are negligible.
                if (kind == Oversampler64::Kind::fir_biquad && frequency > 0.2)
                    continue;
                constexpr double delta = 0.0005;
                double dphi = round_trip_phase(oversampler, frequency + delta) -
                              round_trip_phase(oversampler, frequency - delta);
                dphi = std::remainder(dphi, 2.0 * kPi);
                const double measured = -dphi / (2.0 * kPi * 2.0 * delta);
                const double predicted = oversampler.group_delay_samples(frequency);
                INFO("kind=" << static_cast<int>(kind) << " factor=" << static_cast<int>(factor)
                             << " f=" << frequency << " measured=" << measured
                             << " predicted=" << predicted);
                REQUIRE_THAT(measured, WithinAbs(predicted, 0.01));
            }
        }
    }
    // Pin the standard x2 DC delay so a design change that moves it is a
    // deliberate, reviewed edit: (2 * 3.887 - 1) / 2 base samples, from the
    // stage-0 design's 3.887-sample delay at the 2x rate.
    Oversampler64 x2;
    x2.set_kind(Oversampler64::Kind::polyphase_iir);
    REQUIRE_THAT(x2.group_delay_samples(1e-6), WithinAbs(3.39, 0.01));
}

TEST_CASE("Block processing is identical to per-sample processing for every kind",
          "[signal][oversampling][block]") {
    std::vector<float> input(3001);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.8f * std::sin(static_cast<float>(2.0 * kPi * 0.0371 * i)) +
                   0.3f * std::sin(static_cast<float>(2.0 * kPi * 0.411 * i));
    auto shaper = [](float sample) { return std::tanh(2.0f * sample); };
    for (const auto kind : {Oversampler::Kind::fir_biquad, Oversampler::Kind::polyphase_iir,
                            Oversampler::Kind::linear_phase_fir}) {
        for (const auto factor :
             {Oversampler::Factor::x2, Oversampler::Factor::x4, Oversampler::Factor::x16}) {
            Oversampler per_sample;
            per_sample.set_kind(kind);
            per_sample.set_factor(factor);
            std::vector<float> expected(input.size());
            for (std::size_t i = 0; i < input.size(); ++i)
                expected[i] = per_sample.process(input[i], shaper);
            for (const std::size_t block : {1u, 5u, 64u, 1000u, 3001u}) {
                Oversampler blocked;
                blocked.set_kind(kind);
                blocked.set_factor(factor);
                std::vector<float> actual = input; // in place
                for (std::size_t offset = 0; offset < actual.size(); offset += block) {
                    const std::size_t count = std::min(block, actual.size() - offset);
                    blocked.process_block(actual.data() + offset, actual.data() + offset, count,
                                          shaper);
                }
                INFO("kind=" << static_cast<int>(kind) << " factor=" << static_cast<int>(factor)
                             << " block=" << block);
                REQUIRE(actual == expected);
            }
        }
    }
}

TEST_CASE("Block linear-phase stage matches the per-sample stage to rounding",
          "[signal][oversampling][block]") {
    // Same prototype, same polyphase split; only the summation order differs
    // (fixed oldest-first fused multiply-adds against a SIMD dot), so the two
    // forms must agree to float rounding, never to a filter-design difference.
    // Every stage OversamplerT configures, standard and pristine, including
    // pristine's 385-tap x2 stage.
    struct Stage {
        double cutoff, attenuation_db;
        unsigned taps;
    };
    for (const Stage stage :
         {Stage{0.40, 96.0, 129u}, Stage{0.20, 96.0, 49u}, Stage{0.10, 96.0, 33u},
          Stage{0.05, 96.0, 33u}, Stage{0.45, 140.0, 385u}, Stage{0.225, 140.0, 69u},
          Stage{0.1125, 140.0, 57u}, Stage{0.05625, 140.0, 49u}}) {
        INFO("taps " << stage.taps << " cutoff " << stage.cutoff);
        pulp::signal::detail::LinearPhaseOversamplingStage2x<float> per_sample;
        pulp::signal::detail::LinearPhaseOversamplingBlockStage2x<float> block;
        per_sample.configure(stage.cutoff, stage.attenuation_db, stage.taps);
        block.configure(stage.cutoff, stage.attenuation_db, stage.taps);
        REQUIRE(block.taps() == per_sample.taps());

        constexpr std::size_t kFrames = 1700; // not a multiple of the 16-lane group
        std::vector<float> input(kFrames);
        for (std::size_t i = 0; i < kFrames; ++i)
            input[i] = 0.6f * std::sin(static_cast<float>(0.37 * static_cast<double>(i))) +
                       0.3f * std::sin(static_cast<float>(2.9 * static_cast<double>(i)));

        std::vector<float> up_block(2 * kFrames), down_block(kFrames);
        for (std::size_t offset = 0; offset < kFrames; offset += 100) {
            const std::size_t count = std::min<std::size_t>(100, kFrames - offset);
            block.upsample(input.data() + offset, up_block.data() + 2 * offset, count);
        }
        for (std::size_t offset = 0; offset < kFrames; offset += 37) {
            const std::size_t count = std::min<std::size_t>(37, kFrames - offset);
            block.downsample(up_block.data() + 2 * offset, down_block.data() + offset, count);
        }

        double worst = 0.0;
        double worst_up = 0.0;
        for (std::size_t i = 0; i < kFrames; ++i) {
            float even = 0.0f, odd = 0.0f;
            per_sample.upsample(input[i], even, odd);
            worst_up = std::max({worst_up, std::abs(double(even) - up_block[2 * i]),
                                 std::abs(double(odd) - up_block[2 * i + 1])});
            const float down = per_sample.downsample(even, odd);
            worst = std::max(worst, std::abs(double(down) - down_block[i]));
        }
        INFO("worst upsample " << worst_up << " downsample " << worst);
        CHECK(worst_up < 2e-6);
        CHECK(worst < 2e-6);
    }
}
