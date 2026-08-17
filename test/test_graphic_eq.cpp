#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/graphic_eq.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::GraphicEqConfigureStatus;
using pulp::signal::GraphicEqPrepareStatus;

namespace {

struct ReferenceSection {
    long double b0 = 1.0L, b1 = 0.0L, b2 = 0.0L;
    long double a1 = 0.0L, a2 = 0.0L;
    long double s1 = 0.0L, s2 = 0.0L;

    long double process(long double input) {
        const long double output = b0 * input + s1;
        s1 = b1 * input - a1 * output + s2;
        s2 = b2 * input - a2 * output;
        return output;
    }
};

template <typename Band> ReferenceSection reference_peaking(Band band, long double sample_rate) {
    if (band.gain_db == 0)
        return {};
    const long double pi = std::acos(-1.0L);
    const long double omega = 2.0L * pi * static_cast<long double>(band.frequency_hz) / sample_rate;
    const long double cosine = std::cos(omega);
    const long double alpha = std::sin(omega) / (2.0L * static_cast<long double>(band.q));
    const long double amplitude = std::pow(10.0L, static_cast<long double>(band.gain_db) / 40.0L);
    const long double a0 = 1.0L + alpha / amplitude;
    return {(1.0L + alpha * amplitude) / a0, (-2.0L * cosine) / a0, (1.0L - alpha * amplitude) / a0,
            (-2.0L * cosine) / a0, (1.0L - alpha / amplitude) / a0};
}

template <typename Band, std::size_t Size>
long double reference_magnitude(const std::array<Band, Size>& bands, std::size_t count,
                                long double frequency_hz, long double sample_rate) {
    const long double omega = 2.0L * std::acos(-1.0L) * frequency_hz / sample_rate;
    const auto z1 = std::polar(1.0L, -omega);
    const auto z2 = z1 * z1;
    long double result = 1.0L;
    for (std::size_t index = 0; index < count; ++index) {
        const auto c = reference_peaking(bands[index], sample_rate);
        const auto numerator = c.b0 + c.b1 * z1 + c.b2 * z2;
        const auto denominator = 1.0L + c.a1 * z1 + c.a2 * z2;
        result *= std::abs(numerator) / std::abs(denominator);
    }
    return result;
}

// Bounded from BOTH sides by measurement, not guessed.
//
// Lower bound: float accumulates real error through a recursive four-section
// cascade, and the long double reference is wider on x86-64 (80-bit) than on
// arm64 (64-bit), so the float path diverges further from it on x86-64. The
// observed worst case there is 1.004e-4, which this must clear.
//
// Upper bound: the planted-mutation control below shows a 0.01 dB band-gain
// error produces only 3.40e-4 of divergence. A tolerance at or above that would
// be blind to it -- an earlier 5.0e-4 draft was, which is how this bound was
// found.
//
// 2.0e-4 sits between the two with roughly 2x headroom over platform noise while
// still rejecting a hundredth-of-a-decibel coefficient error.
inline constexpr double kFloatReferenceTolerance = 2.0e-4;

template <typename T> void require_processing_matches_independent_reference() {
    using Eq = pulp::signal::GraphicEqT<T, 8>;
    using Band = typename Eq::Band;
    constexpr std::array<Band, 4> bands{{
        {T{63}, T{5.5}, T{0.8}},
        {T{250}, T{-8}, T{1.4}},
        {T{1000}, T{3}, T{2.2}},
        {T{8000}, T{-4.5}, T{0.7}},
    }};

    Eq eq;
    REQUIRE(eq.prepare(T{48000}, 8) == GraphicEqPrepareStatus::prepared);
    REQUIRE(eq.configure(bands) == GraphicEqConfigureStatus::configured);

    std::array<ReferenceSection, bands.size()> reference{};
    for (std::size_t index = 0; index < bands.size(); ++index)
        reference[index] = reference_peaking(bands[index], 48000.0L);

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        const T input = static_cast<T>(
            sample == 0 ? 1.0L
                        : 0.4L * std::sin(0.017L * static_cast<long double>(sample)) +
                              0.2L * std::cos(0.071L * static_cast<long double>(sample)));
        long double expected = static_cast<long double>(input);
        for (auto& section : reference)
            expected = section.process(expected);
        const T actual = eq.process(input);
        // The reference cascade runs in long double, whose width is platform
        // dependent: 80-bit on x86-64, 64-bit on arm64. The reference is
        // therefore MORE precise on x86-64, so the float path's divergence from
        // it is genuinely larger there. The float bound has to cover the wider
        // of the two, not the arm64 value alone. The double path compares two
        // 64-bit computations and stays tight.
        REQUIRE_THAT(
            static_cast<double>(actual),
            WithinAbs(static_cast<double>(expected),
                      std::is_same_v<T, float> ? kFloatReferenceTolerance : 2.0e-11));
    }
}

} // namespace

TEST_CASE("GraphicEq publishes bounded lifecycle and control domains", "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<float, 4>;
    using Band = Eq::Band;
    Eq eq;

    constexpr std::array<Band, 1> one_band{{{1000.0f, 3.0f, 1.0f}}};
    REQUIRE(eq.configure(one_band) == GraphicEqConfigureStatus::not_prepared);
    REQUIRE(eq.prepare(7999.0f, 4) == GraphicEqPrepareStatus::invalid_sample_rate);
    REQUIRE(eq.prepare(48000.0f, 0) == GraphicEqPrepareStatus::invalid_capacity);
    REQUIRE(eq.prepare(48000.0f, 5) == GraphicEqPrepareStatus::invalid_capacity);
    REQUIRE(eq.prepare(48000.0f, 4) == GraphicEqPrepareStatus::prepared);
    REQUIRE(eq.capacity() == 4);
    REQUIRE(eq.supported_frequency_ceiling_hz() == 20000.0f);
    REQUIRE(Eq::storage_capacity() == 4);

    constexpr std::array<Band, 5> too_many{};
    REQUIRE(eq.configure(too_many) == GraphicEqConfigureStatus::over_capacity);
    REQUIRE(eq.configure(one_band, std::numeric_limits<std::size_t>::max()) ==
            GraphicEqConfigureStatus::invalid_transition);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array nonfinite{Band{1000.0f, nan, 1.0f}};
    const std::array low_frequency{Band{19.0f, 0.0f, 1.0f}};
    const std::array high_frequency{Band{20001.0f, 0.0f, 1.0f}};
    const std::array unordered{Band{1000.0f, 0.0f, 1.0f}, Band{999.0f, 0.0f, 1.0f}};
    const std::array bad_gain{Band{1000.0f, 24.01f, 1.0f}};
    const std::array bad_q{Band{1000.0f, 0.0f, 0.09f}};
    REQUIRE(eq.configure(nonfinite) == GraphicEqConfigureStatus::non_finite);
    REQUIRE(eq.configure(low_frequency) == GraphicEqConfigureStatus::frequency_out_of_range);
    REQUIRE(eq.configure(high_frequency) == GraphicEqConfigureStatus::frequency_out_of_range);
    REQUIRE(eq.configure(unordered) ==
            GraphicEqConfigureStatus::frequencies_not_strictly_increasing);
    REQUIRE(eq.configure(bad_gain) == GraphicEqConfigureStatus::gain_out_of_range);
    REQUIRE(eq.configure(bad_q) == GraphicEqConfigureStatus::q_out_of_range);

    REQUIRE(eq.configure(one_band) == GraphicEqConfigureStatus::configured);
    REQUIRE(eq.band_count() == 1);
    REQUIRE(eq.band(0) == one_band[0]);
    REQUIRE(eq.tail_samples() == -1);
    STATIC_REQUIRE(Eq::latency_samples() == 0);
}

TEST_CASE("GraphicEq rejects invalid replacement transactionally", "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<double, 4>;
    using Band = Eq::Band;
    constexpr std::array initial{Band{100.0, 6.0, 0.7}, Band{2000.0, -4.0, 2.0}};

    Eq candidate, reference;
    REQUIRE(candidate.prepare(48000.0, 4) == GraphicEqPrepareStatus::prepared);
    REQUIRE(reference.prepare(48000.0, 4) == GraphicEqPrepareStatus::prepared);
    REQUIRE(candidate.configure(initial) == GraphicEqConfigureStatus::configured);
    REQUIRE(reference.configure(initial) == GraphicEqConfigureStatus::configured);
    for (int sample = 0; sample < 512; ++sample) {
        const double input = std::sin(0.03 * sample);
        REQUIRE(candidate.process(input) == reference.process(input));
    }

    // Repeating the exact requested endpoint is a history-preserving no-op.
    REQUIRE(candidate.configure(initial, 100) == GraphicEqConfigureStatus::configured);
    for (int sample = 0; sample < 128; ++sample) {
        const double input = std::sin(0.11 * sample);
        REQUIRE(candidate.process(input) == reference.process(input));
    }

    const std::array invalid{Band{100.0, 6.0, 0.7}, Band{80.0, -4.0, 2.0}};
    REQUIRE(candidate.configure(invalid, 100) ==
            GraphicEqConfigureStatus::frequencies_not_strictly_increasing);
    REQUIRE(candidate.band_count() == initial.size());
    REQUIRE(candidate.band(1) == initial[1]);
    for (int sample = 0; sample < 512; ++sample) {
        const double input = std::cos(0.07 * sample);
        REQUIRE(candidate.process(input) == reference.process(input));
    }
}

TEST_CASE("GraphicEq neutral layout is an exact flat bypass", "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<double, 8>;
    using Band = Eq::Band;
    constexpr std::array neutral{Band{31.5, 0.0, 0.5}, Band{125.0, 0.0, 1.0},
                                 Band{1000.0, 0.0, 4.0}, Band{16000.0, 0.0, 12.0}};
    Eq eq;
    REQUIRE(eq.prepare(48000.0, 8) == GraphicEqPrepareStatus::prepared);
    REQUIRE(eq.configure(neutral) == GraphicEqConfigureStatus::configured);
    REQUIRE(eq.tail_samples() == 0);

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        const double input = 0.7 * std::sin(0.019 * sample) + 0.1 * std::cos(0.133 * sample);
        REQUIRE(eq.process(input) == input);
    }
    for (double frequency : {0.0, 20.0, 125.0, 1000.0, 10000.0, 24000.0}) {
        REQUIRE(eq.magnitude(frequency) == 1.0);
        REQUIRE(eq.magnitude_db(frequency) == 0.0);
    }
    REQUIRE(std::isnan(eq.magnitude(-1.0)));
    REQUIRE(std::isnan(eq.magnitude(24000.01)));
    REQUIRE(std::isnan(eq.magnitude(std::numeric_limits<double>::quiet_NaN())));

    REQUIRE(eq.configure(std::span<const Band>{}) == GraphicEqConfigureStatus::configured);
    REQUIRE(eq.band_count() == 0);
    REQUIRE(eq.process(0.25) == 0.25);
}

TEST_CASE("GraphicEq processing matches an independent long-double realization",
          "[signal][graphic-eq]") {
    require_processing_matches_independent_reference<float>();
    require_processing_matches_independent_reference<double>();
}

// Negative control for the tolerance above. The float bound is loose enough to
// absorb platform long-double width differences, so it must still be shown to
// REJECT a real defect rather than merely passing. Planting a one-cent gain
// error on a single band -- far smaller than any plausible coefficient bug --
// drives the divergence past the bound, so a green run above is evidence about
// the DSP and not about the tolerance.
TEST_CASE("GraphicEq reference tolerance rejects a planted gain error",
          "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<float, 8>;
    using Band = Eq::Band;
    constexpr std::array<Band, 4> bands{{
        {63.0f, 5.5f, 0.8f},
        {250.0f, -8.0f, 1.4f},
        {1000.0f, 3.0f, 2.2f},
        {8000.0f, -4.5f, 0.7f},
    }};
    // Same recipe, one band's gain perturbed by 0.01 dB.
    constexpr std::array<Band, 4> planted{{
        {63.0f, 5.5f, 0.8f},
        {250.0f, -8.01f, 1.4f},
        {1000.0f, 3.0f, 2.2f},
        {8000.0f, -4.5f, 0.7f},
    }};

    Eq eq;
    REQUIRE(eq.prepare(48000.0f, 8) == GraphicEqPrepareStatus::prepared);
    REQUIRE(eq.configure(planted) == GraphicEqConfigureStatus::configured);

    std::array<ReferenceSection, bands.size()> reference{};
    for (std::size_t index = 0; index < bands.size(); ++index)
        reference[index] = reference_peaking(bands[index], 48000.0L);

    double worst = 0.0;
    for (std::size_t sample = 0; sample < 4096; ++sample) {
        const float input = static_cast<float>(
            sample == 0 ? 1.0L
                        : 0.4L * std::sin(0.017L * static_cast<long double>(sample)) +
                              0.2L * std::cos(0.071L * static_cast<long double>(sample)));
        long double expected = static_cast<long double>(input);
        for (auto& section : reference)
            expected = section.process(expected);
        worst = std::max(worst,
                         std::abs(static_cast<double>(eq.process(input)) -
                                  static_cast<double>(expected)));
    }
    // The planted error must be visible at the bound the positive test uses.
    REQUIRE(worst > kFloatReferenceTolerance);
}

TEST_CASE("GraphicEq response matches an independent long-double oracle", "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<double, 8>;
    using Band = Eq::Band;
    constexpr std::array bands{Band{63.0, 7.0, 0.65}, Band{500.0, -9.0, 1.8},
                               Band{4000.0, 5.0, 3.0}, Band{16000.0, -3.0, 0.5}};
    Eq eq;
    REQUIRE(eq.prepare(48000.0, 8) == GraphicEqPrepareStatus::prepared);
    REQUIRE(eq.configure(bands) == GraphicEqConfigureStatus::configured);

    for (double frequency :
         {20.0, 31.5, 63.0, 100.0, 500.0, 1000.0, 4000.0, 10000.0, 16000.0, 20000.0, 24000.0}) {
        const double expected =
            static_cast<double>(reference_magnitude(bands, bands.size(), frequency, 48000.0L));
        REQUIRE_THAT(eq.magnitude(frequency), WithinAbs(expected, 2.5e-6));
        REQUIRE_THAT(eq.magnitude_db(frequency),
                     WithinAbs(20.0 * std::log10(eq.magnitude(frequency)), 2.0e-13));
    }
}

TEST_CASE("GraphicEq transition and in-place blocks are partition deterministic",
          "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<float, 8>;
    using Band = Eq::Band;
    constexpr std::array initial{Band{100.0f, 6.0f, 0.8f}, Band{2000.0f, -4.0f, 1.5f}};
    constexpr std::array target{Band{80.0f, -8.0f, 0.6f}, Band{1000.0f, 5.0f, 2.0f},
                                Band{12000.0f, 3.0f, 0.7f}};

    auto render = [&](std::span<const std::size_t> partitions) {
        Eq eq;
        REQUIRE(eq.prepare(48000.0f, 8) == GraphicEqPrepareStatus::prepared);
        REQUIRE(eq.configure(initial) == GraphicEqConfigureStatus::configured);
        for (int sample = 0; sample < 256; ++sample)
            static_cast<void>(eq.process(0.2f * std::sin(0.04f * sample)));
        REQUIRE(eq.configure(target, 257) == GraphicEqConfigureStatus::configured);
        REQUIRE(eq.configure(initial) == GraphicEqConfigureStatus::transition_in_progress);

        std::vector<float> output(2048);
        for (std::size_t sample = 0; sample < output.size(); ++sample)
            output[sample] = 0.4f * std::sin(0.017f * static_cast<float>(sample)) +
                             0.1f * std::cos(0.119f * static_cast<float>(sample));
        std::size_t cursor = 0;
        std::size_t partition = 0;
        while (cursor < output.size()) {
            const std::size_t frames =
                std::min(partitions[partition++ % partitions.size()], output.size() - cursor);
            REQUIRE(eq.process_block(output.data() + cursor, frames));
            cursor += frames;
        }
        REQUIRE_FALSE(eq.transitioning());
        REQUIRE(eq.band_count() == target.size());
        REQUIRE(eq.band(2) == target[2]);
        return output;
    };

    constexpr std::array<std::size_t, 1> one{1};
    constexpr std::array<std::size_t, 7> varied{64, 3, 127, 1, 255, 8, 17};
    REQUIRE(render(one) == render(varied));

    // A large old recursive tail must not contaminate the exact destination
    // on the final transition frame through subtractive interpolation loss.
    Eq endpoint;
    constexpr std::array hot{Band{1000.0f, 24.0f, 12.0f}};
    REQUIRE(endpoint.prepare(48000.0f, 8) == GraphicEqPrepareStatus::prepared);
    REQUIRE(endpoint.configure(hot) == GraphicEqConfigureStatus::configured);
    static_cast<void>(endpoint.process(1.0e30f));
    REQUIRE(endpoint.configure(std::span<const Band>{}, 2) ==
            GraphicEqConfigureStatus::configured);
    REQUIRE(std::isfinite(endpoint.process(0.25f)));
    REQUIRE(endpoint.process(0.25f) == 0.25f);
}

TEST_CASE("GraphicEq faults recover, reset selects the requested endpoint, and null blocks fail",
          "[signal][graphic-eq]") {
    using Eq = pulp::signal::GraphicEqT<double, 4>;
    using Band = Eq::Band;
    constexpr std::array initial{Band{500.0, 8.0, 1.0}};
    constexpr std::array target{Band{2000.0, -6.0, 2.0}};
    Eq eq;
    REQUIRE(eq.prepare(48000.0, 4) == GraphicEqPrepareStatus::prepared);
    REQUIRE(eq.configure(initial) == GraphicEqConfigureStatus::configured);
    REQUIRE(eq.configure(target, 128) == GraphicEqConfigureStatus::configured);
    static_cast<void>(eq.process(0.5));
    REQUIRE(eq.transitioning());

    REQUIRE(eq.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    REQUIRE_FALSE(eq.healthy());
    REQUIRE(eq.fault_count() == 1);
    REQUIRE(eq.transitioning());
    REQUIRE(std::isfinite(eq.process(0.25)));
    REQUIRE(eq.healthy());

    REQUIRE_FALSE(eq.process_block(nullptr, 1));
    REQUIRE(eq.process_block(nullptr, 0));
    eq.reset();
    REQUIRE_FALSE(eq.transitioning());
    REQUIRE(eq.band(0) == target[0]);
    REQUIRE(eq.tail_samples() == -1);
    REQUIRE(std::isfinite(eq.process(std::numeric_limits<double>::max())));
}

TEST_CASE("GraphicEq lifecycle configuration processing and reset allocate no memory",
          "[signal][graphic-eq][rt]") {
    using Eq = pulp::signal::GraphicEqT<float, 31>;
    using Band = Eq::Band;
    constexpr std::array bands{Band{31.5f, 4.0f, 0.7f},   Band{63.0f, -2.0f, 1.0f},
                               Band{125.0f, 3.0f, 1.4f},  Band{250.0f, -5.0f, 2.0f},
                               Band{500.0f, 2.0f, 0.8f},  Band{1000.0f, -3.0f, 1.2f},
                               Band{2000.0f, 5.0f, 2.5f}, Band{4000.0f, -1.0f, 0.5f},
                               Band{8000.0f, 2.0f, 0.9f}, Band{16000.0f, -4.0f, 0.7f}};
    Eq eq;
    GraphicEqPrepareStatus prepared = GraphicEqPrepareStatus::invalid_capacity;
    GraphicEqConfigureStatus configured = GraphicEqConfigureStatus::not_prepared;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        prepared = eq.prepare(48000.0f, 31);
        configured = eq.configure(bands, 128);
        for (int sample = 0; sample < 4096; ++sample)
            static_cast<void>(eq.process(sample == 0 ? 1.0f : 0.0f));
        static_cast<void>(eq.magnitude_db(1000.0));
        eq.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(prepared == GraphicEqPrepareStatus::prepared);
    REQUIRE(configured == GraphicEqConfigureStatus::configured);
    REQUIRE(allocations == 0);
}
