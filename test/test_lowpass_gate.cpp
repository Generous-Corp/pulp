// Tests for the vactrol-modelled lowpass gate.
//
// The defining behaviours are asymmetry and coupling: the control rises far
// faster than it falls, and a falling control darkens the signal as well as
// quietening it. Both are measured here -- the asymmetry from the control
// trajectory, the coupling from the ratio of high-band to low-band energy
// during a decay -- because a gate that passed a level check alone could still
// be an ordinary VCA with the interesting part missing.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/lowpass_gate.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using pulp::signal::LowpassGate64;

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

double goertzel(const std::vector<double>& x, double fs, double f) {
    const std::size_t n = x.size();
    const double w = 2.0 * kPi * f / fs;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        const double s0 = coeff * s1 - s2 + win * x[i];
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (static_cast<double>(n) * 0.25);
}

// Number of samples the control takes to travel from its current value to
// `target_control`, driven by a step to `target`.
int samples_to_reach(LowpassGate64& gate, double target, double target_control,
                     bool rising) {
    for (int i = 0; i < static_cast<int>(kFs * 10); ++i) {
        gate.process(1.0, target);
        if (rising ? gate.control() >= target_control : gate.control() <= target_control) {
            return i + 1;
        }
    }
    return -1;
}

// Sum of two tones so the gate's effect on each band can be measured
// separately from one render.
std::vector<double> two_tone(std::size_t n, double low_hz, double high_hz) {
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        y[i] = 0.5 * std::sin(2.0 * kPi * low_hz * t) +
               0.5 * std::sin(2.0 * kPi * high_hz * t);
    }
    return y;
}

}  // namespace

TEST_CASE("The gate opens much faster than it closes", "[signal][lpg]") {
    // This asymmetry is the entire reason the component exists. A symmetric
    // smoother would let a lowpass gate produce a fast release, which the real
    // device cannot do.
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.set_rise_ms(2.0);
    gate.set_fall_ms(200.0);
    gate.reset();

    const int rise = samples_to_reach(gate, 1.0, 0.63, true);
    REQUIRE(rise > 0);
    const int fall = samples_to_reach(gate, 0.0, 0.37, false);
    REQUIRE(fall > 0);

    // 2 ms against 200 ms is a factor of a hundred; allow generous slack and
    // still assert the order of magnitude.
    REQUIRE(fall > rise * 50);
}

TEST_CASE("Rise and fall times match the requested milliseconds",
          "[signal][lpg]") {
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.set_rise_ms(5.0);
    gate.set_fall_ms(120.0);
    gate.reset();

    const int rise = samples_to_reach(gate, 1.0, 1.0 - std::exp(-1.0), true);
    REQUIRE(std::fabs(static_cast<double>(rise) / kFs - 0.005) < 0.001);

    // A time constant is measured from a settled start, so drive the control
    // all the way open before timing the fall. Timing it from wherever the
    // rise measurement stopped would measure a fraction of the decay and pass
    // for the wrong reason.
    for (int i = 0; i < static_cast<int>(kFs); ++i) gate.process(1.0, 1.0);
    REQUIRE(gate.control() > 0.999);

    const int fall = samples_to_reach(gate, 0.0, std::exp(-1.0), false);
    REQUIRE(std::fabs(static_cast<double>(fall) / kFs - 0.120) < 0.010);
}

TEST_CASE("A closing gate darkens the signal as well as quietening it",
          "[signal][lpg]") {
    // Coupling check: render a two-tone signal through a decaying control and
    // compare how much each tone survives. The high tone must lose more than
    // the low one, which is what "quiet implies dark" means numerically.
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.set_rise_ms(1.0);
    gate.set_fall_ms(60.0);
    gate.set_colour(1.0);  // pure filter, so any level change is the filter's
    gate.set_closed_cutoff_hz(80.0);
    gate.set_open_cutoff_hz(12000.0);
    gate.reset();

    const auto input = two_tone(16384, 150.0, 6000.0);

    // Hold the gate open and capture the reference balance.
    std::vector<double> open(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) open[i] = gate.process(input[i], 1.0);
    const double open_low = goertzel(open, kFs, 150.0);
    const double open_high = goertzel(open, kFs, 6000.0);

    // Now let it close and capture the same balance on the tail.
    std::vector<double> closing(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) closing[i] = gate.process(input[i], 0.0);
    // Skip the first part of the fall so the measurement sits on the tail.
    const std::vector<double> tail(closing.begin() + 8192, closing.end());
    const double tail_low = goertzel(tail, kFs, 150.0);
    const double tail_high = goertzel(tail, kFs, 6000.0);

    const double low_survival = tail_low / (open_low + 1e-20);
    const double high_survival = tail_high / (open_high + 1e-20);
    REQUIRE(high_survival < low_survival * 0.2);
}

TEST_CASE("Colour zero is a pure amplitude gate", "[signal][lpg]") {
    // At colour 0 the two tones must lose the same amount, because nothing is
    // filtering -- only the VCA is acting.
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.set_colour(0.0);
    gate.set_fall_ms(400.0);
    gate.reset();

    const auto input = two_tone(16384, 150.0, 6000.0);
    std::vector<double> open(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) open[i] = gate.process(input[i], 1.0);
    const double open_low = goertzel(open, kFs, 150.0);
    const double open_high = goertzel(open, kFs, 6000.0);

    std::vector<double> closing(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) closing[i] = gate.process(input[i], 0.0);
    const std::vector<double> tail(closing.begin() + 8192, closing.end());
    const double low_survival = goertzel(tail, kFs, 150.0) / (open_low + 1e-20);
    const double high_survival = goertzel(tail, kFs, 6000.0) / (open_high + 1e-20);

    // The gate must actually have closed. Without this the equality check
    // below would also pass for a gate that did nothing at all, which is
    // exactly the bug it is meant to catch.
    REQUIRE(low_survival < 0.5);
    REQUIRE(std::fabs(low_survival - high_survival) < 0.1 * (low_survival + 1e-9));
}

TEST_CASE("A fully open gate passes the signal essentially unchanged",
          "[signal][lpg]") {
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.set_colour(0.5);
    gate.set_open_cutoff_hz(18000.0);
    gate.set_rise_ms(0.1);
    gate.reset();

    // Settle the control at fully open.
    for (int i = 0; i < 4800; ++i) gate.process(0.0, 1.0);

    std::vector<double> input(8192), output(8192);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kFs);
        output[i] = gate.process(input[i], 1.0);
    }
    const double in_level = goertzel(input, kFs, 1000.0);
    const double out_level = goertzel(output, kFs, 1000.0);
    REQUIRE(out_level > in_level * 0.9);
}

TEST_CASE("The gain exponent shapes how the tail spends its travel",
          "[signal][lpg]") {
    // A higher exponent must put the control's midpoint at a lower gain, which
    // is what gives a vactrol its long quiet tail instead of a linear fade.
    auto gain_at_half = [](double exponent) {
        LowpassGate64 gate;
        gate.set_sample_rate(kFs);
        gate.set_colour(0.0);
        gate.set_gain_exponent(exponent);
        gate.set_rise_ms(1.0);
        gate.reset();
        // Drive the control to 0.5 and read the applied gain from a unit input.
        double y = 0.0;
        for (int i = 0; i < static_cast<int>(kFs); ++i) {
            y = gate.process(1.0, 0.5);
            if (gate.control() > 0.4999) break;
        }
        return y;
    };

    const double linear = gain_at_half(1.0);
    const double vactrol = gain_at_half(2.5);
    REQUIRE(vactrol < linear * 0.6);
}

TEST_CASE("Reset returns the gate to fully closed", "[signal][lpg]") {
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.reset();
    for (int i = 0; i < 4800; ++i) gate.process(1.0, 1.0);
    REQUIRE(gate.control() > 0.9);

    gate.reset();
    REQUIRE(gate.control() == 0.0);
}

TEST_CASE("The control is clamped to its documented range", "[signal][lpg]") {
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.reset();
    for (int i = 0; i < 4800; ++i) gate.process(1.0, 5.0);
    REQUIRE(gate.control() <= 1.0);
    for (int i = 0; i < 48000; ++i) gate.process(1.0, -5.0);
    REQUIRE(gate.control() >= 0.0);
}

TEST_CASE("The lowpass gate allocates nothing on the audio thread",
          "[signal][lpg][rt-safety]") {
    LowpassGate64 gate;
    gate.set_sample_rate(kFs);
    gate.set_colour(0.7);
    gate.set_rise_ms(1.5);
    gate.set_fall_ms(300.0);
    gate.reset();

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 16384; ++i) {
            const double target = (i / 2048) % 2 == 0 ? 1.0 : 0.0;
            sink += gate.process(std::sin(0.01 * i), target);
        }
        gate.reset();
        allocations = probe.allocation_count();
    }

    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}
