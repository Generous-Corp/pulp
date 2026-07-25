// Tests for the single-resonance body.
//
// A resonator makes three promises: it rings at the frequency it was tuned to,
// it dies away over the T60 it was given, and its level does not change when
// the decay does. The third is the one that is easy to get wrong -- the
// obvious implementation makes a long decay also mean a loud ring, so the
// decay control doubles as a gain control and every preset has to compensate.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/two_pole_resonator.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using pulp::signal::TwoPoleResonator64;

constexpr double kFs = 48000.0;

// Rings the resonator with a unit impulse and returns the response.
std::vector<double> impulse_response(TwoPoleResonator64& r, int n) {
    std::vector<double> y(static_cast<std::size_t>(n));
    y[0] = r.process(1.0);
    for (int i = 1; i < n; ++i) y[static_cast<std::size_t>(i)] = r.process(0.0);
    return y;
}

double crossing_rate(const std::vector<double>& x, std::size_t from, std::size_t to) {
    int crossings = 0;
    for (std::size_t i = from + 1; i < to && i < x.size(); ++i) {
        if ((x[i - 1] <= 0.0) != (x[i] <= 0.0)) ++crossings;
    }
    return 0.5 * crossings * kFs / static_cast<double>(to - from);
}

double rms(const std::vector<double>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i) sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

double peak(const std::vector<double>& x) {
    double m = 0.0;
    for (double v : x) m = std::max(m, std::fabs(v));
    return m;
}

}  // namespace

TEST_CASE("The resonator rings at its tuned frequency",
          "[signal][resonator]") {
    for (double f : {60.0, 220.0, 1000.0, 5000.0}) {
        TwoPoleResonator64 r;
        r.set_sample_rate(kFs);
        r.set_frequency(f);
        r.set_t60_ms(500.0);
        r.reset();
        const auto y = impulse_response(r, 24000);
        REQUIRE(std::fabs(crossing_rate(y, 480, 12000) - f) < f * 0.03);
    }
}

TEST_CASE("The ring falls by sixty decibels over its T60",
          "[signal][resonator]") {
    TwoPoleResonator64 r;
    r.set_sample_rate(kFs);
    r.set_frequency(200.0);
    r.set_t60_ms(200.0);
    r.reset();

    const auto y = impulse_response(r, 24000);
    const double early = rms(y, 0, 480);                 // first 10 ms
    const double at_t60 = rms(y, 9600, 10080);           // 200-210 ms
    const double drop_db = 20.0 * std::log10(at_t60 / (early + 1e-30));
    REQUIRE(drop_db < -55.0);
    REQUIRE(drop_db > -68.0);
}

TEST_CASE("Decay time does not change how loud the strike is",
          "[signal][resonator]") {
    // The decay control must be a decay control. Normalising the input by one
    // minus the pole radius -- the convention for a sustained tone -- would make
    // a long decay produce a markedly quieter strike, so a preset changing its
    // decay would have to compensate its level.
    auto peak_for = [](double t60_ms) {
        TwoPoleResonator64 r;
        r.set_sample_rate(kFs);
        r.set_frequency(150.0);
        r.set_t60_ms(t60_ms);
        r.reset();
        return peak(impulse_response(r, 48000));
    };

    // A very short decay dies before the ring has finished building to its
    // peak, so the two are not identical; the assertion is that they are the
    // same order rather than the ~40x spread an unnormalised resonator shows.
    const double brief = peak_for(50.0);
    const double long_ring = peak_for(2000.0);
    REQUIRE(long_ring < brief * 1.4);
    REQUIRE(long_ring > brief * 0.7);
}

TEST_CASE("Tuning does not change how loud the strike is",
          "[signal][resonator]") {
    // The trap this guards: an unnormalised two-pole peaks at one over the sine
    // of its pole angle, so a body tuned two decades apart differs in level by
    // about a hundred times and the tune control doubles as a volume control.
    auto peak_for = [](double hz) {
        TwoPoleResonator64 r;
        r.set_sample_rate(kFs);
        r.set_frequency(hz);
        r.set_t60_ms(1000.0);
        r.reset();
        return peak(impulse_response(r, 48000));
    };

    const double low = peak_for(50.0);
    const double high = peak_for(5000.0);
    REQUIRE(low > 0.5);
    REQUIRE(high > 0.5);
    REQUIRE(low < high * 2.0);
    REQUIRE(high < low * 2.0);
}

TEST_CASE("The ring reports itself finished", "[signal][resonator]") {
    TwoPoleResonator64 r;
    r.set_sample_rate(kFs);
    r.set_frequency(150.0);
    r.set_t60_ms(60.0);
    r.reset();
    REQUIRE_FALSE(r.is_ringing());

    r.process(1.0);
    REQUIRE(r.is_ringing());

    for (int i = 0; i < static_cast<int>(kFs); ++i) r.process(0.0);
    REQUIRE_FALSE(r.is_ringing());
}

TEST_CASE("What excites the body changes what the body sounds like",
          "[signal][resonator]") {
    // The difference between a resonator and an oscillator with an envelope:
    // the excitation is part of the sound. A short click and a longer burst
    // through the same body must not produce the same tail.
    TwoPoleResonator64 clicked;
    clicked.set_sample_rate(kFs);
    clicked.set_frequency(180.0);
    clicked.set_t60_ms(400.0);
    clicked.reset();

    TwoPoleResonator64 brushed;
    brushed.set_sample_rate(kFs);
    brushed.set_frequency(180.0);
    brushed.set_t60_ms(400.0);
    brushed.reset();

    std::vector<double> a(12000), b(12000);
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = clicked.process(i == 0 ? 1.0 : 0.0);
        // A burst spread over a few milliseconds, same total energy scale.
        b[i] = brushed.process(i < 240 ? 1.0 / 240.0 : 0.0);
    }

    const double difference = rms(a, 2400, 12000) / (rms(b, 2400, 12000) + 1e-30);
    REQUIRE(difference > 1.2);
}

TEST_CASE("Reset clears the ring", "[signal][resonator]") {
    TwoPoleResonator64 r;
    r.set_sample_rate(kFs);
    r.set_frequency(150.0);
    r.set_t60_ms(1000.0);
    r.reset();
    r.process(1.0);
    for (int i = 0; i < 100; ++i) r.process(0.0);
    REQUIRE(r.is_ringing());

    r.reset();
    REQUIRE_FALSE(r.is_ringing());
    REQUIRE(r.process(0.0) == 0.0);
}

TEST_CASE("A frequency above Nyquist is clamped rather than folded",
          "[signal][resonator]") {
    TwoPoleResonator64 r;
    r.set_sample_rate(kFs);
    r.set_t60_ms(200.0);
    r.set_frequency(100000.0);
    REQUIRE(r.frequency() <= 0.5 * kFs);
    r.reset();
    const auto y = impulse_response(r, 4800);
    for (double v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("Sweeping the frequency carries the ring rather than restarting it",
          "[signal][resonator]") {
    // The state is the ring itself, so a coefficient change between samples is
    // continuous. A realization whose state depended on its coefficients would
    // glitch here.
    TwoPoleResonator64 r;
    r.set_sample_rate(kFs);
    r.set_frequency(400.0);
    r.set_t60_ms(1000.0);
    r.reset();
    r.process(1.0);

    std::vector<double> y(24000);
    for (std::size_t i = 0; i < y.size(); ++i) {
        r.set_frequency(400.0 * std::exp2(-2.0 * static_cast<double>(i) / 24000.0));
        y[i] = r.process(0.0);
    }

    double largest_step = 0.0;
    for (std::size_t i = 1; i < y.size(); ++i) {
        largest_step = std::max(largest_step, std::fabs(y[i] - y[i - 1]));
    }
    // A continuous sweep of a decaying ring cannot step by more than the ring
    // amplitude itself in one sample.
    REQUIRE(largest_step < peak(y) * 0.5);
    REQUIRE(crossing_rate(y, 20000, 24000) < crossing_rate(y, 0, 4000));
}

TEST_CASE("The resonator allocates nothing on the audio thread",
          "[signal][resonator][rt-safety]") {
    TwoPoleResonator64 r;
    r.set_sample_rate(kFs);
    r.set_frequency(180.0);
    r.set_t60_ms(300.0);
    r.reset();

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 48000; ++i) {
            r.set_frequency(180.0 + 0.001 * i);
            sink += r.process(i == 0 ? 1.0 : 0.0);
        }
        r.reset();
        allocations = probe.allocation_count();
    }

    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}
