#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/six_band_eq.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::signal::SixBandEq;

namespace {

template <typename T>
std::array<T, 6> legacy_process(std::array<T, 6> samples,
                                const std::array<typename pulp::signal::SixBandEqT<T, 1>::Parameters,
                                                 6>& parameters,
                                T sample_rate) {
    using Filter = pulp::signal::BiquadT<T>;
    std::array<Filter, 6> filters{};
    for (std::size_t band = 0; band < filters.size(); ++band) {
        const auto& p = parameters[band];
        filters[band].set_coefficients(
            pulp::signal::SixBandEqT<T, 1>::band_type(band), p.frequency_hz, p.q,
            sample_rate, p.gain_db);
    }
    for (auto& sample : samples)
        for (auto& filter : filters) sample = filter.process(sample);
    return samples;
}

template <typename T>
void require_float_and_double_contract() {
    using Eq = pulp::signal::SixBandEqT<T, 1>;
    Eq eq;
    eq.prepare(T{48000});
    REQUIRE(eq.set_band(0, {T{120}, T{6}, T{0.8}}));
    REQUIRE(eq.set_band(2, {T{850}, T{-7}, T{2.5}}));
    REQUIRE(eq.set_band(5, {T{9000}, T{4}, T{0.7}}));

    std::array<T, 6> impulse{T{1}, T{0}, T{0}, T{0}, T{0}, T{0}};
    auto expected_parameters = std::array<typename Eq::Parameters, 6>{};
    for (std::size_t band = 0; band < expected_parameters.size(); ++band)
        expected_parameters[band] = eq.band(band);
    const auto expected = legacy_process<T>(impulse, expected_parameters, T{48000});
    REQUIRE(eq.process_block(impulse.data(), impulse.size()));
    for (std::size_t i = 0; i < impulse.size(); ++i)
        REQUIRE_THAT(static_cast<double>(impulse[i]),
                     WithinAbs(static_cast<double>(expected[i]),
                               std::is_same_v<T, float> ? 1e-7 : 1e-13));
}

double dft_magnitude(const std::array<double, 32768>& impulse, double frequency_hz,
                     double sample_rate) {
    std::complex<double> sum{};
    const double omega = 2.0 * std::acos(-1.0) * frequency_hz / sample_rate;
    for (std::size_t i = 0; i < impulse.size(); ++i)
        sum += impulse[i] * std::polar(1.0, -omega * static_cast<double>(i));
    return std::abs(sum);
}

} // namespace

TEST_CASE("SixBandEq publishes the established fixed roles and defaults", "[signal][eq]") {
    constexpr std::array<float, 6> frequencies{80, 250, 700, 2000, 5000, 12000};
    constexpr std::array<float, 6> qs{0.707f, 1.0f, 1.2f, 1.2f, 1.0f, 0.707f};
    for (std::size_t band = 0; band < SixBandEq::band_count; ++band) {
        const auto defaults = SixBandEq::default_band(band);
        REQUIRE(defaults.frequency_hz == frequencies[band]);
        REQUIRE(defaults.gain_db == 0.0f);
        REQUIRE(defaults.q == qs[band]);
        const auto expected_type = band == 0 ? SixBandEq::Type::low_shelf
                                   : band == 5 ? SixBandEq::Type::high_shelf
                                               : SixBandEq::Type::peaking;
        REQUIRE(SixBandEq::band_type(band) == expected_type);
    }
}

TEST_CASE("SixBandEq float and double processing preserve the legacy cascade", "[signal][eq]") {
    require_float_and_double_contract<float>();
    require_float_and_double_contract<double>();
}

TEST_CASE("SixBandEq clamps finite controls and replaces non-finite controls", "[signal][eq]") {
    SixBandEq eq;
    eq.prepare(32000.0f);
    REQUIRE(eq.set_band(1, {-5.0f, 40.0f, 0.0f}));
    REQUIRE(eq.band(1).frequency_hz == 20.0f);
    REQUIRE(eq.band(1).gain_db == 18.0f);
    REQUIRE(eq.band(1).q == 0.1f);

    REQUIRE(eq.set_band(5, {50000.0f, -40.0f, 100.0f}));
    REQUIRE(eq.band(5).frequency_hz == 32000.0f * 0.49f);
    REQUIRE(eq.band(5).gain_db == -18.0f);
    REQUIRE(eq.band(5).q == 12.0f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(eq.set_band(2, {nan, nan, nan}));
    const auto fallback = SixBandEq::default_band(2);
    REQUIRE(eq.band(2).frequency_hz == fallback.frequency_hz);
    REQUIRE(eq.band(2).gain_db == fallback.gain_db);
    REQUIRE(eq.band(2).q == fallback.q);

    const auto preserved = eq.band(2);
    REQUIRE_FALSE(eq.set_band(6, {400.0f, 3.0f, 2.0f}));
    REQUIRE(eq.band(2).frequency_hz == preserved.frequency_hz);
}

TEST_CASE("SixBandEq prepare fallback and rejected process calls fail safely", "[signal][eq]") {
    pulp::signal::SixBandEqT<float, 1> eq;
    eq.prepare(std::numeric_limits<float>::infinity());
    REQUIRE(eq.sample_rate() == 48000.0f);
    eq.prepare(20.0f);
    REQUIRE(eq.sample_rate() == 48000.0f);

    REQUIRE(eq.process(0.25f, 1) == 0.25f);
    REQUIRE_FALSE(eq.process_block(nullptr, 1));
    REQUIRE(eq.process_block(nullptr, 0));
}

TEST_CASE("SixBandEq automation preserves history and reset clears only history", "[signal][eq]") {
    pulp::signal::SixBandEqT<double, 1> dirty;
    dirty.prepare(48000.0);
    REQUIRE(dirty.set_band(2, {900.0, 12.0, 4.0}));
    static_cast<void>(dirty.process(1.0));
    for (int i = 0; i < 8; ++i) static_cast<void>(dirty.process(0.0));

    REQUIRE(dirty.set_band(2, {900.0, -12.0, 4.0}));
    const double history_tail = dirty.process(0.0);
    REQUIRE(std::abs(history_tail) > 1e-8);

    const auto effective = dirty.band(2);
    dirty.reset();
    REQUIRE(dirty.process(0.0) == 0.0);
    REQUIRE(dirty.band(2).frequency_hz == effective.frequency_hz);
    REQUIRE(dirty.band(2).gain_db == effective.gain_db);
    REQUIRE(dirty.band(2).q == effective.q);
}

TEST_CASE("SixBandEq transition crossfades stable cascades and reaches the exact endpoint",
          "[signal][eq][automation]") {
    pulp::signal::SixBandEqT<double, 1> warmed;
    warmed.prepare(48000.0);
    REQUIRE(warmed.set_band(2, {900.0, 12.0, 5.0}));
    static_cast<void>(warmed.process(1.0));
    for (int i = 0; i < 24; ++i) static_cast<void>(warmed.process(0.0));

    auto old_continuation = warmed;
    auto immediate = warmed;
    auto transitioned = warmed;
    immediate.set_band(2, {900.0, -18.0, 5.0});
    transitioned.set_transition_samples(64);
    transitioned.set_band(2, {900.0, -18.0, 5.0});

    constexpr double automation_sample = 0.25;
    const double old_first = old_continuation.process(automation_sample);
    const double immediate_first = immediate.process(automation_sample);
    const double transitioned_first = transitioned.process(automation_sample);
    REQUIRE(std::abs(transitioned_first - old_first) <
            std::abs(immediate_first - old_first));
    for (int i = 1; i < 64; ++i) {
        static_cast<void>(immediate.process(0.0));
        static_cast<void>(transitioned.process(0.0));
    }
    REQUIRE_FALSE(transitioned.transition_active());
    REQUIRE(transitioned.process(0.0) == immediate.process(0.0));
}

TEST_CASE("SixBandEq transition queues block changes without interpolating coefficients",
          "[signal][eq][automation]") {
    pulp::signal::SixBandEqT<float, 2> eq;
    eq.prepare(48000.0f);
    eq.set_transition_samples(4);
    REQUIRE(eq.set_band(0, {100.0f, 6.0f, 0.7f}));
    REQUIRE(eq.set_band(5, {10000.0f, -6.0f, 0.7f}));
    REQUIRE(eq.transition_active(0));
    REQUIRE(eq.transition_active(1));
    REQUIRE_FALSE(eq.transition_active(2));
    REQUIRE_FALSE(eq.set_band(6, {1000.0f, 3.0f, 1.0f}));

    auto rejected_process_reference = eq;
    REQUIRE_FALSE(eq.process_block(nullptr, 1, 0));
    REQUIRE(eq.process(0.25f, 0) == rejected_process_reference.process(0.25f, 0));

    static_cast<void>(eq.process(0.0f, 0));
    REQUIRE(eq.set_band(2, {800.0f, 12.0f, 3.0f}));
    for (int i = 0; i < 6; ++i) static_cast<void>(eq.process(0.0f, 0));
    REQUIRE_FALSE(eq.transition_active(0));
    REQUIRE(eq.transition_active(1));
    REQUIRE(eq.band(2).gain_db == 12.0f);
    for (int i = 0; i < 4; ++i) static_cast<void>(eq.process(0.0f, 1));
    REQUIRE_FALSE(eq.transition_active());

    REQUIRE(eq.set_band(3, {1800.0f, -12.0f, 2.0f}));
    REQUIRE(eq.transition_active());
    auto unchanged = eq;
    eq.set_transition_samples(4);
    REQUIRE(eq.process(0.125f, 0) == unchanged.process(0.125f, 0));
    eq.set_transition_samples(0);
    REQUIRE(eq.transition_samples() == 0);
    REQUIRE(eq.transition_active());
    REQUIRE(eq.process(0.0f, 0) == unchanged.process(0.0f, 0));
}

TEST_CASE("SixBandEq transition length changes preserve warmed staggered channel state",
          "[signal][eq][automation]") {
    using Eq = pulp::signal::SixBandEqT<double, 2>;
    Eq changed;
    changed.prepare(48000.0);
    REQUIRE(changed.set_band(2, {900.0, 14.0, 6.0}));
    for (int i = 0; i < 48; ++i) {
        static_cast<void>(changed.process(i == 0 ? 0.05 : 0.0, 0));
        static_cast<void>(changed.process(i == 0 ? 0.05 : 0.0, 1));
    }
    changed.set_transition_samples(64);
    REQUIRE(changed.set_band(2, {900.0, -18.0, 6.0}));
    for (int i = 0; i < 19; ++i) static_cast<void>(changed.process(0.0, 0));
    for (int i = 0; i < 7; ++i) static_cast<void>(changed.process(0.0, 1));

    auto reference = changed;
    changed.set_transition_samples(64);
    REQUIRE(changed.process(0.05, 0) == reference.process(0.05, 0));
    changed.set_transition_samples(23);
    REQUIRE(changed.process(-0.05, 1) == reference.process(-0.05, 1));
    changed.set_transition_samples(0);
    REQUIRE(changed.transition_samples() == 0);
    for (int i = 0; i < 70; ++i) {
        REQUIRE(changed.process(i == 0 ? 0.05 : 0.0, 0) ==
                reference.process(i == 0 ? 0.05 : 0.0, 0));
        REQUIRE(changed.process(i == 0 ? -0.05 : 0.0, 1) ==
                reference.process(i == 0 ? -0.05 : 0.0, 1));
    }
    REQUIRE_FALSE(changed.transition_active());
}

TEST_CASE("SixBandEq immediate mode waits for a bounded queued continuation",
          "[signal][eq][automation]") {
    using Eq = pulp::signal::SixBandEqT<double, 1>;
    Eq changed;
    changed.prepare(48000.0);
    changed.set_transition_samples(32);
    REQUIRE(changed.set_band(1, {300.0, 18.0, 7.0}));
    for (int i = 0; i < 11; ++i)
        static_cast<void>(changed.process(i == 0 ? 0.05 : 0.0));

    auto smooth_reference = changed;
    changed.set_transition_samples(0);
    REQUIRE(changed.set_band(4, {6000.0, -18.0, 8.0}));
    REQUIRE(smooth_reference.set_band(4, {6000.0, -18.0, 8.0}));
    for (int i = 0; i < 64; ++i)
        REQUIRE(changed.process(i == 0 ? 0.05 : 0.0) ==
                smooth_reference.process(i == 0 ? 0.05 : 0.0));
    REQUIRE_FALSE(changed.transition_active());
    REQUIRE(changed.transition_samples() == 0);

    REQUIRE(changed.set_band(4, {6000.0, 6.0, 8.0}));
    REQUIRE_FALSE(changed.transition_active());

    changed.set_transition_samples(7);
    REQUIRE(changed.set_band(4, {6000.0, -6.0, 8.0}));
    for (int i = 0; i < 6; ++i) static_cast<void>(changed.process(0.0));
    REQUIRE(changed.transition_active());
    static_cast<void>(changed.process(0.0));
    REQUIRE_FALSE(changed.transition_active());
}

TEST_CASE("SixBandEq identical sanitized controls are exact state no-ops",
          "[signal][eq][automation]") {
    using Eq = pulp::signal::SixBandEqT<float, 1>;
    Eq repeated;
    repeated.prepare(48000.0f);
    repeated.set_transition_samples(256);
    const Eq::Parameters target{850.0f, 12.0f, 3.0f};
    REQUIRE(repeated.set_band(2, target));
    std::array<Eq::Parameters, Eq::band_count> controls{};
    for (std::size_t band = 0; band < controls.size(); ++band)
        controls[band] = repeated.band(band);
    auto reference = repeated;

    for (int frame = 0; frame < 6400; ++frame) {
        if (frame % 64 == 0) {
            REQUIRE(repeated.set_band(2, target));
            repeated.set_bands(controls);
        }
        const float input = frame % 113 == 0 ? 0.05f : 0.0f;
        REQUIRE(repeated.process(input) == reference.process(input));
    }
    REQUIRE_FALSE(repeated.transition_active());

    REQUIRE(repeated.set_band(0, {-1000.0f, 0.0f, 0.707f}));
    for (int i = 0; i < 256; ++i) static_cast<void>(repeated.process(0.0f));
    REQUIRE_FALSE(repeated.transition_active());
    auto clamped_reference = repeated;
    REQUIRE(repeated.set_band(0, {-1.0f, 0.0f, 0.707f}));
    REQUIRE_FALSE(repeated.transition_active());
    REQUIRE(repeated.process(0.05f) == clamped_reference.process(0.05f));

    REQUIRE(repeated.set_band(0, {20.0f, 0.0f, 0.8f}));
    REQUIRE(repeated.transition_active());
    for (int i = 0; i < 256; ++i) static_cast<void>(repeated.process(0.0f));
    REQUIRE_FALSE(repeated.transition_active());
    for (std::size_t band = 0; band < controls.size(); ++band)
        controls[band] = repeated.band(band);
    controls[5].gain_db = 1.0f;
    repeated.set_bands(controls);
    REQUIRE(repeated.transition_active());
}

TEST_CASE("SixBandEq set_bands transition is deterministic across reset and extremes",
          "[signal][eq][automation]") {
    using Eq = pulp::signal::SixBandEqT<double, 1>;
    Eq a;
    a.prepare(41.0);
    a.set_transition_samples(17);
    std::array<Eq::Parameters, Eq::band_count> controls{};
    for (std::size_t band = 0; band < controls.size(); ++band)
        controls[band] = {band & 1 ? 1e12 : -1e12, band & 1 ? 1e12 : -1e12,
                          band & 1 ? 1e12 : -1e12};
    a.set_bands(controls);
    auto b = a;
    for (int i = 0; i < 8; ++i) {
        REQUIRE(std::isfinite(a.process(i == 0 ? 1.0 : 0.0)));
        REQUIRE(std::isfinite(b.process(i == 0 ? 1.0 : 0.0)));
    }
    a.reset();
    b.reset();
    for (int i = 0; i < 32; ++i)
        REQUIRE(a.process(i == 0 ? 1.0 : 0.0) == b.process(i == 0 ? 1.0 : 0.0));
    REQUIRE_FALSE(a.transition_active());
}

TEST_CASE("SixBandEq response reports the active cascade measured by an impulse DFT",
          "[signal][eq][response]") {
    pulp::signal::SixBandEqT<double, 1> response_eq;
    response_eq.prepare(48000.0);
    REQUIRE(response_eq.set_band(0, {100.0, 5.0, 0.707}));
    REQUIRE(response_eq.set_band(1, {300.0, -8.0, 4.0}));
    REQUIRE(response_eq.set_band(2, {800.0, 3.0, 1.5}));
    REQUIRE(response_eq.set_band(3, {2200.0, 7.0, 2.5}));
    REQUIRE(response_eq.set_band(4, {5500.0, -4.0, 2.0}));
    REQUIRE(response_eq.set_band(5, {11000.0, 6.0, 0.707}));

    auto process_eq = response_eq;
    std::array<double, 32768> impulse{};
    impulse[0] = 1.0;
    REQUIRE(process_eq.process_block(impulse.data(), impulse.size()));

    for (double frequency : {40.0, 300.0, 2200.0, 10000.0, 20000.0}) {
        const double measured = dft_magnitude(impulse, frequency, 48000.0);
        REQUIRE_THAT(response_eq.magnitude(frequency), WithinAbs(measured, 2e-5));
        REQUIRE_THAT(response_eq.magnitude_db(frequency),
                     WithinAbs(20.0 * std::log10(measured), 2e-4));
    }

    std::array<double, 5> curve{};
    response_eq.response_curve_db(40.0, 20000.0, curve);
    REQUIRE_THAT(curve.front(), WithinAbs(response_eq.magnitude_db(40.0), 1e-12));
    REQUIRE_THAT(curve.back(), WithinAbs(response_eq.magnitude_db(20000.0), 1e-12));
}

TEST_CASE("SixBandEq control process response and reset paths allocate nothing", "[signal][eq][rt-safety]") {
    SixBandEq eq;
    eq.prepare(48000.0f);
    std::array<float, 64> block{};
    std::array<float, 32> curve{};
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        eq.set_transition_samples(32);
        REQUIRE(eq.set_band(3, {1800.0f, 6.0f, 2.0f}));
        REQUIRE(eq.process_block(block.data(), block.size(), 0));
        eq.response_curve_db(20.0, 20000.0, curve);
        eq.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}
