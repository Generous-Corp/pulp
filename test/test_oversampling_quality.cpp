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
        const double gain_db = tone_gain_db(output, settled, frequency, amplitude);
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

TEST_CASE("Staged Biquad lane rejects callback content above base Nyquist at x16",
          "[signal][oversampling][stopband]") {
    Oversampler64 oversampler;
    oversampler.set_kind(Oversampler64::Kind::fir_biquad);
    oversampler.set_factor(Oversampler64::Factor::x16);

    std::size_t callback_frame = 0;
    double energy = 0.0;
    constexpr std::size_t frames = 8192;
    for (std::size_t i = 0; i < frames; ++i) {
        const double output = oversampler.process(0.0, [&](double) {
            const double value = std::sin(2.0 * kPi * (0.55 / 16.0) * callback_frame);
            ++callback_frame;
            return value;
        });
        if (i > 2048)
            energy += output * output;
    }
    const double rms_db = 10.0 * std::log10(energy / (frames - 2049));
    INFO("x16 staged-Biquad stopband RMS dBFS=" << rms_db);
    REQUIRE(rms_db < -24.0);
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
