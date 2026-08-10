#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/pitch_track.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/reed_waveguide_loop.hpp>
#include <pulp/signal/waveguide_reed_exciter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

double reed_oracle(double mouth_pressure, double bore_incident, double closing_pressure,
                   double flow_gain, double bore_impedance) {
    const auto mouth = std::clamp(mouth_pressure, 0.0, 1.0);
    const auto pressure_difference = mouth - bore_incident;
    const auto opening = std::clamp(1.0 - pressure_difference / closing_pressure, 0.0, 1.0);
    const auto signed_root = pressure_difference < 0.0 ? -std::sqrt(-pressure_difference)
                                                       : std::sqrt(pressure_difference);
    return bore_incident - bore_impedance * flow_gain * opening * signed_root;
}

int expected_linear_phase_latency(double sample_rate, int factor) {
    if (factor == 1)
        return 0;
    pulp::signal::Oversampler64 oversampler;
    oversampler.set_factor(factor == 2 ? pulp::signal::Oversampler64::Factor::x2
                                       : pulp::signal::Oversampler64::Factor::x4);
    oversampler.set_sample_rate(sample_rate);
    oversampler.set_quality(pulp::signal::Oversampler64::Quality::standard);
    oversampler.set_kind(pulp::signal::Oversampler64::Kind::linear_phase_fir);
    return oversampler.latency_samples();
}

std::array<double, 3> retune_positions_after(std::size_t base_rate_samples) {
    constexpr double sample_rate = 48000.0;
    constexpr double initial_seconds = 12.0 / sample_rate;
    constexpr double target_seconds = 72.0 / sample_rate;
    std::array<double, 3> positions{};
    std::size_t index = 0;
    for (const int factor : {1, 2, 4}) {
        pulp::signal::ReedWaveguideLoop64 loop;
        loop.set_one_way_seconds(initial_seconds);
        REQUIRE(loop.prepare(sample_rate, 0.01, factor));
        loop.set_one_way_seconds(target_seconds);
        for (std::size_t sample = 0; sample < base_rate_samples; ++sample)
            (void)loop.process(0.0);
        positions[index++] = loop.current_one_way_seconds();
    }
    return positions;
}

double projected_amplitude(const std::array<double, 24000>& signal, double frequency,
                           double sample_rate) {
    constexpr double two_pi = 6.283185307179586476925286766559;
    double sine = 0.0;
    double cosine = 0.0;
    for (std::size_t sample = 0; sample < signal.size(); ++sample) {
        const auto phase = two_pi * frequency * static_cast<double>(sample) / sample_rate;
        sine += signal[sample] * std::sin(phase);
        cosine += signal[sample] * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine) / static_cast<double>(signal.size());
}

struct ReedAliasLevels {
    double carrier = 0.0;
    double third_order_dbc = 0.0;
    double fifth_order_dbc = 0.0;
};

ReedAliasLevels reed_alias_levels(int factor) {
    constexpr double sample_rate = 48000.0;
    constexpr double carrier_hz = 17000.0;
    constexpr std::size_t discard = 12000;
    constexpr double two_pi = 6.283185307179586476925286766559;
    pulp::signal::ReedWaveguideLoop64 loop;
    loop.set_one_way_seconds(20.5 / sample_rate);
    REQUIRE(loop.prepare(sample_rate, 0.02, factor));
    loop.set_closing_pressure(0.35);
    loop.set_flow_gain(1.0);
    loop.set_bore_impedance(1.0);
    loop.set_bell_reflection_gain(0.0);
    loop.set_bell_loss_pole(0.0);

    std::array<double, 24000> output{};
    for (std::size_t sample = 0; sample < discard + output.size(); ++sample) {
        const auto mouth = 0.32 + 0.25 * std::sin(two_pi * carrier_hz * sample / sample_rate);
        const auto rendered = loop.process(mouth);
        if (sample >= discard)
            output[sample - discard] = rendered;
    }

    const auto carrier = projected_amplitude(output, carrier_hz, sample_rate);
    const auto dbc = [carrier](double amplitude) { return 20.0 * std::log10(amplitude / carrier); };
    return {carrier, dbc(projected_amplitude(output, 3000.0, sample_rate)),
            dbc(projected_amplitude(output, 11000.0, sample_rate))};
}

double tuned_one_way_seconds(double sample_rate, int factor, double target_hz,
                             double base_rate_bell_pole) {
    const auto internal_rate = sample_rate * static_cast<double>(factor);
    const auto internal_pole = std::pow(base_rate_bell_pole, 1.0 / factor);
    const auto angular_frequency = 2.0 * std::numbers::pi * target_hz / internal_rate;
    const auto bell_phase_delay = std::atan2(internal_pole * std::sin(angular_frequency),
                                             1.0 - internal_pole * std::cos(angular_frequency)) /
                                  angular_frequency;
    const auto one_way_samples = internal_rate / (4.0 * target_hz) - 0.5 * bell_phase_delay;
    return one_way_samples / internal_rate;
}

std::vector<float> render(pulp::signal::ReedWaveguideLoop64& loop, std::size_t frames,
                          double mouth_pressure) {
    std::vector<float> output(frames);
    for (auto& sample : output)
        sample = static_cast<float>(loop.process(mouth_pressure));
    return output;
}

} // namespace

TEST_CASE("ReedExciter matches the explicit bounded scalar equation",
          "[signal][waveguide][reed][oracle]") {
    constexpr double closing_pressure = 0.4;
    constexpr double flow_gain = 1.7;
    constexpr double bore_impedance = 0.8;
    pulp::signal::ReedExciter64 reed;
    reed.set_closing_pressure(closing_pressure);
    reed.set_flow_gain(flow_gain);
    reed.set_bore_impedance(bore_impedance);

    constexpr std::array cases{
        std::array{0.30, 0.10}, // partly open, positive flow
        std::array{0.10, 0.40}, // fully open, reverse flow
        std::array{0.90, 0.10}, // reed closed
        std::array{2.00, 0.85}, // mouth-pressure clamp at one
        std::array{-1.0, 0.20}, // mouth-pressure clamp at zero
    };
    for (const auto& values : cases) {
        const auto expected =
            reed_oracle(values[0], values[1], closing_pressure, flow_gain, bore_impedance);
        CHECK_THAT(reed.process(values[0], values[1]), WithinAbs(expected, 2.0e-15));
    }
}

TEST_CASE("ReedExciter has independent valve landmarks and the specified wave sign",
          "[signal][waveguide][reed][oracle][landmarks]") {
    pulp::signal::ReedExciter64 reed;
    reed.set_closing_pressure(0.35);
    reed.set_flow_gain(1.0);
    reed.set_bore_impedance(1.0);

    // Equal pressures produce no flow, while the exact closure point returns
    // the incident wave unchanged.
    CHECK(reed.process(0.2, 0.2) == 0.2);
    CHECK(reed.process(0.55, 0.2) == 0.2);

    // The explicit contract uses p_reflected = p_incident - Z*u. At the
    // analytic forward-flow maximum, dp=p_close/3 and q=2/3.
    constexpr double incident = 0.1;
    constexpr double pressure_difference = 0.35 / 3.0;
    const double maximum_flow = (2.0 / 3.0) * std::sqrt(pressure_difference);
    CHECK_THAT(reed.process(incident + pressure_difference, incident),
               WithinAbs(incident - maximum_flow, 2.0e-15));

    // Reverse pressure opens the valve fully: sqrt(.25)=.5, so the specified
    // minus sign turns the negative flow into a +.5 reflected contribution.
    CHECK_THAT(reed.process(0.20, 0.45), WithinAbs(0.95, 2.0e-15));
}

TEST_CASE("ReedExciter clamps controls and preserves finite fallback state",
          "[signal][waveguide][reed][fault]") {
    pulp::signal::ReedExciter reed;
    reed.set_closing_pressure(-1.0f);
    reed.set_flow_gain(100.0f);
    reed.set_bore_impedance(0.0f);
    CHECK(reed.closing_pressure() == pulp::signal::ReedExciter::minimum_closing_pressure);
    CHECK(reed.flow_gain() == pulp::signal::ReedExciter::maximum_flow_gain);
    CHECK(reed.bore_impedance() == pulp::signal::ReedExciter::minimum_bore_impedance);

    reed.set_closing_pressure(std::numeric_limits<float>::infinity());
    reed.set_flow_gain(std::numeric_limits<float>::quiet_NaN());
    reed.set_bore_impedance(-std::numeric_limits<float>::infinity());
    CHECK(reed.closing_pressure() == pulp::signal::ReedExciter::minimum_closing_pressure);
    CHECK(reed.flow_gain() == pulp::signal::ReedExciter::maximum_flow_gain);
    CHECK(reed.bore_impedance() == pulp::signal::ReedExciter::minimum_bore_impedance);

    const auto finite = reed.process(0.2f, 0.1f);
    CHECK(std::isfinite(finite));
    CHECK(reed.process(std::numeric_limits<float>::quiet_NaN(), 0.1f) == finite);
    CHECK(reed.process(0.2f, std::numeric_limits<float>::infinity()) == finite);
    reed.set_bore_impedance(1000.0f);
    CHECK(reed.bore_impedance() == 1000.0f);
    reed.reset();
    CHECK(reed.process(std::numeric_limits<float>::quiet_NaN(), 0.0f) == 0.0f);

    pulp::signal::ReedExciterT<long double> wide_reed;
    wide_reed.set_bore_impedance(std::numeric_limits<long double>::max());
    CHECK(wide_reed.bore_impedance() == std::numeric_limits<long double>::max());
}

TEST_CASE("ReedWaveguideLoop tunes across the admitted rate and pitch matrix",
          "[signal][waveguide][reed-loop][tuning]") {
    constexpr int factor = 2;
    constexpr double bell_pole = 0.20;
    for (const auto sample_rate : {44100.0, 48000.0, 96000.0}) {
        for (const auto target_hz : {100.0, 440.0, 2000.0}) {
            pulp::signal::ReedWaveguideLoop64 loop;
            loop.set_one_way_seconds(
                tuned_one_way_seconds(sample_rate, factor, target_hz, bell_pole));
            REQUIRE(loop.prepare(sample_rate, 0.01, factor));
            loop.set_closing_pressure(0.35);
            loop.set_flow_gain(1.0);
            loop.set_bore_impedance(1.0);
            loop.set_bell_reflection_gain(-0.995);
            loop.set_bell_loss_pole(bell_pole);

            (void)render(loop, static_cast<std::size_t>(sample_rate * 0.5), 0.20);
            const auto measured = render(loop, 32768, 0.20);
            pulp::test::audio::PitchOptions options;
            options.min_hz = target_hz * 0.7;
            options.max_hz = target_hz * 1.3;
            options.min_confidence = 0.3;
            const auto estimate = pulp::test::audio::estimate_pitch(
                std::span<const float>(measured), sample_rate, options);
            INFO("sample_rate=" << sample_rate << " target_hz=" << target_hz << " confidence="
                                << estimate.confidence << " measured_hz=" << estimate.hz);
            REQUIRE(estimate.voiced);
            CHECK(std::abs(pulp::test::audio::cents_between(estimate.hz, target_hz)) <= 10.0);
        }
    }
}

TEST_CASE("ReedWaveguideLoop validates preparation, factors, tuning bounds, and latency",
          "[signal][waveguide][reed-loop][lifecycle]") {
    constexpr double sample_rate = 48000.0;
    pulp::signal::ReedWaveguideLoop64 invalid;
    CHECK_FALSE(invalid.prepare(0.0, 0.01, 2));
    CHECK_FALSE(invalid.prepare(std::numeric_limits<double>::quiet_NaN(), 0.01, 2));
    CHECK_FALSE(invalid.prepare(sample_rate, 0.0, 2));
    CHECK_FALSE(invalid.prepare(sample_rate, 0.01, 3));
    // ceil(2100 * .001) lets the primitive allocate three samples, but the
    // physical maximum is still shorter than its three-sample minimum.
    CHECK_FALSE(invalid.prepare(2100.0, 0.001, 1));
    CHECK_FALSE(invalid.prepared());
    CHECK(invalid.process(0.5) == 0.0);

    for (const int factor : {1, 2, 4}) {
        pulp::signal::ReedWaveguideLoop64 loop;
        REQUIRE(loop.prepare(sample_rate, 0.01, factor));
        CHECK(loop.prepared());
        CHECK(loop.oversampling_factor() == factor);
        CHECK(loop.latency_samples() == expected_linear_phase_latency(sample_rate, factor));
        CHECK(loop.tail_samples() == -1);

        loop.set_bell_loss_pole(0.98);
        CHECK_THAT(loop.bell_loss_pole(), WithinAbs(0.98, 1.0e-15));
        CHECK_THAT(loop.internal_bell_loss_pole(),
                   WithinAbs(std::pow(0.98, 1.0 / factor), 2.0e-15));
        loop.set_bell_loss_pole(std::numeric_limits<double>::quiet_NaN());
        CHECK_THAT(loop.bell_loss_pole(), WithinAbs(0.98, 1.0e-15));

        loop.set_one_way_seconds(-1.0);
        CHECK_THAT(loop.target_one_way_seconds(), WithinAbs(3.0 / (sample_rate * factor), 1.0e-15));
        loop.set_one_way_seconds(1.0);
        CHECK_THAT(loop.target_one_way_seconds(), WithinAbs(0.01, 1.0e-15));
        loop.set_one_way_seconds(std::numeric_limits<double>::quiet_NaN());
        CHECK_THAT(loop.target_one_way_seconds(), WithinAbs(0.01, 1.0e-15));

        CHECK_FALSE(loop.prepare(sample_rate, 0.01, 8));
        CHECK(loop.prepared());
        CHECK(loop.oversampling_factor() == factor);
    }
}

TEST_CASE("ReedWaveguideLoop factor-one output arrives after the physical one-way delay",
          "[signal][waveguide][reed-loop][timing]") {
    constexpr double sample_rate = 48000.0;
    constexpr int one_way_samples = 8;
    pulp::signal::ReedWaveguideLoop64 loop;
    loop.set_one_way_seconds(one_way_samples / sample_rate);
    REQUIRE(loop.prepare(sample_rate, 0.01, 1));
    loop.set_closing_pressure(1.0);
    loop.set_flow_gain(1.0);
    loop.set_bore_impedance(1.0);
    loop.set_bell_loss_pole(0.0);

    std::array<double, 12> output{};
    for (std::size_t sample = 0; sample < output.size(); ++sample)
        output[sample] = loop.process(sample == 0 ? 0.25 : 0.0);

    for (int sample = 0; sample < one_way_samples; ++sample)
        CHECK(output[static_cast<std::size_t>(sample)] == 0.0);
    const auto expected_excitation = reed_oracle(0.25, 0.0, 1.0, 1.0, 1.0);
    CHECK_THAT(output[one_way_samples], WithinAbs(expected_excitation, 2.0e-15));
    CHECK_THAT(loop.current_one_way_seconds(), WithinAbs(one_way_samples / sample_rate, 1.0e-15));
    CHECK_THAT(loop.round_trip_seconds(), WithinAbs(2.0 * one_way_samples / sample_rate, 1.0e-15));
}

TEST_CASE("ReedWaveguideLoop retuning takes the same physical time at every factor",
          "[signal][waveguide][reed-loop][retune]") {
    constexpr double sample_rate = 48000.0;
    constexpr double initial_seconds = 12.0 / sample_rate;
    constexpr double target_seconds = 72.0 / sample_rate;
    constexpr std::size_t half_ramp_samples = 120; // half of the line's 5 ms glide

    const auto halfway = retune_positions_after(half_ramp_samples);
    const auto expected_halfway = 0.5 * (initial_seconds + target_seconds);
    for (const auto position : halfway)
        CHECK_THAT(position, WithinAbs(expected_halfway, 2.0e-14));
    CHECK_THAT(halfway[0], WithinAbs(halfway[1], 2.0e-14));
    CHECK_THAT(halfway[0], WithinAbs(halfway[2], 2.0e-14));

    const auto complete = retune_positions_after(240);
    for (const auto position : complete)
        CHECK_THAT(position, WithinAbs(target_seconds, 2.0e-14));
}

TEST_CASE("ReedWaveguideLoop removes the nonlinear folds admitted by each OS stage",
          "[signal][waveguide][reed-loop][aliasing]") {
    // 3*17 kHz folds to 3 kHz at 1x; 2x moves that harmonic below its internal
    // Nyquist. 5*17 kHz folds to 11 kHz through 2x; 4x admits and filters it.
    // Coherent line projections avoid classifying legitimate high-band energy.
    const auto one = reed_alias_levels(1);
    const auto two = reed_alias_levels(2);
    const auto four = reed_alias_levels(4);
    INFO("third-order dBc: " << one.third_order_dbc << ", " << two.third_order_dbc << ", "
                             << four.third_order_dbc);
    INFO("fifth-order dBc: " << one.fifth_order_dbc << ", " << two.fifth_order_dbc << ", "
                             << four.fifth_order_dbc);
    CHECK(one.carrier > 0.05);
    CHECK(two.carrier > 0.05);
    CHECK(four.carrier > 0.05);
    CHECK(one.third_order_dbc > -15.0);
    CHECK(two.third_order_dbc < -55.0);
    CHECK(four.third_order_dbc < -55.0);
    CHECK(two.third_order_dbc < one.third_order_dbc - 45.0);
    CHECK(two.fifth_order_dbc > -30.0);
    CHECK(four.fifth_order_dbc < -70.0);
    CHECK(four.fifth_order_dbc < two.fifth_order_dbc - 45.0);
}

TEST_CASE("ReedWaveguideLoop contains non-finite input and reset replays deterministically",
          "[signal][waveguide][reed-loop][fault][reset]") {
    constexpr double sample_rate = 48000.0;
    pulp::signal::ReedWaveguideLoop64 loop;
    loop.set_one_way_seconds(16.5 / sample_rate);
    REQUIRE(loop.prepare(sample_rate, 0.01, 2));
    loop.set_bell_reflection_gain(-0.7);
    loop.set_bell_loss_pole(0.15);

    const auto render = [&] {
        std::array<double, 768> result{};
        loop.reset();
        for (std::size_t sample = 0; sample < result.size(); ++sample) {
            const auto mouth = sample < 192 ? 0.22 + 0.03 * std::sin(0.031 * sample) : 0.0;
            result[sample] = loop.process(mouth);
        }
        return result;
    };
    const auto first = render();
    const auto second = render();
    CHECK(first == second);
    CHECK(
        std::all_of(first.begin(), first.end(), [](double value) { return std::isfinite(value); }));
    CHECK(std::any_of(first.begin(), first.end(), [](double value) { return value != 0.0; }));

    pulp::signal::ReedWaveguideLoop64 reference;
    reference.set_one_way_seconds(16.5 / sample_rate);
    REQUIRE(reference.prepare(sample_rate, 0.01, 2));
    reference.set_bell_reflection_gain(-0.7);
    reference.set_bell_loss_pole(0.15);
    loop.reset();
    reference.reset();
    for (int sample = 0; sample < 128; ++sample)
        CHECK(loop.process(0.2) == reference.process(0.2));
    CHECK(loop.process(std::numeric_limits<double>::quiet_NaN()) == reference.process(0.2));
    for (int sample = 0; sample < 128; ++sample)
        CHECK(loop.process(0.2) == reference.process(0.2));
}

TEST_CASE("ReedWaveguideLoop remains bounded for a five-second sustained drive",
          "[signal][waveguide][reed-loop][stability]") {
    constexpr double sample_rate = 48000.0;
    pulp::signal::ReedWaveguideLoop64 loop;
    loop.set_one_way_seconds(37.5 / sample_rate);
    REQUIRE(loop.prepare(sample_rate, 0.02, 4));
    loop.set_bell_reflection_gain(-0.7);
    loop.set_bell_loss_pole(0.15);

    double peak = 0.0;
    bool all_finite = true;
    for (int sample = 0; sample < 5 * static_cast<int>(sample_rate); ++sample) {
        const auto output = loop.process(0.65);
        all_finite = all_finite && std::isfinite(output);
        if (std::isfinite(output))
            peak = std::max(peak, std::abs(output));
    }

    // With Z=flow_gain=1 and |g|=.7, the passive-return recurrence is bounded
    // by x <= .7*(x + sqrt(1+x)); its positive fixed point is below 8.
    CHECK(all_finite);
    CHECK(peak > 0.0);
    CHECK(peak < 8.0);
}

TEST_CASE("ReedWaveguideLoop prepared audio and control paths allocate nothing",
          "[signal][waveguide][reed-loop][rt]") {
    pulp::signal::ReedWaveguideLoop loop;
    loop.set_one_way_seconds(12.0 / 48000.0);
    REQUIRE(loop.prepare(48000.0, 0.02, 4));

    bool all_finite = true;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int sample = 0; sample < 1024; ++sample) {
            if ((sample % 127) == 0)
                loop.set_one_way_seconds((12.0 + static_cast<double>(sample % 31)) / 48000.0);
            loop.set_closing_pressure(0.2f + 0.001f * static_cast<float>(sample % 200));
            loop.set_flow_gain(0.5f + 0.01f * static_cast<float>(sample % 100));
            loop.set_bore_impedance(0.75f);
            loop.set_bell_reflection_gain(-0.8f);
            loop.set_bell_loss_pole(0.25f);
            const auto mouth = sample == 511 ? std::numeric_limits<float>::quiet_NaN() : 0.2f;
            all_finite = all_finite && std::isfinite(loop.process(mouth));
        }
        loop.reset();
        allocations = probe.allocation_count();
    }
    CHECK(all_finite);
    CHECK(allocations == 0);
}
