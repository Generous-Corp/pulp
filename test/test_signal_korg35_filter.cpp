// Korg35-style two-pole filter: response against the analytic prototype,
// resonance peak location, stability across the parameter space, and the
// self-oscillation threshold.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/korg35_filter.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinRel;
using Filter = pulp::signal::Korg35Filter64;
using Mode = Filter::Mode;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double warped_omega(double hz, double cutoff) {
    // Frequency relative to the cutoff in the analog prototype after prewarp.
    return std::tan(kPi * hz / kFs) / std::tan(kPi * cutoff / kFs);
}

// Sine amplitude (from RMS over whole periods) of the steady-state output.
double gain_at(Mode mode, double cutoff, double resonance, double drive, double hz,
               double amplitude = 1.0) {
    Filter f;
    f.configure(mode, kFs, cutoff, resonance, drive);
    const int settle = 30000, window = 4800;
    double sum = 0.0;
    for (int n = 0; n < settle + window; ++n) {
        const double y = f.process(amplitude * std::sin(2.0 * kPi * hz * n / kFs));
        if (n >= settle)
            sum += y * y;
    }
    return std::sqrt(2.0 * sum / window) / amplitude;
}

double zero_crossing_hz(const std::vector<double>& y) {
    int crossings = 0;
    for (std::size_t i = 1; i < y.size(); ++i)
        if ((y[i - 1] < 0.0) != (y[i] < 0.0))
            ++crossings;
    return kFs * 0.5 * crossings / static_cast<double>(y.size());
}

std::vector<double> impulse_tail(Filter& f, int skip, int keep) {
    std::vector<double> out;
    for (int n = 0; n < skip + keep; ++n) {
        const double y = f.process(n == 0 ? 1.0 : 0.0);
        if (n >= skip)
            out.push_back(y);
    }
    return out;
}

double peak_abs(const std::vector<double>& v) {
    double p = 0.0;
    for (double x : v)
        p = std::max(p, std::abs(x));
    return p;
}

} // namespace

TEST_CASE("Korg35 linear loop matches the analytic prototype response",
          "[signal][korg35][response]") {
    const double cutoff = 3000.0;
    for (const Mode mode : {Mode::lowpass, Mode::highpass}) {
        for (const double resonance : {0.0, 0.5, 0.9}) {
            for (const double hz : {200.0, 1500.0, 2500.0, 3000.0, 3600.0, 8000.0}) {
                const double k = 2.0 * resonance;
                const double want = Filter::prototype_magnitude(mode, k, warped_omega(hz, cutoff));
                const double got = gain_at(mode, cutoff, resonance, 0.0, hz);
                INFO("mode " << static_cast<int>(mode) << " r " << resonance << " hz " << hz);
                CHECK_THAT(got, WithinRel(want, 0.01));
            }
        }
    }
}

TEST_CASE("Korg35 resonance peak sits at the predicted frequency near the cutoff",
          "[signal][korg35][peak]") {
    const double cutoff = 2000.0;
    const double resonance = 0.95; // k = 1.9, damping 0.1
    const double damping = 2.0 - 2.0 * resonance;
    for (const Mode mode : {Mode::lowpass, Mode::highpass}) {
        // The prototype peak: |H| of 1/(s^2 + d s + 1) peaks at sqrt(1 - d^2/2);
        // the highpass peaks at the reciprocal.
        const double omega_peak = mode == Mode::lowpass
                                      ? std::sqrt(1.0 - damping * damping / 2.0)
                                      : 1.0 / std::sqrt(1.0 - damping * damping / 2.0);
        const double want_hz = kFs / kPi * std::atan(omega_peak * std::tan(kPi * cutoff / kFs));
        double best_hz = 0.0, best_gain = 0.0;
        for (double hz = 1700.0; hz <= 2300.0; hz += 10.0) {
            const double g = gain_at(mode, cutoff, resonance, 0.0, hz);
            if (g > best_gain) {
                best_gain = g;
                best_hz = hz;
            }
        }
        INFO("mode " << static_cast<int>(mode));
        CHECK(std::abs(best_hz - want_hz) <= 15.0);
        CHECK(std::abs(best_hz - cutoff) / cutoff < 0.03);
        // The peak gain is 1 / damping = 10 (20 dB) over the passband.
        CHECK_THAT(best_gain, WithinRel(1.0 / damping, 0.02));
    }
}

TEST_CASE("Korg35 passband gain is unity regardless of resonance", "[signal][korg35][gain]") {
    for (const double resonance : {0.0, 0.7, 0.95}) {
        CHECK_THAT(gain_at(Mode::lowpass, 4000.0, resonance, 0.0, 40.0), WithinRel(1.0, 0.01));
        CHECK_THAT(gain_at(Mode::highpass, 500.0, resonance, 0.0, 20000.0), WithinRel(1.0, 0.01));
    }
}

TEST_CASE("Korg35 stays finite and bounded across the parameter space",
          "[signal][korg35][stability]") {
    std::uint32_t rng = 12345u;
    auto noise = [&] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<double>(rng >> 8) / 8388608.0 - 1.0;
    };
    for (const Mode mode : {Mode::lowpass, Mode::highpass}) {
        for (const double drive : {0.0, 1.0, 8.0}) {
            for (const double cutoff : {20.0, 400.0, 5000.0, 15000.0, 23500.0}) {
                for (const double resonance : {0.0, 0.5, 0.99, 1.0, 1.25, 5.0}) {
                    Filter f;
                    f.configure(mode, kFs, cutoff, resonance, drive);
                    double worst = 0.0;
                    for (int n = 0; n < 6000; ++n) {
                        const double y = f.process(noise());
                        REQUIRE(std::isfinite(y));
                        worst = std::max(worst, std::abs(y));
                    }
                    INFO("mode " << static_cast<int>(mode) << " drive " << drive << " fc " << cutoff
                                 << " r " << resonance);
                    // A saturated loop is limited by the tanh; a linear loop is
                    // clamped below the stability edge, so a 1 V noise input
                    // cannot run away in either case.
                    CHECK(worst < 2.0e4);
                }
            }
        }
    }
}

TEST_CASE("Korg35 impulse response decays below the threshold and rings above it",
          "[signal][korg35][self-oscillation]") {
    const double cutoff = 1500.0;
    for (const Mode mode : {Mode::lowpass, Mode::highpass}) {
        INFO("mode " << static_cast<int>(mode));
        {
            // Below the threshold the ring dies away.
            Filter f;
            f.configure(mode, kFs, cutoff, 0.97, 1.0);
            const auto early = impulse_tail(f, 0, 2000);
            const auto late = impulse_tail(f, 40000, 2000);
            CHECK(peak_abs(late) < 1e-3 * peak_abs(early));
        }
        {
            // Above it the oscillation is sustained, bounded and at the cutoff.
            Filter f;
            f.configure(mode, kFs, cutoff, 1.1, 1.0);
            const auto tail = impulse_tail(f, 40000, 16384);
            const double amplitude = peak_abs(tail);
            CHECK(amplitude > 0.05);
            CHECK(amplitude < 10.0);
            const double hz = zero_crossing_hz(tail);
            CHECK(std::abs(hz - cutoff) / cutoff < 0.03);
        }
    }
}

TEST_CASE("Korg35 feedback saturation limits a loud resonant input",
          "[signal][korg35][saturation]") {
    const double cutoff = 2000.0;
    const double small = gain_at(Mode::lowpass, cutoff, 0.95, 1.0, cutoff, 0.001);
    const double loud = gain_at(Mode::lowpass, cutoff, 0.95, 1.0, cutoff, 1.0);
    // Small signals see the linear peak; large ones are compressed.
    CHECK_THAT(small, WithinRel(10.0, 0.05));
    CHECK(loud < 0.5 * small);
    // Without a saturator the same input stays linear.
    const double linear = gain_at(Mode::lowpass, cutoff, 0.95, 0.0, cutoff, 1.0);
    CHECK_THAT(linear, WithinRel(small, 0.05));
}

TEST_CASE("Korg35 survives parameter changes on every sample", "[signal][korg35][modulation]") {
    Filter f;
    f.configure(Mode::lowpass, kFs, 1000.0, 0.5, 1.0);
    for (int n = 0; n < 48000; ++n) {
        const double phase = 2.0 * kPi * 3.0 * n / kFs;
        f.set_frequency(200.0 + 10000.0 * (0.5 + 0.5 * std::sin(phase)));
        f.set_resonance(0.5 + 0.7 * std::sin(0.37 * phase));
        const double y = f.process(std::sin(0.05 * n));
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 100.0);
    }
    f.set_mode(Mode::highpass);
    for (int n = 0; n < 4800; ++n)
        REQUIRE(std::isfinite(f.process(0.0)));
}

TEST_CASE("Korg35 feedback solver matches bisection", "[signal][korg35][solver]") {
    for (const double drive : {0.25, 1.0, 4.0, 16.0}) {
        for (const double a : {0.0, 0.2, 0.4, 0.62}) {
            for (const double c : {-200.0, -5.0, -0.7, -0.01, 0.0, 0.05, 0.9, 5.0, 200.0}) {
                double lo = c - a / drive - 1.0, hi = c + a / drive + 1.0;
                for (int i = 0; i < 200; ++i) {
                    const double mid = 0.5 * (lo + hi);
                    const double f = mid - c - a * std::tanh(drive * mid) / drive;
                    (f > 0 ? hi : lo) = mid;
                }
                const double want = 0.5 * (lo + hi);
                const double got = Filter::solve_loop(c, a, drive);
                INFO("drive " << drive << " a " << a << " c " << c);
                CHECK(std::abs(got - want) <= 1e-9 * std::max(1.0, std::abs(c)));
            }
        }
    }
}

TEST_CASE("Korg35 recovers from a non-finite input", "[signal][korg35][robustness]") {
    Filter f;
    f.configure(Mode::lowpass, kFs, 1000.0, 0.9, 1.0);
    for (int n = 0; n < 100; ++n)
        f.process(0.5);
    CHECK(f.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(f.process(std::numeric_limits<double>::infinity()) == 0.0);
    for (int n = 0; n < 100; ++n)
        REQUIRE(std::isfinite(f.process(0.5)));
}

TEST_CASE("Korg35 float and double agree on a moderate setting", "[signal][korg35][precision]") {
    pulp::signal::Korg35Filter single;
    Filter dbl;
    single.configure(pulp::signal::Korg35Filter::Mode::lowpass, 48000.0f, 1800.0f, 0.8f, 1.0f);
    dbl.configure(Mode::lowpass, kFs, 1800.0, 0.8, 1.0);
    double worst = 0.0;
    for (int n = 0; n < 8000; ++n) {
        const double x = 0.5 * std::sin(0.03 * n) + 0.2 * std::sin(0.31 * n);
        worst = std::max(worst, std::abs(single.process(static_cast<float>(x)) - dbl.process(x)));
    }
    CHECK(worst < 1e-3);
}
