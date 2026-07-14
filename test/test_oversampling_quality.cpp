#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/oversampling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::Oversampler;
using pulp::signal::Oversampler64;

namespace {

constexpr double kPi = 3.14159265358979323846;

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

double tone_residual_db(const std::vector<double>& samples, std::size_t begin,
                        double cycles_per_sample) {
    double sin_projection = 0.0;
    double cos_projection = 0.0;
    double sin_energy = 0.0;
    double cos_energy = 0.0;
    double cross_energy = 0.0;
    for (std::size_t i = begin; i < samples.size(); ++i) {
        const double phase = 2.0 * kPi * cycles_per_sample * i;
        const double s = std::sin(phase);
        const double c = std::cos(phase);
        sin_projection += samples[i] * s;
        cos_projection += samples[i] * c;
        sin_energy += s * s;
        cos_energy += c * c;
        cross_energy += s * c;
    }
    const double determinant = sin_energy * cos_energy - cross_energy * cross_energy;
    const double sin_gain =
        (sin_projection * cos_energy - cos_projection * cross_energy) / determinant;
    const double cos_gain =
        (cos_projection * sin_energy - sin_projection * cross_energy) / determinant;
    double signal_energy = 0.0;
    double residual_energy = 0.0;
    for (std::size_t i = begin; i < samples.size(); ++i) {
        const double phase = 2.0 * kPi * cycles_per_sample * i;
        const double fitted = sin_gain * std::sin(phase) + cos_gain * std::cos(phase);
        signal_energy += fitted * fitted;
        const double residual = samples[i] - fitted;
        residual_energy += residual * residual;
    }
    return 10.0 * std::log10(residual_energy / signal_energy);
}

double tone_gain_db(const std::vector<double>& samples, std::size_t begin, double cycles_per_sample,
                    double input_amplitude) {
    double sin_projection = 0.0;
    double cos_projection = 0.0;
    double sin_energy = 0.0;
    double cos_energy = 0.0;
    double cross_energy = 0.0;
    for (std::size_t i = begin; i < samples.size(); ++i) {
        const double phase = 2.0 * kPi * cycles_per_sample * i;
        const double sine = std::sin(phase);
        const double cosine = std::cos(phase);
        sin_projection += samples[i] * sine;
        cos_projection += samples[i] * cosine;
        sin_energy += sine * sine;
        cos_energy += cosine * cosine;
        cross_energy += sine * cosine;
    }
    const double determinant = sin_energy * cos_energy - cross_energy * cross_energy;
    const double sin_gain =
        (sin_projection * cos_energy - cos_projection * cross_energy) / determinant;
    const double cos_gain =
        (cos_projection * sin_energy - sin_projection * cross_energy) / determinant;
    const double amplitude = std::hypot(sin_gain, cos_gain);
    return 20.0 * std::log10(amplitude / input_amplitude);
}

// Rejection of a pure tone injected *inside* the callback, i.e. content the
// oversampled DSP produced rather than content the caller supplied. `frequency`
// is in cycles per BASE sample, so 0.5 is base Nyquist and factor/2 is the
// oversampled Nyquist. Returns how far the surviving output RMS sits below the
// injected tone's RMS.
double injected_tone_rejection_db(Oversampler64::Kind kind, Oversampler64::Quality quality,
                                  Oversampler64::Factor factor, double frequency) {
    Oversampler64 oversampler;
    oversampler.set_kind(kind);
    oversampler.set_quality(quality);
    oversampler.set_factor(factor);

    const double oversampled_frequency = frequency / static_cast<double>(factor);
    constexpr std::size_t frames = 8192;
    const std::size_t settled = static_cast<std::size_t>(2 * oversampler.latency_samples() + 512);

    std::size_t callback_frame = 0;
    double energy = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double output = oversampler.process(0.0, [&](double) {
            const double value = std::sin(2.0 * kPi * oversampled_frequency *
                                          static_cast<double>(callback_frame));
            ++callback_frame;
            return value;
        });
        if (i >= settled)
            energy += output * output;
    }
    const double mean_square = energy / static_cast<double>(frames - settled);
    // A unit-amplitude sine has mean square 0.5.
    return 10.0 * std::log10(0.5 / (mean_square + 1e-300));
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
    for (const auto kind : {Oversampler::Kind::fir_biquad, Oversampler::Kind::polyphase_iir,
                            Oversampler::Kind::elliptic_polyphase_iir}) {
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
        const double gain_db = tone_gain_db(output, settled, frequency, amplitude);
        INFO("factor=" << static_cast<int>(config.factor) << " quality="
                       << static_cast<int>(config.quality) << " passband gain dB=" << gain_db);
        REQUIRE(std::abs(gain_db) < 0.01);
    }
}

TEST_CASE("Every oversampling lane supports every factor", "[signal][oversampling][factor]") {
    for (const auto kind : {Oversampler::Kind::fir_biquad, Oversampler::Kind::polyphase_iir,
                            Oversampler::Kind::elliptic_polyphase_iir,
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
    const double residual_db = tone_residual_db(output, settled, frequency);
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
                            Oversampler64::Kind::elliptic_polyphase_iir,
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
        return tone_gain_db(output, 2048, frequency, amplitude);
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

TEST_CASE("Elliptic polyphase IIR oversampler preserves passband gain at every factor",
          "[signal][oversampling][elliptic][passband]") {
    constexpr double amplitude = 0.5;
    constexpr std::size_t frames = 8192;
    for (const auto factor : {Oversampler64::Factor::x2, Oversampler64::Factor::x4,
                              Oversampler64::Factor::x8, Oversampler64::Factor::x16}) {
        for (const auto quality :
             {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
            Oversampler64 oversampler;
            oversampler.set_kind(Oversampler64::Kind::elliptic_polyphase_iir);
            oversampler.set_quality(quality);
            oversampler.set_factor(factor);

            constexpr double frequency = 0.1;
            std::vector<double> output(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                const double input = amplitude * std::sin(2.0 * kPi * frequency * i);
                output[i] = oversampler.process(input, [](double sample) { return sample; });
            }
            const double gain_db = tone_gain_db(output, 2048, frequency, amplitude);
            INFO("factor=" << static_cast<int>(factor) << " quality=" << static_cast<int>(quality)
                           << " gain_db=" << gain_db);
            REQUIRE(std::abs(gain_db) < 0.25);
        }
    }
}

TEST_CASE("Elliptic polyphase IIR oversampler rejects deep-stopband energy",
          "[signal][oversampling][elliptic][stopband]") {
    // Same shape as the biquad/linear-phase stopband checks above: drive a
    // tone the callback itself introduces near the fold boundary and
    // confirm the decimator holds it down. Pristine's tighter per-stage
    // schedule (configure_elliptic_filters()) must reject at least as well
    // as standard.
    const double standard_rejection = injected_tone_rejection_db(
        Oversampler64::Kind::elliptic_polyphase_iir, Oversampler64::Quality::standard,
        Oversampler64::Factor::x2, 0.55);
    const double pristine_rejection = injected_tone_rejection_db(
        Oversampler64::Kind::elliptic_polyphase_iir, Oversampler64::Quality::pristine,
        Oversampler64::Factor::x2, 0.55);
    INFO("standard=" << standard_rejection << " dB pristine=" << pristine_rejection << " dB");
    REQUIRE(standard_rejection > 20.0);
    REQUIRE(pristine_rejection > standard_rejection - 1.0);
}

TEST_CASE("Elliptic polyphase IIR differs in group delay from the default polyphase IIR design",
          "[signal][oversampling][elliptic][latency]") {
    // This is the whole reason the Kind exists: the two minimum-phase
    // polyphase IIR designs are NOT interchangeable latency-wise. Neither
    // reports a latency() (both are correctly non-constant / frequency-
    // dependent group delay), so the only way to observe the difference is
    // to compare their impulse responses directly. A relabelled copy of
    // `polyphase_iir` would produce a bit-identical response; a genuinely
    // different design (different section count, different coefficients)
    // does not.
    for (const auto factor : {Oversampler::Factor::x2, Oversampler::Factor::x4,
                              Oversampler::Factor::x8}) {
        Oversampler default_design;
        default_design.set_kind(Oversampler::Kind::polyphase_iir);
        default_design.set_factor(factor);
        const auto default_impulse = render_impulse(default_design, 256);

        Oversampler elliptic_design;
        elliptic_design.set_kind(Oversampler::Kind::elliptic_polyphase_iir);
        elliptic_design.set_quality(Oversampler::Quality::pristine);
        elliptic_design.set_factor(factor);
        const auto elliptic_impulse = render_impulse(elliptic_design, 256);

        double max_abs_diff = 0.0;
        for (std::size_t i = 0; i < default_impulse.size(); ++i) {
            max_abs_diff = std::max(
                max_abs_diff,
                static_cast<double>(std::abs(default_impulse[i] - elliptic_impulse[i])));
        }
        INFO("factor=" << static_cast<int>(factor) << " max_abs_diff=" << max_abs_diff);
        REQUIRE(std::isfinite(max_abs_diff));
        REQUIRE(max_abs_diff > 1e-3);
    }
}

TEST_CASE("Elliptic polyphase IIR oversampler reset restores a fresh instance exactly",
          "[signal][oversampling][elliptic][reset]") {
    Oversampler warmed, fresh;
    warmed.set_kind(Oversampler::Kind::elliptic_polyphase_iir);
    fresh.set_kind(Oversampler::Kind::elliptic_polyphase_iir);
    warmed.set_quality(Oversampler::Quality::pristine);
    fresh.set_quality(Oversampler::Quality::pristine);
    warmed.set_factor(Oversampler::Factor::x4);
    fresh.set_factor(Oversampler::Factor::x4);

    auto saturate = [](float sample) { return std::tanh(2.0f * sample); };
    for (std::size_t i = 0; i < 777; ++i) {
        const float input = 0.7f * std::sin(static_cast<float>(2.0 * kPi * 0.13 * i));
        static_cast<void>(warmed.process(input, saturate));
    }
    warmed.reset();

    for (std::size_t i = 0; i < 512; ++i) {
        const float input = 0.4f * std::sin(static_cast<float>(2.0 * kPi * 0.071 * i));
        INFO("frame=" << i);
        REQUIRE(warmed.process(input, saturate) == fresh.process(input, saturate));
    }
}

TEST_CASE("OversamplerT::Kind ordinals preserve pre-elliptic ABI",
          "[signal][oversampling][kind][abi]") {
    // elliptic_polyphase_iir was added after linear_phase_fir already
    // shipped as ordinal 2. Inserting it before linear_phase_fir would bump
    // linear_phase_fir to ordinal 3, silently reinterpreting any persisted
    // integer Kind == 2 (e.g. a saved plugin state) as a different filter
    // after upgrading. New kinds must only ever append at the end.
    REQUIRE(static_cast<int>(Oversampler::Kind::fir_biquad) == 0);
    REQUIRE(static_cast<int>(Oversampler::Kind::polyphase_iir) == 1);
    REQUIRE(static_cast<int>(Oversampler::Kind::linear_phase_fir) == 2);
    REQUIRE(static_cast<int>(Oversampler::Kind::elliptic_polyphase_iir) == 3);
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
