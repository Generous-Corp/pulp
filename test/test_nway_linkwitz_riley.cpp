#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/linkwitz_riley.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Crossover = pulp::signal::LinkwitzRileyCrossoverT<double, 8>;

namespace {

struct ReferenceBiquad {
    long double b0 = 1.0L;
    long double b1 = 0.0L;
    long double b2 = 0.0L;
    long double a1 = 0.0L;
    long double a2 = 0.0L;
    long double s1 = 0.0L;
    long double s2 = 0.0L;

    long double process(long double input) {
        const long double output = b0 * input + s1;
        s1 = b1 * input - a1 * output + s2;
        s2 = b2 * input - a2 * output;
        return output;
    }
};

ReferenceBiquad reference_section(bool lowpass, long double cutoff, long double sample_rate) {
    const long double omega = 2.0L * std::numbers::pi_v<long double> * cutoff / sample_rate;
    const long double cosine = std::cos(omega);
    constexpr long double q =
        static_cast<long double>(pulp::signal::LinkwitzRileyT<double>::exact_butterworth_q);
    const long double alpha = std::sin(omega) / (2.0L * q);
    const long double a0 = 1.0L + alpha;
    ReferenceBiquad section;
    if (lowpass) {
        section.b0 = (1.0L - cosine) * 0.5L / a0;
        section.b1 = (1.0L - cosine) / a0;
        section.b2 = section.b0;
    } else {
        section.b0 = (1.0L + cosine) * 0.5L / a0;
        section.b1 = -(1.0L + cosine) / a0;
        section.b2 = section.b0;
    }
    section.a1 = -2.0L * cosine / a0;
    section.a2 = (1.0L - alpha) / a0;
    return section;
}

std::complex<long double> reference_section_response(bool lowpass, long double cutoff,
                                                     long double frequency,
                                                     long double sample_rate) {
    const auto c = reference_section(lowpass, cutoff, sample_rate);
    const long double omega =
        std::clamp(2.0L * std::numbers::pi_v<long double> * frequency / sample_rate, 0.0L,
                   std::numbers::pi_v<long double>);
    const auto z1 = std::polar(1.0L, -omega);
    const auto z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0L + c.a1 * z1 + c.a2 * z2);
}

std::complex<long double> reference_coefficients_response(const ReferenceBiquad& c,
                                                          long double frequency,
                                                          long double sample_rate) {
    const long double omega = 2.0L * std::numbers::pi_v<long double> * frequency / sample_rate;
    const auto z1 = std::polar(1.0L, -omega);
    const auto z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0L + c.a1 * z1 + c.a2 * z2);
}

ReferenceBiquad reference_section_from_warped(bool lowpass, long double warped) {
    constexpr long double q =
        static_cast<long double>(pulp::signal::LinkwitzRileyT<double>::exact_butterworth_q);
    const long double square = warped * warped;
    const long double normalization = 1.0L / (1.0L + warped / q + square);
    const long double branch_b0 = (lowpass ? square : 1.0L) * normalization;
    ReferenceBiquad result;
    result.b0 = branch_b0;
    result.b1 = (lowpass ? 2.0L : -2.0L) * branch_b0;
    result.b2 = branch_b0;
    result.a1 = 2.0L * (square - 1.0L) * normalization;
    result.a2 = (1.0L - warped / q + square) * normalization;
    return result;
}

std::complex<long double>
reference_interpolated_reconstruction_response(std::span<const float> from,
                                               std::span<const float> to, long double amount,
                                               long double frequency, long double sample_rate) {
    std::complex<long double> response{1.0L, 0.0L};
    for (std::size_t split = 0; split < from.size(); ++split) {
        const long double from_warped =
            std::tan(std::numbers::pi_v<long double> * from[split] / sample_rate);
        const long double to_warped =
            std::tan(std::numbers::pi_v<long double> * to[split] / sample_rate);
        const long double warped = from_warped * std::pow(to_warped / from_warped, amount);
        const auto low = reference_section_from_warped(true, warped);
        const auto high = reference_section_from_warped(false, warped);
        const auto low_once = reference_coefficients_response(low, frequency, sample_rate);
        const auto high_once = reference_coefficients_response(high, frequency, sample_rate);
        response *= low_once * low_once + high_once * high_once;
    }
    return response;
}

std::complex<long double> reference_reconstruction_response(std::span<const double> cutoffs,
                                                            long double frequency,
                                                            long double sample_rate) {
    std::complex<long double> response{1.0L, 0.0L};
    for (const double cutoff : cutoffs) {
        const auto low = reference_section_response(true, cutoff, frequency, sample_rate);
        const auto high = reference_section_response(false, cutoff, frequency, sample_rate);
        response *= low * low + high * high;
    }
    return response;
}

std::complex<long double> reference_band_response(std::span<const double> cutoffs, std::size_t band,
                                                  long double frequency, long double sample_rate) {
    std::complex<long double> response{1.0L, 0.0L};
    auto split_response = [&](std::size_t split, bool lowpass) {
        const auto once =
            reference_section_response(lowpass, cutoffs[split], frequency, sample_rate);
        return once * once;
    };
    for (std::size_t split = 0; split < band; ++split)
        response *= split_response(split, false);
    if (band < cutoffs.size())
        response *= split_response(band, true);
    for (std::size_t split = band + 1; split < cutoffs.size(); ++split)
        response *= split_response(split, true) + split_response(split, false);
    return response;
}

struct ReferenceAllpassCascade {
    struct Split {
        ReferenceBiquad low1, low2, high1, high2;
    };
    std::array<Split, 7> splits{};
    std::size_t count = 0;

    ReferenceAllpassCascade(std::span<const double> cutoffs, double sample_rate)
        : count(cutoffs.size()) {
        for (std::size_t i = 0; i < count; ++i) {
            splits[i].low1 = reference_section(true, cutoffs[i], sample_rate);
            splits[i].low2 = reference_section(true, cutoffs[i], sample_rate);
            splits[i].high1 = reference_section(false, cutoffs[i], sample_rate);
            splits[i].high2 = reference_section(false, cutoffs[i], sample_rate);
        }
    }

    long double process(long double input) {
        for (std::size_t i = 0; i < count; ++i) {
            const long double low = splits[i].low2.process(splits[i].low1.process(input));
            const long double high = splits[i].high2.process(splits[i].high1.process(input));
            input = low + high;
        }
        return input;
    }
};

struct ReferenceNway {
    struct Split {
        ReferenceBiquad low1, low2, high1, high2;
    };
    std::array<Split, 7> splitters{};
    std::array<std::array<Split, 7>, 7> compensators{};
    std::size_t count = 0;

    ReferenceNway(std::span<const double> cutoffs, double sample_rate) : count(cutoffs.size()) {
        auto configure = [&](Split& split, double cutoff) {
            split.low1 = reference_section(true, cutoff, sample_rate);
            split.low2 = reference_section(true, cutoff, sample_rate);
            split.high1 = reference_section(false, cutoff, sample_rate);
            split.high2 = reference_section(false, cutoff, sample_rate);
        };
        for (std::size_t split = 0; split < count; ++split) {
            configure(splitters[split], cutoffs[split]);
            for (std::size_t band = 0; band < split; ++band)
                configure(compensators[band][split], cutoffs[split]);
        }
    }

    static std::array<long double, 2> process_split(Split& split, long double input) {
        return {split.low2.process(split.low1.process(input)),
                split.high2.process(split.high1.process(input))};
    }

    std::array<long double, 8> process(long double input) {
        std::array<long double, 8> bands{};
        long double remainder = input;
        for (std::size_t split = 0; split < count; ++split) {
            const auto divided = process_split(splitters[split], remainder);
            bands[split] = divided[0];
            remainder = divided[1];
        }
        bands[count] = remainder;
        for (std::size_t band = 0; band < count; ++band) {
            for (std::size_t split = band + 1; split < count; ++split) {
                const auto aligned = process_split(compensators[band][split], bands[band]);
                bands[band] = aligned[0] + aligned[1];
            }
        }
        return bands;
    }
};

template <typename Frame> double sum(const Frame& frame) {
    double result = 0.0;
    for (std::size_t band = 0; band < frame.count; ++band)
        result += static_cast<double>(frame.bands[band]);
    return result;
}

template <typename Actual, typename Expected>
double wrapped_phase_error(std::complex<Actual> actual, std::complex<Expected> expected) {
    return std::abs(std::remainder(static_cast<double>(std::arg(actual) - std::arg(expected)),
                                   2.0 * std::numbers::pi));
}

} // namespace

TEST_CASE("N-way Linkwitz-Riley rejects invalid topology without changing prepared state",
          "[signal][crossover]") {
    Crossover crossover;
    const std::array valid{300.0, 1500.0, 6000.0};
    REQUIRE(crossover.prepare(48000.0, valid));

    const std::array unordered{300.0, 300.0, 6000.0};
    const std::array outside{300.0, 1500.0, 24000.0};
    const std::array nonfinite{300.0, 1500.0, std::numeric_limits<double>::infinity()};
    REQUIRE_FALSE(crossover.set_cutoffs(unordered));
    REQUIRE_FALSE(crossover.set_cutoffs(outside));
    REQUIRE_FALSE(crossover.set_cutoffs(nonfinite));
    REQUIRE(crossover.band_count() == 4);
    REQUIRE(crossover.cutoff(1) == 1500.0);
    REQUIRE(crossover.process(0.25).healthy);

    Crossover unprepared;
    REQUIRE_FALSE(unprepared.prepare(0.0, valid));
    REQUIRE_FALSE(unprepared.prepare(48000.0, std::span<const double>{}));
    REQUIRE_FALSE(unprepared.set_cutoffs(valid));
    REQUIRE(unprepared.process(1.0).count == 0);
}

TEST_CASE("N-way Linkwitz-Riley work preserves the historical two-band coefficient and render path",
          "[signal][crossover][compatibility]") {
    auto prove_parity = []<typename Sample>() {
        pulp::signal::LinkwitzRileyT<Sample> legacy;
        std::array<pulp::signal::BiquadT<Sample>, 4> reference;
        constexpr Sample cutoff = static_cast<Sample>(1733.0);
        constexpr Sample sample_rate = static_cast<Sample>(48000.0);
        legacy.set_frequency(cutoff, sample_rate);
        reference[0].set_coefficients(pulp::signal::BiquadT<Sample>::Type::lowpass, cutoff,
                                      Sample{0.707f}, sample_rate);
        reference[1].set_coefficients(pulp::signal::BiquadT<Sample>::Type::lowpass, cutoff,
                                      Sample{0.707f}, sample_rate);
        reference[2].set_coefficients(pulp::signal::BiquadT<Sample>::Type::highpass, cutoff,
                                      Sample{0.707f}, sample_rate);
        reference[3].set_coefficients(pulp::signal::BiquadT<Sample>::Type::highpass, cutoff,
                                      Sample{0.707f}, sample_rate);
        std::uint32_t rng = 0x6d2b79f5u;
        for (std::size_t sample = 0; sample < 8192; ++sample) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            const Sample input = static_cast<Sample>(static_cast<double>(rng) / 4294967295.0 - 0.5);
            const auto actual = legacy.process(input);
            const Sample expected_low = reference[1].process(reference[0].process(input));
            const Sample expected_high = reference[3].process(reference[2].process(input));
            REQUIRE(actual.low == expected_low);
            REQUIRE(actual.high == expected_high);
        }
    };
    prove_parity.template operator()<float>();
    prove_parity.template operator()<double>();
}

TEST_CASE("N-way Linkwitz-Riley reconstruction matches an independent long-double oracle",
          "[signal][crossover][audio]") {
    const std::array sample_rates{44100.0, 48000.0, 96000.0};
    const std::array all_cutoffs{180.0, 900.0, 3600.0, 12000.0};

    for (const double sample_rate : sample_rates) {
        for (const std::size_t cutoff_count : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            const auto cutoffs = std::span<const double>(all_cutoffs.data(), cutoff_count);
            Crossover crossover;
            REQUIRE(crossover.prepare(sample_rate, cutoffs));
            ReferenceAllpassCascade oracle(cutoffs, sample_rate);

            double worst_error = 0.0;
            double output_energy = 0.0;
            for (std::size_t sample = 0; sample < 32768; ++sample) {
                const double impulse = sample == 0 ? 1.0 : 0.0;
                const auto frame = crossover.process(impulse);
                REQUIRE(frame.healthy);
                const double rendered = sum(frame);
                const double expected = static_cast<double>(oracle.process(impulse));
                worst_error = std::max(worst_error, std::abs(rendered - expected));
                output_energy += rendered * rendered;
            }
            INFO("sample_rate=" << sample_rate << " bands=" << cutoff_count + 1);
            REQUIRE(worst_error < 2.0e-12);
            REQUIRE(output_energy > 0.0);

            for (const double frequency : {0.0, 20.0, 180.0, 1000.0, 8000.0, sample_rate * 0.499}) {
                const auto measured =
                    Crossover::reconstruction_response(cutoffs, frequency, sample_rate);
                const auto expected =
                    reference_reconstruction_response(cutoffs, frequency, sample_rate);
                INFO("frequency=" << frequency);
                REQUIRE_THAT(std::abs(measured), WithinAbs(1.0, 1.0e-10));
                REQUIRE_THAT(measured.real(),
                             WithinAbs(static_cast<double>(expected.real()), 1.0e-10));
                REQUIRE_THAT(measured.imag(),
                             WithinAbs(static_cast<double>(expected.imag()), 1.0e-10));
                for (std::size_t band = 0; band <= cutoffs.size(); ++band) {
                    const auto measured_band =
                        Crossover::band_response(cutoffs, band, frequency, sample_rate);
                    const auto expected_band =
                        reference_band_response(cutoffs, band, frequency, sample_rate);
                    INFO("band=" << band);
                    REQUIRE_THAT(measured_band.real(),
                                 WithinAbs(static_cast<double>(expected_band.real()), 1.0e-10));
                    REQUIRE_THAT(measured_band.imag(),
                                 WithinAbs(static_cast<double>(expected_band.imag()), 1.0e-10));
                }
            }
        }
    }
}

TEST_CASE("N-way Linkwitz-Riley bands match an independent runtime realization",
          "[signal][crossover][audio]") {
    const std::array cutoffs{220.0, 1100.0, 4200.0, 12000.0};
    ReferenceNway reference(cutoffs, 48000.0);
    Crossover runtime;
    REQUIRE(runtime.prepare(48000.0, cutoffs));

    for (std::size_t sample = 0; sample < 16384; ++sample) {
        const double input = sample == 0 ? 1.0 : 0.0;
        const auto actual = runtime.process(input);
        const auto expected = reference.process(input);
        for (std::size_t band = 0; band < actual.count; ++band) {
            INFO("sample=" << sample << " band=" << band);
            REQUIRE_THAT(actual.bands[band], WithinAbs(static_cast<double>(expected[band]), 3e-12));
        }
    }

    using FloatCrossover = pulp::signal::LinkwitzRileyCrossover;
    const std::array<float, 4> float_cutoffs{220.0f, 1100.0f, 4200.0f, 12000.0f};
    ReferenceNway float_reference(cutoffs, 48000.0);
    FloatCrossover float_runtime;
    REQUIRE(float_runtime.prepare(48000.0f, float_cutoffs));
    for (std::size_t sample = 0; sample < 16384; ++sample) {
        const float input = sample == 0 ? 1.0f : 0.0f;
        const auto actual = float_runtime.process(input);
        const auto expected = float_reference.process(input);
        for (std::size_t band = 0; band < actual.count; ++band) {
            INFO("float sample=" << sample << " band=" << band);
            REQUIRE_THAT(actual.bands[band],
                         WithinAbs(static_cast<float>(expected[band]), 2.0e-7f));
        }
    }
}

TEST_CASE("N-way Linkwitz-Riley renders deterministic sine and noise across caller blocks",
          "[signal][crossover][audio]") {
    const std::array cutoffs{250.0, 1200.0, 5000.0};
    auto render = [&](std::span<const std::size_t> blocks) {
        Crossover crossover;
        REQUIRE(crossover.prepare(48000.0, cutoffs));
        std::vector<double> result;
        result.reserve(8192);
        std::uint32_t rng = 0x12345678u;
        std::size_t cursor = 0;
        for (std::size_t block = 0; cursor < 8192; ++block) {
            const std::size_t count = std::min(blocks[block % blocks.size()], 8192 - cursor);
            for (std::size_t i = 0; i < count; ++i, ++cursor) {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                const double noise = (static_cast<double>(rng) / 4294967295.0 - 0.5) * 0.1;
                const double sine = 0.4 * std::sin(2.0 * std::numbers::pi * 997.0 *
                                                   static_cast<double>(cursor) / 48000.0);
                result.push_back(sum(crossover.process(sine + noise)));
            }
        }
        return result;
    };

    const std::array<std::size_t, 1> regular{128};
    const std::array<std::size_t, 5> irregular{1, 31, 257, 64, 7};
    REQUIRE(render(regular) == render(irregular));
    REQUIRE(render(regular) == render(regular));
}

TEST_CASE("N-way Linkwitz-Riley retunes with a bounded deterministic transition",
          "[signal][crossover][automation]") {
    Crossover crossover;
    const std::array initial{400.0, 2000.0};
    const std::array target{700.0, 5000.0};
    REQUIRE(crossover.prepare(48000.0, initial));

    double previous = 0.0;
    for (std::size_t i = 0; i < 2048; ++i) {
        const double input =
            0.5 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(i) / 48000.0);
        previous = sum(crossover.process(input));
    }

    REQUIRE(crossover.set_cutoffs(target, 256));
    REQUIRE(crossover.transitioning());
    REQUIRE_FALSE(crossover.set_cutoffs(initial, 64));
    double worst_step = 0.0;
    for (std::size_t i = 2048; i < 2304; ++i) {
        const double input =
            0.5 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(i) / 48000.0);
        const double output = sum(crossover.process(input));
        worst_step = std::max(worst_step, std::abs(output - previous));
        previous = output;
    }
    REQUIRE_FALSE(crossover.transitioning());
    REQUIRE(worst_step < 0.15);
    REQUIRE(crossover.cutoff(0) == target[0]);

    Crossover target_fresh;
    REQUIRE(target_fresh.prepare(48000.0, target));
    REQUIRE(crossover.set_cutoffs(initial, 128));
    crossover.reset();
    target_fresh.prepare(48000.0, initial);
    for (int i = 0; i < 256; ++i) {
        const double input = i == 0 ? 1.0 : 0.0;
        REQUIRE(crossover.process(input).bands == target_fresh.process(input).bands);
    }

    Crossover immediate;
    REQUIRE(immediate.prepare(48000.0, initial));
    for (int i = 0; i < 512; ++i)
        (void)immediate.process(i == 0 ? 1.0 : 0.0);
    REQUIRE(immediate.set_cutoffs(target));
    REQUIRE(immediate.cutoff(0) == target[0]);
    REQUIRE(immediate.process(0.25).healthy);
}

TEST_CASE("N-way Linkwitz-Riley coefficient retune avoids adversarial phase cancellation",
          "[signal][crossover][automation][audio]") {
    using Runtime = pulp::signal::LinkwitzRileyCrossover;
    const std::array<float, 3> initial{100.0f, 500.0f, 2000.0f};
    const std::array<float, 3> target{1000.0f, 5000.0f, 15000.0f};
    constexpr float sample_rate = 48000.0f;
    constexpr std::size_t transition_samples = 65536;

    for (const double frequency : {100.0, 1000.0, 15958.0, 22000.0}) {
        Runtime runtime;
        REQUIRE(runtime.prepare(sample_rate, initial));
        std::size_t cursor = 0;
        auto input_at = [&](std::size_t sample) {
            return static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * frequency *
                                                     static_cast<double>(sample) / sample_rate));
        };
        for (; cursor < 32768; ++cursor)
            (void)runtime.process(input_at(cursor));

        REQUIRE(runtime.set_cutoffs(target, transition_samples));
        std::complex<double> input_phasor{};
        std::complex<double> output_phasor{};
        constexpr std::size_t window = 512;
        const std::size_t window_begin = transition_samples / 2 - window / 2;
        const std::size_t window_end = window_begin + window;
        bool all_finite = true;
        for (std::size_t transition_sample = 0; transition_sample < transition_samples;
             ++transition_sample, ++cursor) {
            const float input = input_at(cursor);
            const float output = static_cast<float>(sum(runtime.process(input)));
            all_finite = all_finite && std::isfinite(output);
            if (transition_sample >= window_begin && transition_sample < window_end) {
                const auto demod = std::polar(1.0, -2.0 * std::numbers::pi * frequency *
                                                       static_cast<double>(cursor) / sample_rate);
                input_phasor += static_cast<double>(input) * demod;
                output_phasor += static_cast<double>(output) * demod;
            }
        }
        REQUIRE(all_finite);
        REQUIRE_FALSE(runtime.transitioning());

        const auto measured = output_phasor / input_phasor;
        const auto expected = reference_interpolated_reconstruction_response(
            initial, target, 0.5L, frequency, sample_rate);
        INFO("frequency=" << frequency << " measured=" << measured << " expected=" << expected);
        REQUIRE(std::abs(measured) > 0.90);
        REQUIRE(std::abs(measured) < 1.15);
        REQUIRE(wrapped_phase_error(measured, expected) < 0.12);
    }

    Runtime one_sample;
    REQUIRE(one_sample.prepare(sample_rate, initial));
    std::size_t cursor = 0;
    for (; cursor < 4096; ++cursor)
        (void)one_sample.process(
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 15958.0 *
                                              static_cast<double>(cursor) / sample_rate)));
    REQUIRE(one_sample.set_cutoffs(target, 1));
    const auto switched = one_sample.process(
        static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 15958.0 *
                                          static_cast<double>(cursor) / sample_rate)));
    REQUIRE(switched.healthy);
    REQUIRE_FALSE(one_sample.transitioning());
    std::complex<double> one_sample_input{};
    std::complex<double> one_sample_output{};
    for (std::size_t sample = 0; sample < 1024; ++sample, ++cursor) {
        const float input =
            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 15958.0 *
                                              static_cast<double>(cursor) / sample_rate));
        const double output = sum(one_sample.process(input));
        const auto demod = std::polar(1.0, -2.0 * std::numbers::pi * 15958.0 *
                                               static_cast<double>(cursor) / sample_rate);
        one_sample_input += static_cast<double>(input) * demod;
        one_sample_output += output * demod;
    }
    const auto one_sample_measured = one_sample_output / one_sample_input;
    const auto one_sample_expected = Runtime::reconstruction_response(target, 15958.0, sample_rate);
    REQUIRE(std::abs(one_sample_measured) > 0.85);
    REQUIRE(std::abs(one_sample_measured) < 1.15);
    REQUIRE(wrapped_phase_error(one_sample_measured, one_sample_expected) < 0.15);
}

TEST_CASE("N-way Linkwitz-Riley validates its numerical support domain and maximum topology",
          "[signal][crossover][stability]") {
    using FloatCrossover = pulp::signal::LinkwitzRileyCrossover;
    const std::array<float, 1> one_hz{1.0f};
    const std::array<float, 1> very_low{0.01f};
    REQUIRE(FloatCrossover::supports_configuration(48000.0f, one_hz));
    REQUIRE(FloatCrossover::supports_configuration(48000.0f, very_low));

    const std::array<float, 1> degenerate_low{std::numeric_limits<float>::denorm_min()};
    const std::array<float, 1> highest_float_below_nyquist{std::nextafter(24000.0f, 0.0f)};
    const std::array<float, 1> at_nyquist{24000.0f};
    REQUIRE_FALSE(FloatCrossover::supports_configuration(48000.0f, degenerate_low));
    REQUIRE(FloatCrossover::supports_configuration(48000.0f, highest_float_below_nyquist));
    REQUIRE_FALSE(FloatCrossover::supports_configuration(48000.0f, at_nyquist));

    FloatCrossover long_decay;
    REQUIRE(long_decay.prepare(48000.0f, one_hz));
    double late_peak = 0.0;
    for (std::size_t sample = 0; sample < 700000; ++sample) {
        const auto frame = long_decay.process(sample == 0 ? 1.0f : 0.0f);
        if (sample >= 650000)
            late_peak = std::max(late_peak, std::abs(sum(frame)));
    }
    REQUIRE(long_decay.healthy());
    REQUIRE(long_decay.fault_count() == 0);
    REQUIRE(late_peak < 1.0e-6);

    FloatCrossover sub_hertz;
    REQUIRE(sub_hertz.prepare(48000.0f, very_low));
    double peak = 0.0;
    double worst_reference_error = 0.0;
    const std::array<double, 1> reference_cutoff{0.01};
    ReferenceNway sub_hertz_reference(reference_cutoff, 48000.0);
    for (std::size_t sample = 0; sample < 700000; ++sample) {
        const auto frame = sub_hertz.process(sample == 0 ? 1.0f : 0.0f);
        const auto expected = sub_hertz_reference.process(sample == 0 ? 1.0L : 0.0L);
        peak = std::max(peak, std::abs(sum(frame)));
        for (std::size_t band = 0; band < frame.count; ++band) {
            worst_reference_error =
                std::max(worst_reference_error, std::abs(static_cast<double>(frame.bands[band]) -
                                                         static_cast<double>(expected[band])));
        }
    }
    REQUIRE(sub_hertz.healthy());
    REQUIRE(sub_hertz.fault_count() == 0);
    REQUIRE(peak < 1.01);
    REQUIRE(worst_reference_error < 2.0e-7);

    using TwoBand = pulp::signal::LinkwitzRileyCrossoverT<float, 2>;
    TwoBand two_band;
    REQUIRE(two_band.prepare(8000.0f, std::array<float, 1>{1000.0f}));
    REQUIRE(two_band.band_count() == 2);
    REQUIRE(
        FloatCrossover::supports_configuration(192000.0f, std::array<float, 2>{0.01f, 95000.0f}));

    using NineBand = pulp::signal::LinkwitzRileyCrossoverT<double, 9>;
    const std::array<double, 8> maximum_cutoffs{40.0,   100.0,  250.0,  600.0,
                                                1500.0, 4000.0, 9000.0, 18000.0};
    NineBand maximum;
    REQUIRE(maximum.prepare(96000.0, maximum_cutoffs));
    REQUIRE(maximum.band_count() == 9);
    bool maximum_healthy = true;
    for (std::size_t sample = 0; sample < 32768; ++sample)
        maximum_healthy = maximum_healthy && maximum.process(sample == 0 ? 1.0 : 0.0).healthy;
    REQUIRE(maximum_healthy);

    using DoubleCrossover = pulp::signal::LinkwitzRileyCrossover64;
    REQUIRE_FALSE(DoubleCrossover::supports_configuration(
        48000.0, std::array<double, 1>{std::numeric_limits<double>::denorm_min()}));
    REQUIRE_FALSE(DoubleCrossover::supports_configuration(
        48000.0, std::array<double, 1>{std::nextafter(24000.0, 0.0)}));

    using ExtendedApiCrossover = pulp::signal::LinkwitzRileyCrossoverT<long double, 3>;
    ExtendedApiCrossover extended_api;
    REQUIRE(extended_api.prepare(48000.0L, std::array<long double, 2>{0.01L, 12000.0L}));
    REQUIRE(extended_api.process(1.0L).healthy);

    const auto beyond_nyquist = FloatCrossover::reconstruction_response(one_hz, 24000.01, 48000.0);
    REQUIRE(beyond_nyquist == std::complex<double>{});
}

TEST_CASE("N-way Linkwitz-Riley reset, latency, faults, and RT storage are explicit",
          "[signal][crossover][rt-safety]") {
    const std::array cutoffs{300.0, 1800.0, 7200.0};
    Crossover fresh;
    Crossover reused;
    REQUIRE(fresh.prepare(48000.0, cutoffs));
    REQUIRE(reused.prepare(48000.0, cutoffs));
    for (int i = 0; i < 512; ++i)
        (void)reused.process((i % 5 - 2) * 0.1);
    reused.reset();

    for (int i = 0; i < 512; ++i) {
        const double input = i == 0 ? 1.0 : 0.0;
        REQUIRE(fresh.process(input).bands == reused.process(input).bands);
    }
    REQUIRE(Crossover::latency_samples() == 0);

    Crossover latency_probe;
    REQUIRE(latency_probe.prepare(48000.0, cutoffs));
    const auto first = latency_probe.process(1.0);
    REQUIRE(std::abs(sum(first)) > 0.0);

    const auto bad = latency_probe.process(std::numeric_limits<double>::quiet_NaN());
    REQUIRE_FALSE(bad.healthy);
    REQUIRE(sum(bad) == 0.0);
    REQUIRE_FALSE(latency_probe.healthy());
    REQUIRE(latency_probe.fault_count() == 1);
    REQUIRE(latency_probe.process(0.25).healthy);

    bool overload_recovered = false;
    for (int i = 0; i < 16; ++i) {
        const auto overload =
            latency_probe.process((i & 1) == 0 ? std::numeric_limits<double>::max()
                                               : -std::numeric_limits<double>::max());
        if (!overload.healthy) {
            REQUIRE(sum(overload) == 0.0);
            overload_recovered = true;
            break;
        }
    }
    REQUIRE(overload_recovered);
    REQUIRE(latency_probe.process(0.25).healthy);

    const std::array retuned_cutoffs{450.0, 2400.0, 9000.0};
    pulp::test::RtAllocationProbe probe;
    REQUIRE(latency_probe.set_cutoffs(retuned_cutoffs, 1024));
    for (int i = 0; i < 1024; ++i) {
        const double tiny = i == 0 ? 1.0e-300 : 0.0;
        REQUIRE(latency_probe.process(tiny).healthy);
    }
    REQUIRE(probe.allocation_count() == 0);
}
