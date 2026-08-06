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

double sum(const Crossover::Frame& frame) {
    double result = 0.0;
    for (std::size_t band = 0; band < frame.count; ++band)
        result += frame.bands[band];
    return result;
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
            }
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

    pulp::test::RtAllocationProbe probe;
    for (int i = 0; i < 1024; ++i) {
        const double tiny = i == 0 ? 1.0e-300 : 0.0;
        REQUIRE(latency_probe.process(tiny).healthy);
    }
    REQUIRE(probe.allocation_count() == 0);
}
