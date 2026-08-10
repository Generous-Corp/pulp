#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/pitch_track.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/waveguide_line.hpp>
#include <pulp/signal/waveguide_reed_exciter.hpp>
#include <pulp/signal/waveguide_reflection_filter.hpp>

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

double reed_reference(double bore_incident, double mouth_pressure, double closing_pressure,
                      double flow_gain, double bore_impedance) {
    const auto pressure_difference = mouth_pressure - bore_incident;
    const auto opening = std::clamp(1.0 - pressure_difference / closing_pressure, 0.0, 1.0);
    const auto flow = std::copysign(flow_gain * opening * std::sqrt(std::abs(pressure_difference)),
                                    pressure_difference);
    return bore_incident - bore_impedance * flow;
}

class OversampledReedBore {
  public:
    using Oversampler = pulp::signal::OversamplerT<double>;

    bool prepare(double sample_rate, int factor, double target_hz) {
        if (!std::isfinite(sample_rate) || sample_rate < 8000.0 ||
            (factor != 1 && factor != 2 && factor != 4) || !std::isfinite(target_hz) ||
            target_hz <= 0.0)
            return false;

        factor_ = factor;
        const auto loop_rate = sample_rate * static_cast<double>(factor);
        constexpr auto open_loss_pole = 0.20;
        const auto angular_frequency = 2.0 * std::numbers::pi * target_hz / loop_rate;
        const auto open_filter_phase_delay =
            std::atan2(open_loss_pole * std::sin(angular_frequency),
                       1.0 - open_loss_pole * std::cos(angular_frequency)) /
            angular_frequency;
        const auto one_way_length = loop_rate / (4.0 * target_hz) - 0.5 * open_filter_phase_delay;
        const auto max_seconds =
            std::clamp(std::ceil(one_way_length) / loop_rate + 0.001, 0.001, 1.0);
        if (!line_.prepare(loop_rate, max_seconds))
            return false;
        line_.set_length_samples(one_way_length);

        open_end_.set_reflection_gain(-0.995);
        open_end_.set_loss_pole(open_loss_pole);
        reed_.set_mouth_pressure(0.20);
        reed_.set_closing_pressure(0.35);
        reed_.set_flow_gain(1.0);
        reed_.set_bore_impedance(1.0);

        if (factor > 1) {
            oversampler_.set_sample_rate(sample_rate);
            oversampler_.set_factor(static_cast<Oversampler::Factor>(factor));
            oversampler_.set_kind(Oversampler::Kind::linear_phase_fir);
            oversampler_.set_quality(Oversampler::Quality::standard);
        }
        reset();
        return true;
    }

    double process() {
        if (factor_ == 1)
            return process_loop_step();
        return oversampler_.process(0.0, [this](double) { return process_loop_step(); });
    }

    void reset() {
        line_.reset();
        reed_.reset();
        open_end_.reset();
        oversampler_.reset();
        loop_steps_ = 0;
        pending_excitation_ = 0.0;
    }

    void set_mouth_pressure(double value) {
        reed_.set_mouth_pressure(value);
    }
    void set_closing_pressure(double value) {
        reed_.set_closing_pressure(value);
    }
    void set_flow_gain(double value) {
        reed_.set_flow_gain(value);
    }
    void set_length_samples(double value) {
        line_.set_length_samples(value);
    }
    void excite(double value) {
        pending_excitation_ = value;
    }

    int latency_samples() const noexcept {
        return factor_ == 1 ? 0 : oversampler_.latency_samples();
    }
    std::size_t retained_bytes() const noexcept {
        return line_.retained_bytes();
    }
    std::size_t loop_steps() const noexcept {
        return loop_steps_;
    }

  private:
    double process_loop_step() {
        ++loop_steps_;
        double reed_arrival = 0.0;
        double bell_arrival = 0.0;
        line_.read_outputs(reed_arrival, bell_arrival);
        const auto reed_reflection = reed_.process(reed_arrival);
        const auto bell_reflection = open_end_.process(bell_arrival);
        line_.push_inputs(reed_reflection + pending_excitation_, bell_reflection);
        pending_excitation_ = 0.0;
        return bell_arrival;
    }

    pulp::signal::WaveguideLine64 line_{};
    pulp::signal::WaveguideReflectionFilter64 open_end_{};
    pulp::signal::ReedExciter64 reed_{};
    Oversampler oversampler_{};
    int factor_ = 1;
    std::size_t loop_steps_ = 0;
    double pending_excitation_ = 0.0;
};

std::vector<float> render(OversampledReedBore& bore, std::size_t frames) {
    std::vector<float> output(frames);
    for (auto& sample : output)
        sample = static_cast<float>(bore.process());
    return output;
}

double peak(std::span<const float> signal) {
    double value = 0.0;
    for (const auto sample : signal)
        value = std::max(value, std::abs(static_cast<double>(sample)));
    return value;
}

} // namespace

TEST_CASE("ReedExciter matches the independent pressure-valve equation",
          "[signal][waveguide][reed][oracle]") {
    pulp::signal::ReedExciter64 reed;
    reed.set_mouth_pressure(0.62);
    reed.set_closing_pressure(0.41);
    reed.set_flow_gain(0.73);
    reed.set_bore_impedance(1.17);

    for (const auto incident : {-0.8, -0.1, 0.0, 0.2, 0.62, 0.9, 1.4}) {
        const auto expected = reed_reference(incident, 0.62, 0.41, 0.73, 1.17);
        const auto evaluation =
            pulp::signal::ReedExciter64::evaluate(incident, 0.62, 0.41, 0.73, 1.17);
        REQUIRE(evaluation.valid);
        CHECK_THAT(evaluation.reflected_pressure, WithinAbs(expected, 2.0e-15));
        CHECK_THAT(reed.process(incident), WithinAbs(evaluation.reflected_pressure, 2.0e-15));
    }
}

TEST_CASE("ReedExciter clamps controls and contains non-finite samples",
          "[signal][waveguide][reed][fault]") {
    pulp::signal::ReedExciter reed;
    reed.set_mouth_pressure(-1.0f);
    reed.set_closing_pressure(0.0f);
    reed.set_flow_gain(20.0f);
    reed.set_bore_impedance(0.0f);
    CHECK(reed.mouth_pressure() == 0.0f);
    CHECK(reed.closing_pressure() == 0.05f);
    CHECK(reed.flow_gain() == 4.0f);
    CHECK(reed.bore_impedance() == 1.0e-6f);

    reed.set_mouth_pressure(0.5f);
    const auto previous = reed.process(0.1f);
    CHECK(previous != 0.0f);
    CHECK(reed.process(std::numeric_limits<float>::quiet_NaN()) == previous);
    reed.set_flow_gain(std::numeric_limits<float>::infinity());
    CHECK(reed.flow_gain() == 4.0f);
    reed.reset();
    CHECK(reed.process(std::numeric_limits<float>::infinity()) == 0.0f);

    using Reed = pulp::signal::ReedExciter64;
    CHECK(pulp::signal::ReedExciter::evaluate(0.0f, 0.5f,
                                              static_cast<float>(Reed::minimum_closing_pressure),
                                              static_cast<float>(Reed::minimum_flow_gain),
                                              static_cast<float>(Reed::minimum_bore_impedance))
              .valid);
    CHECK_FALSE(Reed::evaluate(0.0, -0.1, 0.35, 1.0, 1.0).valid);
    CHECK_FALSE(Reed::evaluate(0.0, 0.5, 0.01, 1.0, 1.0).valid);
    CHECK_FALSE(Reed::evaluate(0.0, 0.5, 0.35, 5.0, 1.0).valid);
    CHECK_FALSE(Reed::evaluate(0.0, 0.5, 0.35, 1.0, 0.0).valid);
}

TEST_CASE("ReedExciter audio-thread operations allocate nothing", "[signal][waveguide][reed][rt]") {
    pulp::signal::ReedExciter reed;
    reed.set_mouth_pressure(0.6f);
    {
        pulp::test::RtAllocationProbe probe;
        float value = 0.0f;
        for (int i = 0; i < 4096; ++i) {
            reed.set_closing_pressure(0.05f + 0.95f * static_cast<float>(i) / 4095.0f);
            reed.set_flow_gain(0.01f + 3.99f * static_cast<float>(i) / 4095.0f);
            value = reed.process(value * 0.25f);
        }
        reed.reset();
        CHECK(probe.allocation_count() == 0);
    }
}

TEST_CASE("The reed bore self-oscillates at the closed-open tube fundamental",
          "[signal][waveguide][reed][composition][tuning]") {
    for (const auto sample_rate : {44100.0, 48000.0, 96000.0}) {
        for (const auto target_hz : {100.0, 440.0, 2000.0}) {
            OversampledReedBore bore;
            REQUIRE(bore.prepare(sample_rate, 2, target_hz));
            (void)render(bore, static_cast<std::size_t>(sample_rate * 0.5));
            const auto measured = render(bore, 32768);

            pulp::test::audio::PitchOptions options;
            options.min_hz = target_hz * 0.7;
            options.max_hz = target_hz * 1.3;
            options.min_confidence = 0.3;
            const auto estimate = pulp::test::audio::estimate_pitch(
                std::span<const float>(measured), sample_rate, options);
            INFO("sample_rate=" << sample_rate << " target_hz=" << target_hz << " peak="
                                << peak(measured) << " confidence=" << estimate.confidence
                                << " measured_hz=" << estimate.hz);
            REQUIRE(estimate.voiced);
            CHECK(std::abs(pulp::test::audio::cents_between(estimate.hz, target_hz)) <= 10.0);
        }
    }
}

TEST_CASE("Whole-loop oversampling is deterministic, bounded, and block independent",
          "[signal][waveguide][reed][composition][oversampling]") {
    for (const auto factor : {1, 2, 4}) {
        OversampledReedBore whole;
        OversampledReedBore split;
        REQUIRE(whole.prepare(48000.0, factor, 440.0));
        REQUIRE(split.prepare(48000.0, factor, 440.0));

        const auto reference = render(whole, 32768);
        std::vector<float> partitioned;
        partitioned.reserve(reference.size());
        for (const auto count : {1u, 7u, 64u, 4096u, 28600u}) {
            const auto part = render(split, count);
            partitioned.insert(partitioned.end(), part.begin(), part.end());
        }
        INFO("factor=" << factor << " peak=" << peak(reference));
        CHECK(
            std::equal(reference.begin(), reference.end(), partitioned.begin(), partitioned.end()));
        CHECK(peak(reference) > 1.0e-5);
        CHECK(peak(reference) < 4.0);
        CHECK(std::all_of(reference.begin(), reference.end(),
                          [](float sample) { return std::isfinite(sample); }));

        whole.reset();
        split.reset();
        const auto whole_reset = render(whole, 8192);
        const auto split_reset = render(split, 8192);
        CHECK(std::equal(whole_reset.begin(), whole_reset.end(), split_reset.begin(),
                         split_reset.end()));
        CHECK(whole.loop_steps() == 8192u * static_cast<std::size_t>(factor));
        CHECK(split.loop_steps() == 8192u * static_cast<std::size_t>(factor));
    }
}

TEST_CASE("The reed bore remains bounded through long runs and live control sweeps",
          "[signal][waveguide][reed][composition][stability]") {
    for (const auto factor : {1, 2, 4}) {
        OversampledReedBore silent_bore;
        REQUIRE(silent_bore.prepare(48000.0, factor, 220.0));
        silent_bore.set_mouth_pressure(0.0);
        const auto silent = render(silent_bore, 8192);
        CHECK(peak(silent) == 0.0);

        for (int sweep_control = 0; sweep_control < 4; ++sweep_control) {
            OversampledReedBore bore;
            REQUIRE(bore.prepare(48000.0, factor, 220.0));
            auto maximum = 0.0;
            auto all_finite = true;
            for (int frame = 0; frame < 5 * 48000; ++frame) {
                const auto sweep = static_cast<double>(frame % 9600) / 9599.0;
                if (sweep_control == 0)
                    bore.set_length_samples(12.0 + 188.0 * sweep);
                else if (sweep_control == 1)
                    bore.set_mouth_pressure(0.34 * sweep);
                else if (sweep_control == 2)
                    bore.set_closing_pressure(0.05 + 0.95 * sweep);
                else
                    bore.set_flow_gain(0.01 + 3.99 * sweep);
                const auto output = bore.process();
                all_finite = all_finite && std::isfinite(output);
                maximum = std::max(maximum, std::abs(output));
            }
            constexpr auto reflection_magnitude = 0.995;
            const auto maximum_flow_gain = sweep_control == 3 ? 4.0 : 1.0;
            // At a round trip, x_next <= r * (x + G * sqrt(x)). Solving the
            // corresponding fixed point gives (rG / (1-r))^2; one normalized
            // pressure unit covers the swept mouth-pressure offset.
            const auto equilibrium_bound =
                std::pow(reflection_magnitude * maximum_flow_gain / (1.0 - reflection_magnitude),
                         2.0) +
                1.0;
            INFO("factor=" << factor << " control=" << sweep_control << " peak=" << maximum
                           << " bound=" << equilibrium_bound);
            CHECK(all_finite);
            CHECK(maximum < equilibrium_bound);
        }
    }
}

TEST_CASE("A closed reed bore dissipates a finite impulse",
          "[signal][waveguide][reed][composition][decay]") {
    for (const auto factor : {1, 2, 4}) {
        OversampledReedBore bore;
        REQUIRE(bore.prepare(48000.0, factor, 220.0));
        bore.set_mouth_pressure(1.0);
        bore.set_closing_pressure(0.05);
        bore.excite(0.5);
        const auto response = render(bore, 5 * 48000);

        const auto early = peak(std::span<const float>(response).subspan(0, 48000));
        const auto late = peak(std::span<const float>(response).subspan(4 * 48000, 48000));
        INFO("factor=" << factor << " early=" << early << " late=" << late);
        CHECK(early > 1.0e-4);
        CHECK(late < early * 1.0e-3);
    }
}

TEST_CASE("The composition reports only the oversampler delay",
          "[signal][waveguide][reed][composition][latency]") {
    for (const auto factor : {1, 2, 4}) {
        OversampledReedBore bore;
        REQUIRE(bore.prepare(48000.0, factor, 110.0));

        int expected = 0;
        if (factor > 1) {
            pulp::signal::Oversampler64 oversampler;
            oversampler.set_sample_rate(48000.0);
            oversampler.set_factor(static_cast<pulp::signal::Oversampler64::Factor>(factor));
            oversampler.set_kind(pulp::signal::Oversampler64::Kind::linear_phase_fir);
            oversampler.set_quality(pulp::signal::Oversampler64::Quality::standard);
            expected = oversampler.latency_samples();
        }
        CHECK(bore.latency_samples() == expected);
    }
}

TEST_CASE("The complete oversampled reed loop allocates nothing while processing",
          "[signal][waveguide][reed][composition][rt]") {
    for (const auto factor : {1, 2, 4}) {
        OversampledReedBore bore;
        REQUIRE(bore.prepare(48000.0, factor, 220.0));
        (void)render(bore, 4096);
        {
            pulp::test::RtAllocationProbe probe;
            double output = 0.0;
            for (int i = 0; i < 4096; ++i) {
                bore.set_mouth_pressure(static_cast<double>(i) / 4095.0);
                bore.set_closing_pressure(0.05 + 0.95 * static_cast<double>(i) / 4095.0);
                bore.set_flow_gain(0.01 + 3.99 * static_cast<double>(i) / 4095.0);
                output = bore.process();
            }
            CHECK(std::isfinite(output));
            CHECK(probe.allocation_count() == 0);
        }
    }
}
