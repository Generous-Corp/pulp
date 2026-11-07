// Diode-ladder lowpass: response against an independently solved analog
// prototype, the self-oscillation threshold, stability, the feedback solver,
// and agreement with the transistor ladder at low resonance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/diode_ladder_filter.hpp>
#include <pulp/signal/ladder_filter.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Filter = pulp::signal::DiodeLadderFilter64;
using cplx = std::complex<double>;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kWosc = Filter::kOscillationOmega;

// Open-loop response y4/u of the coupled chain at normalized frequency w,
// solved by complex Gaussian elimination (independent of the filter code).
cplx open_loop(double w) {
    const double a[4][4] = {{-2, 1, 0, 0}, {1, -2, 1, 0}, {0, 1, -2, 1}, {0, 0, 2, -2}};
    cplx m[4][5];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            m[i][j] = (i == j ? cplx(0, w) : cplx(0)) - a[i][j];
        m[i][4] = i == 0 ? 1.0 : 0.0;
    }
    for (int i = 0; i < 4; ++i) {
        for (int r = 0; r < 4; ++r) {
            if (r == i)
                continue;
            const cplx f = m[r][i] / m[i][i];
            for (int j = 0; j < 5; ++j)
                m[r][j] -= f * m[i][j];
        }
    }
    return m[3][4] / m[3][3];
}

double closed_loop_gain(double k, double w) {
    const cplx h = open_loop(w);
    return std::abs(h / (1.0 + k * h));
}

double normalized_omega(double hz, double cutoff) {
    return kWosc * std::tan(kPi * hz / kFs) / std::tan(kPi * cutoff / kFs);
}

template <typename Process> double sine_gain(Process&& process, double hz, double amplitude) {
    const int settle = 30000, window = 4800;
    double sum = 0.0;
    for (int n = 0; n < settle + window; ++n) {
        const double y = process(amplitude * std::sin(2.0 * kPi * hz * n / kFs));
        if (n >= settle)
            sum += y * y;
    }
    return std::sqrt(2.0 * sum / window) / amplitude;
}

double filter_gain(double cutoff, double resonance, double drive, double hz, double amplitude = 1.0,
                   double compensation = 0.0) {
    Filter f;
    f.configure(kFs, cutoff, resonance, drive);
    f.set_bass_compensation(compensation);
    return sine_gain([&](double x) { return f.process(x); }, hz, amplitude);
}

double peak_abs(const std::vector<double>& v) {
    double p = 0.0;
    for (double x : v)
        p = std::max(p, std::abs(x));
    return p;
}

double zero_crossing_hz(const std::vector<double>& y) {
    int crossings = 0;
    for (std::size_t i = 1; i < y.size(); ++i)
        if ((y[i - 1] < 0.0) != (y[i] < 0.0))
            ++crossings;
    return kFs * 0.5 * crossings / static_cast<double>(y.size());
}

} // namespace

TEST_CASE("Diode ladder unresonant corner sits at the documented fraction of the cutoff",
          "[signal][diode-ladder][response]") {
    // Low cutoffs, where warping is negligible: the -3 dB corner is
    // kUnresonantCornerRatio times the set frequency.
    for (const double cutoff : {2000.0, 5000.0}) {
        const double corner = Filter::kUnresonantCornerRatio * cutoff;
        CHECK_THAT(filter_gain(cutoff, 0.0, 0.0, corner), WithinRel(1.0 / std::sqrt(2.0), 0.02));
    }
}

TEST_CASE("Diode ladder corner helper is exact at high cutoffs where the ratio drifts",
          "[signal][diode-ladder][response][corner]") {
    // The ratio rises with the cutoff because only the oscillation frequency is
    // prewarped. The documented figures at 48 kHz:
    const std::array<std::array<double, 2>, 6> documented{{{1000.0, 0.106},
                                                           {5000.0, 0.110},
                                                           {10000.0, 0.124},
                                                           {15000.0, 0.160},
                                                           {20000.0, 0.287},
                                                           {23520.0, 0.833}}};
    for (const auto& [cutoff, ratio] : documented) {
        INFO("cutoff " << cutoff);
        CHECK_THAT(Filter::corner_frequency_hz(kFs, cutoff) / cutoff, WithinAbs(ratio, 0.002));
    }
    // Measured: the filter is 3 dB down exactly at the helper's corner, and the
    // fixed ratio is visibly wrong there.
    for (const double cutoff : {10000.0, 15000.0, 20000.0}) {
        const double corner = Filter::corner_frequency_hz(kFs, cutoff);
        INFO("cutoff " << cutoff << " corner " << corner);
        CHECK_THAT(filter_gain(cutoff, 0.0, 0.0, corner), WithinRel(1.0 / std::sqrt(2.0), 0.01));
        CHECK(std::abs(filter_gain(cutoff, 0.0, 0.0, Filter::kUnresonantCornerRatio * cutoff) -
                       1.0 / std::sqrt(2.0)) > 0.03);
    }
    // The analog prototype agrees: |H| at the helper's corner, mapped back to
    // normalized frequency, is 1/sqrt(2).
    for (const double cutoff : {500.0, 12000.0, 22000.0}) {
        const double corner = Filter::corner_frequency_hz(kFs, cutoff);
        CHECK_THAT(closed_loop_gain(0.0, normalized_omega(corner, cutoff)),
                   WithinRel(1.0 / std::sqrt(2.0), 1e-9));
    }
    // Round trip, and the clamp.
    for (const double corner : {50.0, 1000.0, 4000.0, 12000.0, 19000.0}) {
        const double cutoff = Filter::frequency_for_corner_hz(kFs, corner);
        CHECK_THAT(Filter::corner_frequency_hz(kFs, cutoff), WithinRel(corner, 1e-9));
    }
    const double top = Filter::corner_frequency_hz(kFs, 0.49 * kFs);
    CHECK_THAT(top, WithinRel(19590.0, 2e-3));
    CHECK_THAT(Filter::frequency_for_corner_hz(kFs, 23000.0), WithinRel(0.49 * kFs, 1e-12));
}

TEST_CASE("Diode ladder corner reference makes set_frequency name the -3 dB corner",
          "[signal][diode-ladder][corner]") {
    for (const double corner : {300.0, 2500.0, 9000.0, 16000.0}) {
        Filter f;
        f.configure(kFs, corner, 0.0, 0.0);
        f.set_cutoff_reference(Filter::CutoffReference::corner);
        CHECK(f.cutoff_reference() == Filter::CutoffReference::corner);
        CHECK_THAT(f.oscillation_frequency_hz(),
                   WithinRel(Filter::frequency_for_corner_hz(kFs, corner), 1e-12));
        INFO("corner " << corner);
        CHECK_THAT(sine_gain([&](double x) { return f.process(x); }, corner, 1.0),
                   WithinRel(1.0 / std::sqrt(2.0), 0.01));
    }
    // The corner is kept through a sample-rate change.
    Filter f;
    f.configure(kFs, 4000.0, 0.0, 0.0);
    f.set_cutoff_reference(Filter::CutoffReference::corner);
    f.set_sample_rate(96000.0);
    CHECK_THAT(Filter::corner_frequency_hz(96000.0, f.oscillation_frequency_hz()),
               WithinRel(4000.0, 1e-9));
    // Back to the default reference.
    f.set_cutoff_reference(Filter::CutoffReference::oscillation);
    CHECK_THAT(f.oscillation_frequency_hz(), WithinRel(4000.0, 1e-12));
}

TEST_CASE("Diode ladder resonance changes update the feedback without changing the matrix",
          "[signal][diode-ladder][modulation]") {
    // Per-sample resonance, drive and compensation changes through the cheap
    // setters must give exactly what a full reconfigure gives.
    Filter cheap, full;
    cheap.configure(kFs, 3000.0, 0.2, 1.0);
    for (int n = 0; n < 4000; ++n) {
        const double r = 0.6 + 0.55 * std::sin(0.003 * n);
        const double d = 1.0 + 0.5 * std::sin(0.001 * n);
        const double comp = 0.5 + 0.5 * std::sin(0.002 * n);
        cheap.set_resonance(r);
        cheap.set_drive(d);
        cheap.set_bass_compensation(comp);
        full.configure(kFs, 3000.0, r, d);
        full.set_bass_compensation(comp);
        const double x = std::sin(0.05 * n) + 0.3 * std::sin(0.9 * n);
        REQUIRE(cheap.process(x) == full.process(x));
    }
}

TEST_CASE("Diode ladder response matches the analytic prototype",
          "[signal][diode-ladder][response]") {
    const double cutoff = 2500.0;
    for (const double resonance : {0.0, 0.3, 0.7, 0.9}) {
        const double k = Filter::kSelfOscillationFeedback * resonance;
        for (const double hz : {100.0, 800.0, 1800.0, 2500.0, 4000.0, 9000.0}) {
            const double want = closed_loop_gain(k, normalized_omega(hz, cutoff));
            const double got = filter_gain(cutoff, resonance, 0.0, hz);
            INFO("r " << resonance << " hz " << hz);
            CHECK_THAT(got, WithinRel(want, 0.012));
        }
    }
}

TEST_CASE("Diode ladder passband falls as 1/(1+k) and bass compensation restores it",
          "[signal][diode-ladder][gain]") {
    for (const double resonance : {0.0, 0.4, 0.8}) {
        const double k = 17.0 * resonance;
        CHECK_THAT(filter_gain(6000.0, resonance, 0.0, 30.0), WithinRel(1.0 / (1.0 + k), 0.01));
        CHECK_THAT(filter_gain(6000.0, resonance, 0.0, 30.0, 1.0, 1.0), WithinRel(1.0, 0.01));
    }
}

TEST_CASE("Diode ladder rolls off at 24 dB per octave far above the cutoff",
          "[signal][diode-ladder][slope]") {
    const double g1 = filter_gain(100.0, 0.0, 0.0, 1600.0);
    const double g2 = filter_gain(100.0, 0.0, 0.0, 3200.0);
    const double octave_db = 20.0 * std::log10(g1 / g2);
    // Fourth-order: 24 dB / octave well above the highest pole.
    CHECK(octave_db > 23.0);
    CHECK(octave_db < 25.0);
}

TEST_CASE("Diode ladder resonance peak lies near the analytic peak",
          "[signal][diode-ladder][peak]") {
    const double cutoff = 1500.0;
    const double resonance = 0.95;
    const double k = 17.0 * resonance;
    double best_hz = 0.0, best = 0.0, best_model_hz = 0.0, best_model = 0.0;
    for (double hz = 500.0; hz <= 3000.0; hz += 10.0) {
        const double g = filter_gain(cutoff, resonance, 0.0, hz);
        if (g > best) {
            best = g;
            best_hz = hz;
        }
        const double m = closed_loop_gain(k, normalized_omega(hz, cutoff));
        if (m > best_model) {
            best_model = m;
            best_model_hz = hz;
        }
    }
    CHECK(std::abs(best_hz - best_model_hz) <= 30.0);
    CHECK(std::abs(best_hz - cutoff) / cutoff < 0.1);
    CHECK_THAT(best, WithinRel(best_model, 0.03));
    // A genuine resonant peak well above the (compressed) passband.
    CHECK(best > 3.0 * filter_gain(cutoff, resonance, 0.0, 60.0));
}

TEST_CASE("Diode ladder self-oscillation threshold is a feedback of 17",
          "[signal][diode-ladder][self-oscillation]") {
    const double cutoff = 1200.0;
    // The loop crosses -1 where the open-loop phase reaches 180 degrees, which
    // the filter's cutoff parameter is defined to be.
    // The open-loop phase is -180 degrees at sqrt(2) with gain 1/17.
    const double w_osc = kWosc;
    CHECK_THAT(std::abs(open_loop(w_osc).imag()), WithinAbs(0.0, 1e-12));
    CHECK_THAT(1.0 / -open_loop(w_osc).real(), WithinRel(17.0, 1e-9));
    const double osc_hz = cutoff;

    auto tail = [&](double resonance) {
        Filter f;
        f.configure(kFs, cutoff, resonance, 1.0);
        std::vector<double> out;
        for (int n = 0; n < 60000 + 16384; ++n) {
            const double y = f.process(n == 0 ? 1.0 : 0.0);
            if (n >= 60000)
                out.push_back(y);
        }
        return out;
    };
    CHECK(peak_abs(tail(0.96)) < 1e-4); // k = 16.3: dies away
    const auto ring = tail(1.08);       // k = 18.4: sustained
    CHECK(peak_abs(ring) > 0.02);
    CHECK(peak_abs(ring) < 10.0);
    CHECK(std::abs(zero_crossing_hz(ring) - osc_hz) / osc_hz < 0.03);
}

TEST_CASE("Diode ladder stays finite and bounded across the parameter space",
          "[signal][diode-ladder][stability]") {
    std::uint32_t rng = 777u;
    auto noise = [&] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<double>(rng >> 8) / 8388608.0 - 1.0;
    };
    for (const double drive : {0.0, 1.0, 8.0}) {
        for (const double cutoff : {20.0, 400.0, 5000.0, 15000.0, 23500.0}) {
            for (const double resonance : {0.0, 0.5, 0.99, 1.0, 1.25, 7.0}) {
                Filter f;
                f.configure(kFs, cutoff, resonance, drive);
                double worst = 0.0;
                for (int n = 0; n < 6000; ++n) {
                    const double y = f.process(noise());
                    REQUIRE(std::isfinite(y));
                    worst = std::max(worst, std::abs(y));
                }
                INFO("drive " << drive << " fc " << cutoff << " r " << resonance);
                CHECK(worst < 1.0e3);
            }
        }
    }
}

TEST_CASE("Diode ladder cutoff means the same as on the transistor ladder and the Svf",
          "[signal][diode-ladder][convention]") {
    // On LadderFilter and Svf the cutoff is the characteristic frequency: the
    // resonance peak sits there and the filter self-oscillates there. At the
    // same set_frequency and a resonance below the threshold, the small-signal
    // resonance peak of the diode ladder lands in the same place.
    auto peak_hz = [&](auto make) {
        double best = 0.0, best_hz = 0.0;
        for (double hz = 150.0; hz < 0.4 * kFs; hz *= 1.02) {
            auto f = make();
            const double g = sine_gain([&](double x) { return f.process(x); }, hz, 1e-4);
            if (g > best) {
                best = g;
                best_hz = hz;
            }
        }
        return best_hz;
    };
    for (const double cutoff : {500.0, 1500.0}) {
        const double diode_hz = peak_hz([&] {
            Filter f;
            f.configure(kFs, cutoff, 0.9, 0.0);
            return f;
        });
        const double ladder_hz = peak_hz([&] {
            struct Wrap {
                pulp::signal::LadderFilter64 l;
                double process(double x) {
                    return l.process_reference(x);
                }
            } w;
            w.l.set_sample_rate(kFs);
            w.l.set_frequency(cutoff);
            w.l.set_resonance(0.9);
            return w;
        });
        INFO("cutoff " << cutoff << " diode peak " << diode_hz << " ladder peak " << ladder_hz);
        CHECK(std::abs(diode_hz - cutoff) / cutoff < 0.05);
        CHECK(std::abs(ladder_hz - cutoff) / cutoff < 0.05);
        CHECK(std::abs(diode_hz - ladder_hz) / cutoff < 0.06);
    }
}

TEST_CASE("Diode ladder feedback solver matches bisection", "[signal][diode-ladder][solver]") {
    for (const double drive : {0.25, 1.0, 4.0, 16.0}) {
        for (const double ck : {0.0, 0.5, 4.0, 21.0}) {
            for (const double d : {-300.0, -20.0, -1.0, -0.01, 0.0, 0.3, 2.0, 25.0, 400.0}) {
                double lo = d - ck / drive - 1.0, hi = d + ck / drive + 1.0;
                for (int i = 0; i < 200; ++i) {
                    const double mid = 0.5 * (lo + hi);
                    const double f = mid + ck * std::tanh(drive * mid) / drive - d;
                    (f > 0 ? hi : lo) = mid;
                }
                const double want = 0.5 * (lo + hi);
                const double got = Filter::solve_loop(d, ck, drive);
                INFO("drive " << drive << " ck " << ck << " d " << d);
                CHECK_THAT(got, WithinAbs(want, 1e-9 * std::max(1.0, std::abs(d))));
            }
        }
    }
}

TEST_CASE("Diode ladder agrees with the transistor ladder below the cutoff at low resonance",
          "[signal][diode-ladder][parity]") {
    // Parity contract: with the two cutoffs set to the same -3 dB frequency and
    // no resonance, the small-signal magnitudes agree within 0.25 dB from DC up
    // to that frequency. Above it the diode ladder's spread poles give a gentler
    // knee, so no agreement is claimed there.
    const double f3 = 1000.0;
    const double amplitude = 1e-3; // small signal: both saturators are linear

    // Find the transistor-ladder cutoff whose measured -3 dB point is f3.
    auto ladder_gain = [&](double ladder_cutoff, double hz) {
        pulp::signal::LadderFilter64 ladder;
        ladder.set_sample_rate(kFs);
        ladder.set_frequency(ladder_cutoff);
        ladder.set_resonance(0.0);
        return sine_gain([&](double x) { return ladder.process_reference(x); }, hz, amplitude);
    };
    double lo = 0.2 * f3, hi = 5.0 * f3;
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (ladder_gain(mid, f3) > 1.0 / std::sqrt(2.0) ? hi : lo) = mid;
    }
    const double ladder_cutoff = 0.5 * (lo + hi);

    // And the diode-ladder cutoff whose measured -3 dB point is f3.
    auto diode_cutoff_for = [&](double corner) {
        double dlo = corner, dhi = 40.0 * corner;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (dlo + dhi);
            (filter_gain(mid, 0.0, 1.0, corner, amplitude) > 1.0 / std::sqrt(2.0) ? dhi : dlo) =
                mid;
        }
        return 0.5 * (dlo + dhi);
    };
    const double diode_cutoff = diode_cutoff_for(f3);

    double worst_db = 0.0;
    for (const double hz : {50.0, 150.0, 300.0, 500.0, 700.0, 850.0, 1000.0}) {
        const double diode = filter_gain(diode_cutoff, 0.0, 1.0, hz, amplitude);
        const double ladder = ladder_gain(ladder_cutoff, hz);
        worst_db = std::max(worst_db, std::abs(20.0 * std::log10(diode / ladder)));
    }
    CHECK(worst_db < 0.25);

    // Negative control: a 15 percent cutoff mismatch breaks the same check, so
    // the tolerance is tight enough to mean something.
    double mismatched_db = 0.0;
    for (const double hz : {300.0, 500.0, 700.0, 850.0}) {
        const double diode = filter_gain(1.15 * diode_cutoff, 0.0, 1.0, hz, amplitude);
        const double ladder = ladder_gain(ladder_cutoff, hz);
        mismatched_db = std::max(mismatched_db, std::abs(20.0 * std::log10(diode / ladder)));
    }
    CHECK(mismatched_db > 0.25);
}

TEST_CASE("Diode ladder recovers from a non-finite input", "[signal][diode-ladder][robustness]") {
    Filter f;
    f.configure(kFs, 1000.0, 0.9, 1.0);
    for (int n = 0; n < 100; ++n)
        f.process(0.5);
    CHECK(f.process(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(f.process(std::numeric_limits<double>::infinity()) == 0.0);
    for (int n = 0; n < 100; ++n)
        REQUIRE(std::isfinite(f.process(0.5)));
}

TEST_CASE("Diode ladder survives per-sample parameter changes and float precision",
          "[signal][diode-ladder][modulation]") {
    pulp::signal::DiodeLadderFilter f;
    f.configure(48000.0f, 800.0f, 0.5f, 1.0f);
    for (int n = 0; n < 48000; ++n) {
        const float phase = 2.0f * 3.14159265f * 2.0f * static_cast<float>(n) / 48000.0f;
        f.set_frequency(100.0f + 9000.0f * (0.5f + 0.5f * std::sin(phase)));
        f.set_resonance(0.6f + 0.6f * std::sin(0.41f * phase));
        const float y = f.process(std::sin(0.07f * static_cast<float>(n)));
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 100.0f);
    }
}

TEST_CASE("Diode ladder processing allocates nothing", "[signal][diode-ladder][rt-safety]") {
    Filter f;
    f.configure(kFs, 1200.0, 1.1, 2.0);
    double sink = 0.0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int n = 0; n < 20000; ++n) {
            if (n % 64 == 0) {
                f.set_frequency(400.0 + 30.0 * (n % 300));
                f.set_resonance(0.3 + 0.002 * (n % 400));
            }
            sink += f.process(std::sin(0.03 * n));
        }
        CHECK(probe.allocation_count() == 0);
    }
    CHECK(std::isfinite(sink));
}
