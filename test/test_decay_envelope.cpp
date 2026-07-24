// Tests for the one-shot percussion envelope and its coefficient helpers.
//
// The two decay conventions the header offers -- an RC time constant and a
// T60 -- are only useful if a caller can trust the number they type. So the
// tests measure the rendered contour and check it hits 1/e and -60 dB at the
// requested times, at more than one sample rate, rather than checking that a
// coefficient formula matches itself.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/decay_envelope.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using pulp::signal::DecayEnvelope64;

std::vector<double> render(DecayEnvelope64& env, int n) {
    std::vector<double> y(static_cast<std::size_t>(n));
    for (auto& v : y) v = env.process();
    return y;
}

// Index of the first sample at or below `level`, or -1.
int first_below(const std::vector<double>& y, double level) {
    for (std::size_t i = 0; i < y.size(); ++i) {
        if (y[i] <= level) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

TEST_CASE("An RC decay reaches one over e at its time constant",
          "[signal][envelope]") {
    constexpr double kFs = 48000.0;
    DecayEnvelope64 env;
    env.set_sample_rate(kFs);
    env.set_attack_ms(0.0);
    env.set_hold_ms(0.0);
    env.set_decay_time_constant_ms(100.0);
    env.trigger();

    const auto y = render(env, 48000);
    const auto at_tau = static_cast<std::size_t>(0.1 * kFs);
    REQUIRE(y[at_tau] > 0.36);
    REQUIRE(y[at_tau] < 0.38);
}

TEST_CASE("A T60 decay reaches minus sixty decibels at its stated time",
          "[signal][envelope]") {
    constexpr double kFs = 48000.0;
    DecayEnvelope64 env;
    env.set_sample_rate(kFs);
    env.set_attack_ms(0.0);
    env.set_hold_ms(0.0);
    env.set_decay_t60_ms(250.0);
    env.trigger();

    const auto y = render(env, 48000);
    const auto at_t60 = static_cast<std::size_t>(0.25 * kFs);
    const double db = 20.0 * std::log10(y[at_t60] + 1e-30);
    REQUIRE(db < -59.0);
    REQUIRE(db > -61.0);
}

TEST_CASE("Decay timing is independent of sample rate", "[signal][envelope]") {
    // Same requested T60, two rates: the -60 dB crossing must land at the same
    // wall-clock time, not the same sample index.
    auto crossing_seconds = [](double fs) {
        DecayEnvelope64 env;
        env.set_sample_rate(fs);
        env.set_attack_ms(0.0);
        env.set_decay_t60_ms(180.0);
        env.trigger();
        const auto y = render(env, static_cast<int>(fs));
        const int idx = first_below(y, 0.001);
        REQUIRE(idx > 0);
        return static_cast<double>(idx) / fs;
    };

    const double at_44 = crossing_seconds(44100.0);
    const double at_96 = crossing_seconds(96000.0);
    REQUIRE(std::fabs(at_44 - at_96) < 0.002);
}

TEST_CASE("The attack ramp removes the step edge", "[signal][envelope]") {
    // Without a ramp the envelope would jump from 0 to 1 in one sample. The
    // ramp's job is to spread that over the requested time, so the largest
    // single-sample jump must be far below full scale.
    constexpr double kFs = 48000.0;
    DecayEnvelope64 env;
    env.set_sample_rate(kFs);
    env.set_attack_ms(1.0);
    env.set_decay_t60_ms(500.0);
    env.trigger();

    const auto y = render(env, 480);
    double largest_step = y[0];
    for (std::size_t i = 1; i < y.size(); ++i) {
        largest_step = std::max(largest_step, std::fabs(y[i] - y[i - 1]));
    }
    // A 1 ms ramp at 48 kHz is 48 samples, so each step is about 1/48.
    REQUIRE(largest_step < 0.03);
    // ...and it must still get all the way up.
    REQUIRE(y[static_cast<std::size_t>(0.001 * kFs)] > 0.99);
}

TEST_CASE("A zero attack reaches full scale on the first sample",
          "[signal][envelope]") {
    DecayEnvelope64 env;
    env.set_sample_rate(48000.0);
    env.set_attack_ms(0.0);
    env.set_decay_t60_ms(100.0);
    env.trigger();
    REQUIRE(env.process() == 1.0);
}

TEST_CASE("Hold keeps the envelope at full scale before the decay",
          "[signal][envelope]") {
    constexpr double kFs = 48000.0;
    DecayEnvelope64 env;
    env.set_sample_rate(kFs);
    env.set_attack_ms(0.0);
    env.set_hold_ms(10.0);
    env.set_decay_t60_ms(100.0);
    env.trigger();

    const auto y = render(env, 1440);
    const auto near_end_of_hold = static_cast<std::size_t>(0.009 * kFs);
    REQUIRE(y[near_end_of_hold] == 1.0);
    const auto after_hold = static_cast<std::size_t>(0.02 * kFs);
    REQUIRE(y[after_hold] < 0.6);
}

TEST_CASE("The trigger peak scales the whole contour", "[signal][envelope]") {
    auto peak_render = [](double peak) {
        DecayEnvelope64 env;
        env.set_sample_rate(48000.0);
        env.set_attack_ms(0.0);
        env.set_decay_t60_ms(120.0);
        env.trigger(peak);
        return render(env, 2000);
    };

    const auto full = peak_render(1.0);
    const auto half = peak_render(0.5);
    for (std::size_t i = 0; i < full.size(); ++i) {
        REQUIRE(std::fabs(half[i] - 0.5 * full[i]) < 1e-12);
    }
}

TEST_CASE("The envelope reports itself finished and then emits exact zero",
          "[signal][envelope]") {
    // A voice relies on this to stop rendering. If the envelope stayed
    // nominally active forever, an idle kit would keep every voice on the CPU.
    DecayEnvelope64 env;
    env.set_sample_rate(48000.0);
    env.set_attack_ms(0.0);
    env.set_decay_t60_ms(50.0);
    env.trigger();
    REQUIRE(env.is_active());

    int samples = 0;
    while (env.is_active() && samples < 480000) {
        env.process();
        ++samples;
    }
    REQUIRE_FALSE(env.is_active());
    // -100 dB is reached well before -60 dB has had time to happen twice.
    REQUIRE(samples < static_cast<int>(0.2 * 48000.0));
    for (int i = 0; i < 64; ++i) REQUIRE(env.process() == 0.0);
}

TEST_CASE("Reset silences the envelope immediately", "[signal][envelope]") {
    DecayEnvelope64 env;
    env.set_sample_rate(48000.0);
    env.set_decay_t60_ms(1000.0);
    env.trigger();
    for (int i = 0; i < 100; ++i) env.process();
    REQUIRE(env.is_active());

    env.reset();
    REQUIRE_FALSE(env.is_active());
    REQUIRE(env.process() == 0.0);
}

TEST_CASE("Retriggering restarts from silence", "[signal][envelope]") {
    // A percussion envelope must not sum with its own tail on a fast
    // retrigger, or repeated hits build up level.
    DecayEnvelope64 env;
    env.set_sample_rate(48000.0);
    env.set_attack_ms(1.0);
    env.set_decay_t60_ms(400.0);
    env.trigger();
    for (int i = 0; i < 48; ++i) env.process();
    const double before = env.value();
    REQUIRE(before > 0.9);

    env.trigger();
    REQUIRE(env.process() < 0.05);
}

TEST_CASE("A zero trigger peak leaves the envelope inactive",
          "[signal][envelope]") {
    DecayEnvelope64 env;
    env.set_sample_rate(48000.0);
    env.trigger(0.0);
    REQUIRE_FALSE(env.is_active());
    REQUIRE(env.process() == 0.0);
}

TEST_CASE("A default-constructed envelope produces its documented contour",
          "[signal][envelope]") {
    // The default constructor computes coefficients so a caller that only sets
    // one control still gets a decay rather than instant silence.
    DecayEnvelope64 env;
    env.trigger();
    const auto y = render(env, 4410);
    REQUIRE(y[100] > 0.5);
    REQUIRE(y[4409] < y[100]);
}

TEST_CASE("Envelope rendering allocates nothing on the audio thread",
          "[signal][envelope][rt-safety]") {
    DecayEnvelope64 env;
    env.set_sample_rate(48000.0);
    env.set_attack_ms(0.5);
    env.set_hold_ms(2.0);
    env.set_decay_t60_ms(300.0);

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int hit = 0; hit < 8; ++hit) {
            env.trigger(0.8);
            for (int i = 0; i < 4096; ++i) sink += env.process();
        }
        env.reset();
        allocations = probe.allocation_count();
    }

    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}
