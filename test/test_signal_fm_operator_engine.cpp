#include <pulp/signal/fm_operator_engine.hpp>

#include <pulp/signal/drum/fm.hpp>
#include <pulp/signal/drum/fm6.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

#if defined(_MSC_VER)
#define PULP_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define PULP_TEST_NOINLINE __attribute__((noinline))
#else
#define PULP_TEST_NOINLINE
#endif

using FloatFmEngine = pulp::signal::FmOperatorEngine;

PULP_TEST_NOINLINE bool set_float_trig_profile(FloatFmEngine& engine,
                                                FloatFmEngine::TrigProfile profile) {
    return engine.set_trig_profile(profile);
}

PULP_TEST_NOINLINE float process_float_sample(FloatFmEngine& engine) {
    return engine.process();
}

PULP_TEST_NOINLINE void process_float_block(FloatFmEngine& engine, float* output,
                                             std::size_t count) {
    engine.process(output, count);
}

#undef PULP_TEST_NOINLINE

template <typename Engine> void set_sustained_envelopes(Engine& engine, double release_ms = 0.0) {
    typename Engine::Envelope envelope;
    envelope.attack_ms = 0.0;
    envelope.decay_ms = 0.0;
    envelope.sustain = 1.0;
    envelope.release_ms = release_ms;
    for (std::size_t op = 0; op < engine.operator_count(); ++op)
        engine.set_operator_envelope(op, envelope);
}

template <typename SampleType, std::size_t MaxOperators>
std::vector<SampleType> render(pulp::signal::FmOperatorEngineT<SampleType, MaxOperators>& engine,
                               std::size_t samples) {
    std::vector<SampleType> output(samples);
    engine.process(output.data(), output.size());
    return output;
}

template <typename SampleType>
double bin_magnitude(const std::vector<SampleType>& signal, double frequency_hz,
                     double sample_rate = kSampleRate) {
    std::complex<double> sum{};
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double phase = -kTwoPi * frequency_hz * static_cast<double>(i) / sample_rate;
        sum +=
            static_cast<double>(signal[i]) * std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return 2.0 * std::abs(sum) / static_cast<double>(signal.size());
}

template <typename SampleType> double rms(const std::vector<SampleType>& signal) {
    double sum = 0.0;
    for (const auto sample : signal) {
        const double x = static_cast<double>(sample);
        sum += x * x;
    }
    return std::sqrt(sum / static_cast<double>(signal.size()));
}

template <typename SampleType>
double estimate_frequency(const std::vector<SampleType>& signal, double sample_rate = kSampleRate) {
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < signal.size(); ++i) {
        if (signal[i - 1] <= SampleType{0} && signal[i] > SampleType{0})
            ++crossings;
    }
    return static_cast<double>(crossings) * sample_rate / static_cast<double>(signal.size() - 1);
}

template <typename SampleType>
double nonfundamental_energy(const std::vector<SampleType>& signal, double fundamental_hz) {
    double mean = 0.0;
    double mean_square = 0.0;
    for (const auto sample : signal) {
        const double x = static_cast<double>(sample);
        mean += x;
        mean_square += x * x;
    }
    mean /= static_cast<double>(signal.size());
    mean_square /= static_cast<double>(signal.size());
    const double fundamental = bin_magnitude(signal, fundamental_hz);
    return std::max(0.0, mean_square - mean * mean - 0.5 * fundamental * fundamental);
}

} // namespace

TEST_CASE("A single melodic operator follows the played frequency",
          "[signal][fm-operator][pitch]") {
    pulp::signal::FmOperatorEngine engine;
    engine.prepare(kSampleRate);
    engine.set_base_frequency_hz(375.0f);
    set_sustained_envelopes(engine);
    engine.note_on();

    const auto output = render(engine, 48000);
    REQUIRE(estimate_frequency(output) == Catch::Approx(375.0).margin(1.1));
    REQUIRE(bin_magnitude(output, 375.0) == Catch::Approx(1.0).margin(1e-4));
    REQUIRE(engine.latency_samples() == 0);
}

TEST_CASE("Ratio and fixed-Hz operators use distinct real-unit frequency modes",
          "[signal][fm-operator][pitch]") {
    using Engine = pulp::signal::FmOperatorEngine64;
    Engine ratio;
    ratio.prepare(kSampleRate);
    ratio.set_base_frequency_hz(220.0);
    ratio.set_operator_ratio(0, 2.0);
    set_sustained_envelopes(ratio);
    ratio.note_on();

    Engine fixed;
    fixed.prepare(kSampleRate);
    fixed.set_base_frequency_hz(110.0);
    fixed.set_operator_frequency_mode(0, Engine::FrequencyMode::fixed_hz);
    fixed.set_operator_fixed_frequency_hz(0, 660.0);
    set_sustained_envelopes(fixed);
    fixed.note_on();

    REQUIRE(estimate_frequency(render(ratio, 48000)) == Catch::Approx(440.0).margin(1.1));
    REQUIRE(estimate_frequency(render(fixed, 48000)) == Catch::Approx(660.0).margin(1.1));
}

TEST_CASE("Phase modulation follows the one-sample-delayed analytic recurrence",
          "[signal][fm-operator][pm][oracle]") {
    using Engine = pulp::signal::FmOperatorEngine64;
    Engine engine;
    engine.set_operator_count(2);
    engine.prepare(kSampleRate);
    engine.set_base_frequency_hz(375.0);
    engine.set_operator_ratio(1, 2.0);
    engine.routing().clear();
    engine.routing().set_carrier_gain(0, 1.0);
    constexpr double depth_radians = 1.75;
    engine.routing().set_phase_modulation_radians(0, 1, depth_radians);
    set_sustained_envelopes(engine);
    engine.note_on();

    double previous_modulator = 0.0;
    double carrier_phase = 0.0;
    double modulator_phase = 0.0;
    for (int sample = 0; sample < 256; ++sample) {
        carrier_phase += 375.0 / kSampleRate;
        modulator_phase += 750.0 / kSampleRate;
        const double expected =
            std::sin(kTwoPi * carrier_phase + depth_radians * previous_modulator);
        previous_modulator = std::sin(kTwoPi * modulator_phase);
        REQUIRE(engine.process() == Catch::Approx(expected).margin(1e-12));
    }
}

TEST_CASE("Frequency modulation integrates routed Hz deviation",
          "[signal][fm-operator][fm][oracle]") {
    using Engine = pulp::signal::FmOperatorEngine64;
    Engine engine;
    engine.set_operator_count(2);
    engine.prepare(kSampleRate);
    engine.set_base_frequency_hz(500.0);
    engine.set_operator_fixed_frequency_hz(1, 125.0);
    engine.set_operator_frequency_mode(1, Engine::FrequencyMode::fixed_hz);
    engine.routing().clear();
    engine.routing().set_carrier_gain(0, 1.0);
    constexpr double deviation_hz = 180.0;
    engine.routing().set_frequency_modulation_hz(0, 1, deviation_hz);
    set_sustained_envelopes(engine);
    engine.note_on();

    double previous_modulator = 0.0;
    double carrier_phase = 0.0;
    double modulator_phase = 0.0;
    for (int sample = 0; sample < 256; ++sample) {
        carrier_phase += (500.0 + deviation_hz * previous_modulator) / kSampleRate;
        modulator_phase += 125.0 / kSampleRate;
        const double expected = std::sin(kTwoPi * carrier_phase);
        previous_modulator = std::sin(kTwoPi * modulator_phase);
        REQUIRE(engine.process() == Catch::Approx(expected).margin(1e-12));
    }
}

TEST_CASE("Carrier normalization and key scaling have explicit gain laws",
          "[signal][fm-operator][gain]") {
    using Engine = pulp::signal::FmOperatorEngine64;
    Engine one;
    one.prepare(kSampleRate);
    one.set_base_frequency_hz(375.0);
    set_sustained_envelopes(one);
    one.note_on();

    Engine two;
    two.set_operator_count(2);
    two.prepare(kSampleRate);
    two.set_base_frequency_hz(375.0);
    two.routing().set_carrier_gain(1, 1.0);
    set_sustained_envelopes(two);
    two.note_on();

    const auto one_output = render(one, 4096);
    const auto two_output = render(two, 4096);
    REQUIRE(two_output == one_output);

    Engine attenuated;
    attenuated.prepare(kSampleRate);
    attenuated.set_base_frequency_hz(375.0);
    attenuated.routing().set_carrier_gain(0, 0.25);
    set_sustained_envelopes(attenuated);
    attenuated.note_on();
    const auto attenuated_output = render(attenuated, 4096);
    for (std::size_t sample = 0; sample < one_output.size(); ++sample) {
        REQUIRE(attenuated_output[sample] ==
                Catch::Approx(one_output[sample] * 0.25).margin(1e-12));
    }

    Engine low;
    low.prepare(kSampleRate);
    low.set_base_frequency_hz(220.0);
    low.set_operator_key_scaling(0, -6.0, 220.0);
    set_sustained_envelopes(low);
    low.note_on();

    Engine high;
    high.prepare(kSampleRate);
    high.set_base_frequency_hz(440.0);
    high.set_operator_key_scaling(0, -6.0, 220.0);
    set_sustained_envelopes(high);
    high.note_on();

    const double ratio = rms(render(high, 48000)) / rms(render(low, 48000));
    REQUIRE(ratio == Catch::Approx(std::pow(10.0, -6.0 / 20.0)).margin(1e-5));
}

TEST_CASE("Bounded feedback creates independently measurable sidebands",
          "[signal][fm-operator][feedback][spectrum]") {
    auto render_feedback = [](double feedback_radians) {
        pulp::signal::FmOperatorEngine64 engine;
        engine.prepare(kSampleRate);
        engine.set_base_frequency_hz(375.0);
        engine.set_operator_feedback_radians(0, feedback_radians);
        set_sustained_envelopes(engine);
        engine.note_on();
        return render(engine, 48000);
    };

    const auto clean = render_feedback(0.0);
    const auto driven = render_feedback(4.0);
    REQUIRE(nonfundamental_energy(clean, 375.0) < 1e-20);
    REQUIRE(nonfundamental_energy(driven, 375.0) > 1e-3);
}

TEST_CASE("The bright-band alias policy suppresses unsafe high-note modulation",
          "[signal][fm-operator][alias][spectrum]") {
    auto render_policy = [](pulp::signal::FmOperatorAliasPolicy policy) {
        pulp::signal::FmOperatorEngine64 engine;
        engine.set_operator_count(2);
        engine.prepare(kSampleRate);
        engine.set_base_frequency_hz(21000.0);
        engine.set_operator_fixed_frequency_hz(1, 7000.0);
        engine.set_operator_frequency_mode(1, pulp::signal::FmOperatorFrequencyMode::fixed_hz);
        engine.routing().clear();
        engine.routing().set_carrier_gain(0, 1.0);
        engine.routing().set_phase_modulation_radians(0, 1, 10.0);
        engine.routing().set_frequency_modulation_hz(0, 1, 12000.0);
        engine.set_alias_policy(policy);
        set_sustained_envelopes(engine);
        engine.note_on();
        return render(engine, 48000);
    };

    const auto bounded = render_policy(pulp::signal::FmOperatorAliasPolicy::bounded);
    const auto safe = render_policy(pulp::signal::FmOperatorAliasPolicy::bright_band_safe);
    REQUIRE(nonfundamental_energy(safe, 21000.0) < nonfundamental_energy(bounded, 21000.0) * 0.01);
    REQUIRE(bin_magnitude(safe, 21000.0) > 0.99);
}

TEST_CASE("Reset and block partitioning are bit deterministic for cyclic routing",
          "[signal][fm-operator][determinism]") {
    using Engine = pulp::signal::FmOperatorEngine64;
    auto configured = [] {
        Engine engine;
        engine.set_operator_count(3);
        engine.prepare(kSampleRate);
        engine.set_base_frequency_hz(275.0);
        engine.set_operator_ratio(1, 1.5);
        engine.set_operator_ratio(2, 2.25);
        engine.routing().clear();
        engine.routing().set_carrier_gain(0, 1.0);
        engine.routing().set_phase_modulation_radians(0, 1, 2.0);
        engine.routing().set_phase_modulation_radians(1, 2, 1.25);
        engine.routing().set_phase_modulation_radians(2, 0, 0.75);
        engine.set_operator_feedback_radians(2, 0.5);
        set_sustained_envelopes(engine, 40.0);
        return engine;
    };

    auto contiguous = configured();
    contiguous.note_on();
    const auto expected = render(contiguous, 4096);

    auto partitioned = configured();
    partitioned.note_on();
    std::vector<double> actual(4096);
    partitioned.process(actual.data(), 17);
    partitioned.process(actual.data() + 17, 1000);
    partitioned.process(actual.data() + 1017, actual.size() - 1017);
    REQUIRE(actual == expected);

    partitioned.reset();
    partitioned.note_on();
    REQUIRE(render(partitioned, 4096) == expected);
}

TEST_CASE("The shared evaluator preserves the six-operator drum routing law",
          "[signal][fm-operator][compatibility]") {
    constexpr std::size_t count =
        static_cast<std::size_t>(pulp::signal::drum::Fm6DrumVoice::operator_count);
    const auto& algorithm = pulp::signal::drum::Fm6DrumVoice::algorithms[7];
    constexpr std::array<double, count> ratios = {1.0, 1.5, 2.0, 2.5, 3.0, 4.0};
    constexpr std::array<double, count> amplitudes = {0.9, 0.8, 0.7, 0.6, 0.5, 0.4};
    constexpr double base_hz = 137.0;
    constexpr double depth = 2.75;
    constexpr double feedback_depth = 1.5;

    std::array<double, count> legacy_phases{};
    std::array<double, count> legacy_previous{};
    std::array<double, count> shared_phases{};
    std::array<double, count> shared_previous{};
    std::array<double, count> feedback{};
    feedback[algorithm.feedback_op] = feedback_depth;

    for (int sample = 0; sample < 2048; ++sample) {
        std::array<double, count> legacy_current{};
        for (std::size_t op = 0; op < count; ++op) {
            double modulation = 0.0;
            for (std::size_t source = 0; source < count; ++source) {
                if (algorithm.modulated_by[op] & (1u << source))
                    modulation += legacy_previous[source];
            }
            modulation *= depth;
            if (op == algorithm.feedback_op)
                modulation += feedback_depth * legacy_previous[op];
            const double hz = std::min(base_hz * ratios[op], 0.49 * kSampleRate);
            legacy_phases[op] += hz / kSampleRate;
            legacy_phases[op] -= std::floor(legacy_phases[op]);
            legacy_current[op] = std::sin(kTwoPi * legacy_phases[op] + modulation) * amplitudes[op];
        }
        double legacy_output = 0.0;
        int carriers = 0;
        for (std::size_t op = 0; op < count; ++op) {
            if (algorithm.carriers & (1u << op)) {
                legacy_output += legacy_current[op];
                ++carriers;
            }
        }
        if (carriers > 1)
            legacy_output /= static_cast<double>(carriers);
        legacy_previous = legacy_current;

        const auto frequency = [&](std::size_t op) { return base_hz * ratios[op]; };
        const auto phase_route = [&](std::size_t destination, std::size_t source) {
            return (algorithm.modulated_by[destination] & (1u << source)) ? depth : 0.0;
        };
        const auto no_frequency_route = [](std::size_t, std::size_t) { return 0.0; };
        const auto carrier = [&](std::size_t op) {
            return (algorithm.carriers & (1u << op)) ? 1.0 : 0.0;
        };
        const auto sine = [](std::size_t, double phase, double offset, double) {
            return std::sin(kTwoPi * phase + offset);
        };
        const double shared_output = pulp::signal::detail::render_fm_operator_sample(
            count, kSampleRate, pulp::signal::FmOperatorAliasPolicy::bounded, shared_phases,
            shared_previous, amplitudes, feedback, frequency, phase_route, no_frequency_route,
            carrier, sine);

        REQUIRE(shared_output == Catch::Approx(legacy_output).margin(2e-13));
    }
}

TEST_CASE("The shared evaluator preserves FM8 wave-table phase units",
          "[signal][fm-operator][compatibility]") {
    constexpr std::size_t count =
        static_cast<std::size_t>(pulp::signal::drum::Fm8DrumVoice::operator_count);
    const auto& algorithm = pulp::signal::drum::Fm8DrumVoice::algorithms[12];
    constexpr std::array<double, count> ratios = {1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0};
    constexpr std::array<double, count> amplitudes = {0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2};
    constexpr std::array<int, count> waves = {0, 3, 6, 9, 12, 15, 18, 21};
    constexpr double base_hz = 173.0;
    constexpr double depth = 3.25;

    std::array<double, count> legacy_phases{};
    std::array<double, count> legacy_previous{};
    std::array<double, count> shared_phases{};
    std::array<double, count> shared_previous{};
    std::array<double, count> feedback{};
    feedback[2] = 2.0;

    for (int sample = 0; sample < 2048; ++sample) {
        std::array<double, count> legacy_current{};
        for (std::size_t op = 0; op < count; ++op) {
            double modulation = 0.0;
            for (std::size_t source = 0; source < count; ++source) {
                if (algorithm.modulated_by[op] & (1u << source))
                    modulation += legacy_previous[source];
            }
            modulation *= depth;
            modulation += feedback[op] * legacy_previous[op];
            const double hz = std::min(base_hz * ratios[op], 0.49 * kSampleRate);
            legacy_phases[op] += hz / kSampleRate;
            legacy_phases[op] -= std::floor(legacy_phases[op]);
            legacy_current[op] =
                pulp::signal::drum::FmWaveTable::read(
                    waves[op], legacy_phases[op] + modulation / kTwoPi, hz / kSampleRate) *
                amplitudes[op];
        }
        double legacy_output = 0.0;
        int carriers = 0;
        for (std::size_t op = 0; op < count; ++op) {
            if (algorithm.carriers & (1u << op)) {
                legacy_output += legacy_current[op];
                ++carriers;
            }
        }
        if (carriers > 1)
            legacy_output /= static_cast<double>(carriers);
        legacy_previous = legacy_current;

        const auto frequency = [&](std::size_t op) { return base_hz * ratios[op]; };
        const auto phase_route = [&](std::size_t destination, std::size_t source) {
            return (algorithm.modulated_by[destination] & (1u << source)) ? depth : 0.0;
        };
        const auto no_frequency_route = [](std::size_t, std::size_t) { return 0.0; };
        const auto carrier = [&](std::size_t op) {
            return (algorithm.carriers & (1u << op)) ? 1.0 : 0.0;
        };
        const auto wave = [&](std::size_t op, double phase, double offset, double increment) {
            return pulp::signal::drum::FmWaveTable::read(waves[op], phase + offset / kTwoPi,
                                                         increment);
        };
        const double shared_output = pulp::signal::detail::render_fm_operator_sample(
            count, kSampleRate, pulp::signal::FmOperatorAliasPolicy::bounded, shared_phases,
            shared_previous, amplitudes, feedback, frequency, phase_route, no_frequency_route,
            carrier, wave);

        REQUIRE(shared_output == Catch::Approx(legacy_output).margin(2e-13));
    }
}

TEST_CASE("Envelope release, latency, and tail declarations agree exactly",
          "[signal][fm-operator][time]") {
    pulp::signal::FmOperatorEngine64 engine;
    engine.prepare(kSampleRate);
    set_sustained_envelopes(engine, 10.0);
    engine.note_on();
    REQUIRE(engine.process() != 0.0);
    engine.note_off();
    REQUIRE(engine.latency_samples() == 0);
    REQUIRE(engine.tail_samples() == 480);
    for (int sample = 0; sample < engine.tail_samples(); ++sample)
        (void)engine.process();
    REQUIRE_FALSE(engine.is_active());
    REQUIRE(engine.process() == 0.0);
}

TEST_CASE("Float FM operators offer explicit efficient and precise trig profiles",
          "[signal][fm-operator][trig-profile]") {
    using Engine = pulp::signal::FmOperatorEngine;
    const auto configure = [](Engine& engine) {
        engine.set_operator_count(4);
        engine.prepare(kSampleRate);
        engine.set_base_frequency_hz(187.5f);
        for (std::size_t op = 0; op < engine.operator_count(); ++op) {
            engine.set_operator_ratio(op, static_cast<float>(op + 1));
            engine.set_operator_level(op, 0.8f);
        }
        engine.routing().clear();
        engine.routing().set_carrier_gain(0, 1.0f);
        engine.routing().set_phase_modulation_radians(0, 1, 2.5f);
        engine.routing().set_phase_modulation_radians(1, 2, 1.75f);
        engine.routing().set_phase_modulation_radians(2, 3, 1.25f);
        engine.set_operator_feedback_radians(3, 0.8f);
        set_sustained_envelopes(engine);
        engine.note_on();
    };

    Engine reference;
    Engine efficient;
    Engine precise;
    configure(reference);
    configure(efficient);
    configure(precise);
    REQUIRE(reference.trig_profile() == Engine::TrigProfile::reference);
    REQUIRE(efficient.set_trig_profile(Engine::TrigProfile::realtime_efficient));
    REQUIRE(precise.set_trig_profile(Engine::TrigProfile::realtime_precise));

    const auto expected = render(reference, 8192);
    const auto efficient_output = render(efficient, expected.size());
    const auto precise_output = render(precise, expected.size());
    double efficient_squared_error = 0.0;
    double precise_squared_error = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const double efficient_error = efficient_output[i] - expected[i];
        const double precise_error = precise_output[i] - expected[i];
        efficient_squared_error += efficient_error * efficient_error;
        precise_squared_error += precise_error * precise_error;
    }
    const double efficient_rms =
        std::sqrt(efficient_squared_error / static_cast<double>(expected.size()));
    const double precise_rms =
        std::sqrt(precise_squared_error / static_cast<double>(expected.size()));
    REQUIRE(efficient_rms > 0.0);
    REQUIRE(efficient_rms < 1.0e-4);
    REQUIRE(precise_rms > 0.0);
    REQUIRE(precise_rms < 3.0e-7);
}

TEST_CASE("FM trig profile support is float-only and block dispatch is stable",
          "[signal][fm-operator][trig-profile]") {
    using FloatEngine = pulp::signal::FmOperatorEngine;
    using DoubleEngine = pulp::signal::FmOperatorEngine64;
    STATIC_REQUIRE(FloatEngine::supports_realtime_trig_profiles());
    STATIC_REQUIRE_FALSE(DoubleEngine::supports_realtime_trig_profiles());

    DoubleEngine double_engine;
    REQUIRE_FALSE(
        double_engine.set_trig_profile(DoubleEngine::TrigProfile::realtime_precise));
    REQUIRE(double_engine.trig_profile() == DoubleEngine::TrigProfile::reference);

    FloatEngine block_engine;
    FloatEngine sample_engine;
    for (auto* engine : {&block_engine, &sample_engine}) {
        engine->prepare(kSampleRate);
        engine->set_base_frequency_hz(375.0f);
        set_sustained_envelopes(*engine);
        REQUIRE(engine->set_trig_profile(FloatEngine::TrigProfile::realtime_precise));
        engine->note_on();
    }
    std::array<float, 257> block_output{};
    std::array<float, 257> sample_output{};
    block_engine.process(block_output.data(), block_output.size());
    for (auto& sample : sample_output)
        sample = sample_engine.process();
    REQUIRE(block_output == sample_output);
}

TEST_CASE("FM trig profile runtime dispatch covers efficient and invalid requests",
          "[signal][fm-operator][trig-profile]") {
    using Engine = pulp::signal::FmOperatorEngine;
    Engine block_engine;
    Engine sample_engine;
    for (auto* engine : {&block_engine, &sample_engine}) {
        engine->prepare(kSampleRate);
        engine->set_base_frequency_hz(375.0f);
        set_sustained_envelopes(*engine);
        REQUIRE(set_float_trig_profile(*engine, Engine::TrigProfile::realtime_efficient));
        engine->note_on();
    }

    std::array<float, 257> block_output{};
    std::array<float, 257> sample_output{};
    process_float_block(block_engine, block_output.data(), block_output.size());
    for (auto& sample : sample_output)
        sample = process_float_sample(sample_engine);
    REQUIRE(block_output == sample_output);

    process_float_block(block_engine, nullptr, block_output.size());
    const auto invalid =
        static_cast<Engine::TrigProfile>(std::numeric_limits<std::uint8_t>::max());
    REQUIRE_FALSE(set_float_trig_profile(block_engine, invalid));
    REQUIRE(block_engine.trig_profile() == Engine::TrigProfile::realtime_efficient);
}

TEST_CASE("Per-operator envelopes expose delay and hold stages",
          "[signal][fm-operator][envelope]") {
    pulp::signal::FmOperatorEngine64 engine;
    engine.prepare(kSampleRate);
    engine.set_base_frequency_hz(375.0);
    auto envelope = engine.operator_envelope(0);
    envelope.delay_ms = 5.0; // 240 samples
    envelope.attack_ms = 0.0;
    envelope.hold_ms = 5.0; // 240 samples
    envelope.decay_ms = 0.0;
    envelope.sustain = 0.25;
    envelope.release_ms = 0.0;
    engine.set_operator_envelope(0, envelope);
    engine.note_on();

    // The shared envelope transitions on its final delay tick, returning the
    // first hold value on that tick; this is the established DahdsrT law.
    for (int sample = 0; sample < 239; ++sample)
        REQUIRE(engine.process() == 0.0);

    for (int sample = 239; sample < 479; ++sample) {
        const double phase = 375.0 * static_cast<double>(sample + 1) / kSampleRate;
        REQUIRE(engine.process() == Catch::Approx(std::sin(kTwoPi * phase)).margin(1e-12));
    }

    const double sustain_phase = 375.0 * 480.0 / kSampleRate;
    REQUIRE(engine.process() ==
            Catch::Approx(0.25 * std::sin(kTwoPi * sustain_phase)).margin(1e-12));
}

TEST_CASE("Configuration and recursive state contain non-finite extremes",
          "[signal][fm-operator][finite]") {
    pulp::signal::FmOperatorEngine engine;
    engine.set_operator_count(999);
    REQUIRE(engine.operator_count() == engine.max_operator_count());
    engine.set_base_frequency_hz(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(engine.base_frequency_hz() == 440.0f);
    engine.set_operator_ratio(0, std::numeric_limits<float>::infinity());
    REQUIRE(engine.operator_ratio(0) == 1.0f);
    engine.set_operator_frequency_mode(0, pulp::signal::FmOperatorEngine::FrequencyMode::fixed_hz);
    engine.set_operator_frequency_mode(0,
                                       static_cast<pulp::signal::FmOperatorEngine::FrequencyMode>(
                                           std::numeric_limits<std::uint8_t>::max()));
    REQUIRE(engine.operator_frequency_mode(0) ==
            pulp::signal::FmOperatorEngine::FrequencyMode::fixed_hz);
    engine.set_alias_policy(pulp::signal::FmOperatorEngine::AliasPolicy::bounded);
    engine.set_alias_policy(static_cast<pulp::signal::FmOperatorEngine::AliasPolicy>(
        std::numeric_limits<std::uint8_t>::max()));
    REQUIRE(engine.alias_policy() == pulp::signal::FmOperatorEngine::AliasPolicy::bounded);
    engine.set_operator_feedback_radians(0, std::numeric_limits<float>::max());
    REQUIRE(engine.operator_feedback_radians(0) == engine.kMaxFeedbackRadians);
    engine.routing().set_phase_modulation_radians(0, 0, std::numeric_limits<float>::quiet_NaN());
    REQUIRE(engine.routing().phase_modulation_radians(0, 0) == 0.0f);
    engine.prepare(std::numeric_limits<double>::max());
    REQUIRE(engine.tail_samples() >= 0);

    engine.prepare(kSampleRate);
    engine.set_base_frequency_hz(engine.kMaxFrequencyHz);
    for (std::size_t op = 0; op < engine.operator_count(); ++op) {
        engine.set_operator_level(op, 1.0f);
        engine.set_operator_feedback_radians(op, engine.kMaxFeedbackRadians);
        engine.routing().set_carrier_gain(op, 1.0f);
        for (std::size_t source = 0; source < engine.operator_count(); ++source) {
            engine.routing().set_phase_modulation_radians(
                op, source, engine.routing().kMaxPhaseDeviationRadians);
            engine.routing().set_frequency_modulation_hz(op, source,
                                                         engine.routing().kMaxFrequencyDeviationHz);
        }
    }
    set_sustained_envelopes(engine);
    engine.note_on();
    for (int sample = 0; sample < 8192; ++sample)
        REQUIRE(std::isfinite(engine.process()));
}

TEST_CASE("The shared evaluator fails closed on invalid dimensions and sample rates",
          "[signal][fm-operator][finite]") {
    constexpr std::size_t max_operators = 2;
    std::array<double, max_operators> phases{};
    std::array<double, max_operators> previous{};
    std::array<double, max_operators> amplitudes = {1.0, 1.0};
    std::array<double, max_operators> feedback{};
    const auto frequency = [](std::size_t) { return 440.0; };
    const auto no_route = [](std::size_t, std::size_t) { return 0.0; };
    const auto carrier = [](std::size_t op) { return op == 0 ? 1.0 : 0.0; };
    const auto sine = [](std::size_t, double phase, double offset, double) {
        return std::sin(kTwoPi * phase + offset);
    };
    const auto render_invalid = [&](std::size_t operator_count, double sample_rate) {
        phases.fill(0.25);
        previous.fill(0.5);
        const double output = pulp::signal::detail::render_fm_operator_sample(
            operator_count, sample_rate, pulp::signal::FmOperatorAliasPolicy::bounded, phases,
            previous, amplitudes, feedback, frequency, no_route, no_route, carrier, sine);
        REQUIRE(output == 0.0);
        REQUIRE(
            std::all_of(phases.begin(), phases.end(), [](double value) { return value == 0.0; }));
        REQUIRE(std::all_of(previous.begin(), previous.end(),
                            [](double value) { return value == 0.0; }));
    };

    render_invalid(max_operators + 1, kSampleRate);
    render_invalid(1, 0.0);
    render_invalid(1, -kSampleRate);
    render_invalid(1, std::numeric_limits<double>::quiet_NaN());
}

TEST_CASE("Float and double operator engines allocate nothing while rendering",
          "[signal][fm-operator][rt-safety]") {
    pulp::signal::FmOperatorEngine float_engine;
    pulp::signal::FmOperatorEngine64 double_engine;
    float_engine.set_operator_count(8);
    float_engine.prepare(kSampleRate);
    REQUIRE(float_engine.set_trig_profile(
        pulp::signal::FmOperatorEngine::TrigProfile::realtime_precise));
    set_sustained_envelopes(float_engine, 20.0);
    double_engine.set_operator_count(8);
    double_engine.prepare(kSampleRate);
    set_sustained_envelopes(double_engine, 20.0);

    std::array<float, 257> float_output{};
    std::array<double, 257> double_output{};
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int repeat = 0; repeat < 8; ++repeat) {
            float_engine.note_on(0.75f);
            double_engine.note_on(0.75);
            float_engine.process(float_output.data(), float_output.size());
            double_engine.process(double_output.data(), double_output.size());
            float_engine.note_off();
            double_engine.note_off();
            float_engine.process(float_output.data(), float_output.size());
            double_engine.process(double_output.data(), double_output.size());
            float_engine.reset();
            double_engine.reset();
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}
