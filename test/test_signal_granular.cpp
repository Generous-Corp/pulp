// test_signal_granular.cpp — acceptance suite for the granular engine.
//
// Two things in this module are easy to assert and hard to actually test, and
// the suite is organised around both.
//
// The first is determinism, and it has two independent halves that are easy to
// conflate.
//
// Block-size independence is a property of the SCHEDULER: onsets are driven by
// an absolute sample clock, so where the block boundaries fall cannot matter.
// It is worth testing, but it does not test the draw mechanism at all — a
// sequential generator advanced once per spawn passes every block-size check in
// this file, which was verified by building exactly that and watching the suite
// stay green.
//
// What the stateless keyed draw actually buys is that a grain's parameters
// depend on its own index and on nothing else — not on which pool slot it
// landed in, not on how big the pool is, and not on whether earlier grains were
// dropped. A sequential generator fails that the moment a grain is dropped,
// because the draws it would have consumed never happen and every later grain
// shifts. That is the discriminating test, and it is "Grain draws depend only
// on the grain index" below.
//
// The second is the √N energy law. Its premise is that grains are UNCORRELATED,
// and grains are only uncorrelated if something separates the source positions
// they read. With the playhead at realtime, unity ratio, and no spray, every
// live grain reads the same source sample at the same instant: they are
// perfectly coherent, and the incoherent law is then the wrong law by 8.24 dB.
// The suite asserts the invariance the law promises AND the +3 dB failure of
// the configuration that omits spray, so the reason the one is configured the
// way it is stays recorded next to it.
//
// Expected values are computed from shipped constants throughout: window mean
// and RMS come off the engine's own table, the per-grain gain comes from
// `grain_gain()`, and the worst-case bound comes from a scan of the shipped
// interpolator.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/fft.hpp>
#include <pulp/signal/granular.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Approx;
using namespace pulp::signal;

namespace {

// ── Measurement-recipe constants (acceptance class) ───────────────────────
constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kFs = 48000.0;

/// Welch segment length for the pitch measurements, and the render length fed
/// to it. A single periodogram of a random-phase grain cloud is a poor estimate
/// of where its energy sits; averaging over the ~45 half-overlapping segments
/// an 8 s render provides recovers the expected spectrum, which is the thing
/// the transposition claim is actually about.
constexpr int kSpectrumSize = 16384;
constexpr int kSpectrumRender = 384000;  // 8 s

/// Position spray used by every measurement that needs grains to be mutually
/// uncorrelated. 30 ms at a 400 Hz density separates grain source positions by
/// far more than the correlation length of the material under test.
constexpr double kDecorrelationSprayMs = 30.0;

std::vector<double> sine_buffer(int count, double hz) {
    std::vector<double> out(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = std::sin(kTwoPi * hz * static_cast<double>(i) / kFs);
    }
    return out;
}

std::vector<double> noise_buffer(int count, std::uint32_t seed) {
    Xorshift32 rng(seed);
    std::vector<double> out(static_cast<std::size_t>(count));
    for (auto& v : out) v = rng.next_bipolar<double>();
    return out;
}

double rms(const std::vector<double>& v, int from = 0) {
    double sum = 0.0;
    for (std::size_t i = static_cast<std::size_t>(from); i < v.size(); ++i) sum += v[i] * v[i];
    return std::sqrt(sum / static_cast<double>(v.size() - static_cast<std::size_t>(from)));
}

/// Welch-averaged power spectrum.
///
/// Averaging is load-bearing, and so is NOT reading the answer off an argmax.
/// A grain cloud's spectrum is the source line convolved with the grain
/// window's main lobe — 80 Hz wide for a 50 ms Hann grain — and inside that
/// lobe neighbouring bins are near-tied, so the largest-bin index flips by
/// several bins under a last-bit change in the arithmetic. Band power and a
/// band centroid are stable where the argmax is not; an earlier version of this
/// suite believed an argmax and drew a confident, wrong conclusion from it.
std::vector<double> welch_power(const std::vector<double>& x, int nfft = kSpectrumSize) {
    FftT<double> fft(nfft);
    std::vector<double> window(static_cast<std::size_t>(nfft));
    for (int i = 0; i < nfft; ++i) {
        window[static_cast<std::size_t>(i)] =
            0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(nfft)));
    }
    std::vector<double> segment(static_cast<std::size_t>(nfft));
    std::vector<std::complex<double>> spectrum(static_cast<std::size_t>(nfft / 2 + 1));
    std::vector<double> power(static_cast<std::size_t>(nfft / 2 + 1), 0.0);

    for (int offset = 0; offset + nfft <= static_cast<int>(x.size()); offset += nfft / 2) {
        for (int i = 0; i < nfft; ++i) {
            segment[static_cast<std::size_t>(i)] =
                x[static_cast<std::size_t>(offset + i)] * window[static_cast<std::size_t>(i)];
        }
        fft.forward_real(segment.data(), spectrum.data());
        for (int k = 0; k <= nfft / 2; ++k) {
            power[static_cast<std::size_t>(k)] += std::norm(spectrum[static_cast<std::size_t>(k)]);
        }
    }
    return power;
}

double bin_hz(int k, int nfft = kSpectrumSize) {
    return static_cast<double>(k) * kFs / static_cast<double>(nfft);
}

/// Power in [lo, hi] as a fraction of the power over the whole audio band.
double band_fraction(const std::vector<double>& power, double lo, double hi,
                     int nfft = kSpectrumSize) {
    double inside = 0.0;
    double total = 0.0;
    for (int k = 1; k < static_cast<int>(power.size()); ++k) {
        const double hz = bin_hz(k, nfft);
        if (hz < 20.0 || hz > 20000.0) continue;
        total += power[static_cast<std::size_t>(k)];
        if (hz >= lo && hz <= hi) inside += power[static_cast<std::size_t>(k)];
    }
    return total > 0.0 ? inside / total : 0.0;
}

/// Power-weighted centre of a band — stable to a tenth of a percent where the
/// largest-bin index is not stable at all.
double band_centroid(const std::vector<double>& power, double lo, double hi,
                     int nfft = kSpectrumSize) {
    double weighted = 0.0;
    double total = 0.0;
    for (int k = 1; k < static_cast<int>(power.size()); ++k) {
        const double hz = bin_hz(k, nfft);
        if (hz < lo || hz > hi) continue;
        weighted += power[static_cast<std::size_t>(k)] * hz;
        total += power[static_cast<std::size_t>(k)];
    }
    return total > 0.0 ? weighted / total : 0.0;
}

/// Sets up a granulator on a borrowed buffer with everything the pitch and
/// level measurements need in common.
void configure_buffer_engine(GranularEngine64& engine, const std::vector<double>& source) {
    engine.prepare(kFs);
    engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(400.0);
    engine.set_position_spray_ms(kDecorrelationSprayMs);
    engine.reset();
}

struct Stereo {
    std::vector<double> left;
    std::vector<double> right;
};

Stereo render(GranularEngine64& engine, int n, int block = 0) {
    Stereo out{std::vector<double>(static_cast<std::size_t>(n)),
               std::vector<double>(static_cast<std::size_t>(n))};
    if (block <= 0) {
        engine.process(out.left.data(), out.right.data(), n);
        return out;
    }
    for (int i = 0; i < n; i += block) {
        const int count = std::min(block, n - i);
        engine.process(out.left.data() + i, out.right.data() + i, count);
    }
    return out;
}

/// The four Catmull-Rom basis weights the shipped interpolator produces at a
/// fractional offset. Used to derive the worst-case sample gain and the
/// variance a fractional read of white noise keeps — both from shipped code
/// rather than from restated coefficients.
void hermite_basis(double t, double (&h)[4]) {
    h[0] = Interpolator::hermite(t, 1.0, 0.0, 0.0, 0.0);
    h[1] = Interpolator::hermite(t, 0.0, 1.0, 0.0, 0.0);
    h[2] = Interpolator::hermite(t, 0.0, 0.0, 1.0, 0.0);
    h[3] = Interpolator::hermite(t, 0.0, 0.0, 0.0, 1.0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Ground truth, before anything is measured against it.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Window table reproduces the exact Hann identities", "[granular][window]") {
    // The normalization law is written in terms of the window's mean and RMS,
    // so those two numbers are the module's foundation. At full cosine taper
    // they are exactly 1/2 and sqrt(3/8): the discrete sums of sin^2 and sin^4
    // over a whole period are exact for any table size above 4, so this is an
    // identity check and not a tolerance check.
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_window_taper(1.0);
    CHECK(engine.window_mean() == Approx(0.5).margin(1e-12));
    CHECK(engine.window_rms() == Approx(std::sqrt(3.0 / 8.0)).margin(1e-12));

    engine.set_window_taper(0.0);
    CHECK(engine.window_mean() == Approx(1.0).margin(1e-12));
    CHECK(engine.window_rms() == Approx(1.0).margin(1e-12));

    // A Tukey between the two sits between the two, monotonically.
    double previous_mean = 1.0;
    for (double taper : {0.25, 0.5, 0.75, 1.0}) {
        engine.set_window_taper(taper);
        CHECK(engine.window_mean() < previous_mean);
        CHECK(engine.window_rms() > engine.window_mean());
        previous_mean = engine.window_mean();
    }
}

TEST_CASE("Composed interpolator is the specified Catmull-Rom", "[granular][interpolator]") {
    // The spec writes the four cubic coefficients out. The shared interpolator
    // already implements them, so the right move is to compose it and prove the
    // composition rather than paste a second copy into the engine.
    Xorshift32 rng(4242u);
    for (int trial = 0; trial < 200; ++trial) {
        const double t = rng.next_unit<double>();
        const double ym1 = rng.next_bipolar<double>();
        const double y0 = rng.next_bipolar<double>();
        const double y1 = rng.next_bipolar<double>();
        const double y2 = rng.next_bipolar<double>();

        const double a = -0.5 * ym1 + 1.5 * y0 - 1.5 * y1 + 0.5 * y2;
        const double b = ym1 - 2.5 * y0 + 2.0 * y1 - 0.5 * y2;
        const double c = -0.5 * ym1 + 0.5 * y1;
        const double expected = ((a * t + b) * t + c) * t + y0;

        CHECK(Interpolator::hermite(t, ym1, y0, y1, y2) == Approx(expected).margin(1e-14));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-2 — window fidelity, in the table and in the audio.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Grain window matches its closed form", "[granular][window]") {
    GranularEngine64 engine;
    engine.prepare(kFs);

    SECTION("Hann") {
        engine.set_window_taper(1.0);
        double worst = 0.0;
        for (int i = 0; i <= 200000; ++i) {
            const double p = static_cast<double>(i) / 200000.0;
            const double truth = std::sin(kPi * p) * std::sin(kPi * p);
            worst = std::max(worst, std::abs(engine.window_at(p) - truth));
        }
        CHECK(worst < 1e-6);

        // The residual is entirely the table's linear interpolation, whose
        // error is |w''|·h²/8 with |w''| = 2π². Asserting the measured error
        // against that bound rather than only against 1e-6 is what turns "it
        // passed" into "the table is the only error source" — and it documents
        // that the margin under the spec's 1e-6 is only 1.7x at the shipped
        // table size.
        const double table = static_cast<double>(GranularEngine64::kWindowTableSize);
        const double bound = 2.0 * kPi * kPi / (8.0 * table * table);
        CHECK(worst == Approx(bound).epsilon(0.05));
    }

    SECTION("rectangular") {
        engine.set_window_taper(0.0);
        for (int i = 0; i <= 1000; ++i) {
            const double p = static_cast<double>(i) / 1000.0;
            CHECK(engine.window_at(p) == Approx(1.0).margin(1e-12));
        }
    }

    SECTION("trapezoid") {
        engine.set_window_trapezoid(true);
        engine.set_window_taper(1.0);  // degenerates to a triangle
        double worst = 0.0;
        for (int i = 0; i <= 200000; ++i) {
            const double p = static_cast<double>(i) / 200000.0;
            const double truth = p < 0.5 ? 2.0 * p : 2.0 * (1.0 - p);
            worst = std::max(worst, std::abs(engine.window_at(p) - truth));
        }
        // Piecewise linear through table nodes, and the breakpoint at p = 0.5
        // lands exactly on a node, so this is exact rather than bounded.
        CHECK(worst < 1e-12);
    }

    SECTION("slope bound") {
        // The bound T-7 leans on: dw/dp = π·sin(2πp) for Hann, so no grain's
        // contribution can step by more than π·phase_inc between samples. That
        // is what "the window has no discontinuity" means numerically.
        engine.set_window_taper(1.0);
        const double phase_increment = 1.0 / (0.050 * kFs);
        double worst = 0.0;
        for (double p = 0.0; p + phase_increment <= 1.0; p += phase_increment) {
            worst = std::max(worst, std::abs(engine.window_at(p + phase_increment) -
                                             engine.window_at(p)));
        }
        CHECK(worst <= kPi * phase_increment * 1.01);
    }
}

TEST_CASE("A single grain plays exactly its window", "[granular][window]") {
    // The audio path, not the table: a DC source and one non-overlapping grain
    // make the output literally the window times the per-grain gain times the
    // centre pan gain — every factor predicted from shipped constants.
    const std::vector<double> dc(48000, 1.0);
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(4.0);  // one grain per 250 ms; 50 ms grains never overlap
    engine.set_stretch(0.0);
    engine.reset();

    // Mean overlap is 0.2, so the incoherent gain is above 1 and the clamp
    // pins it — which is the clamp's whole purpose.
    REQUIRE(engine.mean_overlap() == Approx(0.2));
    REQUIRE(engine.grain_gain() == Approx(1.0));

    const auto out = render(engine, 2400);
    const double pan = std::cos(kPi / 4.0);
    double worst = 0.0;
    for (int n = 0; n < 2400; ++n) {
        const double p = static_cast<double>(n) / 2400.0;
        const double expected =
            std::sin(kPi * p) * std::sin(kPi * p) * engine.grain_gain() * pan;
        worst = std::max(worst, std::abs(out.left[static_cast<std::size_t>(n)] - expected));
    }
    CHECK(worst < 1e-6);
    CHECK(engine.active_grain_count() == 1);
}

// ─────────────────────────────────────────────────────────────────────────
// T-1 — the scheduler.
// ─────────────────────────────────────────────────────────────────────────

namespace {

/// Onset sample indices over a render, recovered by stepping one sample at a
/// time and watching the monotonic grain counter.
std::vector<int> onset_indices(GranularEngine64& engine, int samples) {
    std::vector<int> onsets;
    std::uint64_t previous = engine.grain_index();
    double left = 0.0;
    double right = 0.0;
    for (int n = 0; n < samples; ++n) {
        engine.process(&left, &right, 1);
        if (engine.grain_index() != previous) {
            onsets.push_back(n);
            previous = engine.grain_index();
        }
    }
    return onsets;
}

double interval_cv(const std::vector<int>& onsets) {
    if (onsets.size() < 3) return 0.0;
    std::vector<double> intervals;
    for (std::size_t i = 1; i < onsets.size(); ++i) {
        intervals.push_back(static_cast<double>(onsets[i] - onsets[i - 1]));
    }
    double mean = 0.0;
    for (double v : intervals) mean += v;
    mean /= static_cast<double>(intervals.size());
    double variance = 0.0;
    for (double v : intervals) variance += (v - mean) * (v - mean);
    variance /= static_cast<double>(intervals.size());
    return std::sqrt(variance) / mean;
}

}  // namespace

TEST_CASE("Synchronous grain rate is exact over the long run", "[granular][schedule]") {
    for (double density : {100.0, 97.0}) {
        GranularEngine64 engine;
        engine.prepare(kFs);
        engine.set_density_hz(density);
        engine.set_async_jitter(0.0);
        engine.reset();

        const auto out = render(engine, 480000);  // 10 s
        (void)out;
        const auto expected = static_cast<std::uint64_t>(std::llround(density * 10.0));
        // The onset accumulator carries its fractional remainder, so a density
        // whose period is not a whole number of samples still lands on the
        // exact long-run count.
        CHECK(engine.grain_index() == expected);
    }

    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_density_hz(100.0);
    engine.set_async_jitter(0.0);
    engine.reset();
    const auto onsets = onset_indices(engine, 480000);
    CHECK(interval_cv(onsets) == Approx(0.0).margin(1e-12));
}

TEST_CASE("Full jitter is a Poisson process and zero jitter is a clock",
          "[granular][schedule]") {
    // The two named endpoints, measured. Inter-onset intervals of a Poisson
    // process are exponential, whose coefficient of variation is exactly 1;
    // a fixed clock's is exactly 0. Anything that only dithers the grid
    // position lands at sqrt(1/6) = 0.408 instead and is not Poisson at all,
    // which is why the blend here is on the interval rather than the position.
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_density_hz(100.0);
    engine.set_async_jitter(1.0);
    engine.reset();

    const int samples = 2400000;  // 50 s, ~5000 onsets
    const auto onsets = onset_indices(engine, samples);
    CHECK(interval_cv(onsets) == Approx(1.0).margin(0.1));

    // Blending the interval keeps the mean interval at fs/D, so the long-run
    // rate is the requested density at every jitter setting.
    const double rate = static_cast<double>(onsets.size()) / (static_cast<double>(samples) / kFs);
    CHECK(rate == Approx(100.0).epsilon(0.05));
}

// ─────────────────────────────────────────────────────────────────────────
// T-3 / T-4 — pitch and time, decoupled.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Pitch follows the ratio and ignores the playhead", "[granular][pitch]") {
    // The decoupling claim, measured as a matrix rather than asserted. The
    // transposition must depend only on the semitone setting; the stretch
    // column must not move it.
    //
    // Measured as band power against the three candidate frequencies rather
    // than as a peak-bin index. The grain window smears every component across
    // an 80 Hz main lobe, and the largest bin inside that lobe is not a
    // repeatable quantity — it moves by several bins under a last-bit change in
    // the arithmetic. Band power is stable, and the answer it gives is
    // unambiguous: the correct band holds better than 99.9 % of the energy in
    // every cell below.
    const auto source = sine_buffer(48000, 1000.0);
    const std::vector<double> candidates{500.0, 1000.0, 2000.0};

    for (double semitones : {0.0, 12.0, -12.0}) {
        const double expected = 1000.0 * std::exp2(semitones / 12.0);
        for (double stretch : {1.0, 0.5}) {
            GranularEngine64 engine;
            configure_buffer_engine(engine, source);
            engine.set_pitch_semitones(semitones);
            engine.set_stretch(stretch);
            engine.reset();

            const auto out = render(engine, kSpectrumRender);
            const auto power = welch_power(out.left);

            for (double candidate : candidates) {
                const double fraction =
                    band_fraction(power, candidate - 60.0, candidate + 60.0);
                if (candidate == expected) {
                    CHECK(fraction > 0.99);
                } else {
                    CHECK(fraction < 0.01);
                }
            }
            // And the transposition is accurate, not merely in the right
            // octave: the spec's +/-1 %, measured on the band centroid.
            const double centroid =
                band_centroid(power, expected * 0.88, expected * 1.12);
            CHECK(centroid == Approx(expected).epsilon(0.01));
        }
    }
}

TEST_CASE("Phase-locked grains cancel a periodic source", "[granular][pitch]") {
    // Why every measurement above sprays, stated as the level fact it is.
    //
    // With no spray, consecutive grains read source positions exactly one hop
    // apart. At 400 grains/s into a 1 kHz tone that hop is 2.5 periods, so
    // neighbouring grains sit in antiphase and annihilate each other: the
    // output collapses by about 40 dB and what survives is numerical residue.
    // This is a property of granular synthesis on periodic material, not a
    // defect — but it means a spray-free configuration measures the residue
    // rather than the effect, and any spectral claim made about it is a claim
    // about nothing.
    //
    // The exception proves the mechanism: when the grain ratio equals the
    // playhead rate every grain reads the SAME source sample at the same
    // instant, so instead of cancelling they add coherently and the output is
    // loud.
    const auto source = sine_buffer(48000, 1000.0);

    auto level = [&source](double spray, double semitones, double stretch) {
        GranularEngine64 engine;
        configure_buffer_engine(engine, source);
        engine.set_position_spray_ms(spray);
        engine.set_pitch_semitones(semitones);
        engine.set_stretch(stretch);
        engine.reset();
        const auto out = render(engine, 96000);
        return rms(out.left, 4800);
    };

    const double cancelled = level(0.0, 12.0, 1.0);
    const double sprayed = level(kDecorrelationSprayMs, 12.0, 1.0);
    CHECK(20.0 * std::log10(cancelled / sprayed) < -30.0);

    // Ratio equal to the playhead rate: fully coherent, and louder than the
    // decorrelated cloud rather than quieter.
    const double coherent = level(0.0, 0.0, 1.0);
    CHECK(20.0 * std::log10(coherent / sprayed) > 6.0);
}

TEST_CASE("Stretch dilates the output timeline", "[granular][stretch]") {
    // A marker at source 1.0 s must arrive at output 2.0 s when the playhead
    // runs at half speed. The tolerance is one grain length because that is
    // genuinely how well the marker is localised: it is only reproduced by
    // whichever grains' windows cover it.
    std::vector<double> source(96000, 0.0);
    for (int i = 0; i < 240; ++i) source[static_cast<std::size_t>(48000 + i)] = 1.0;

    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(400.0);
    engine.set_stretch(0.5);
    engine.reset();

    const auto out = render(engine, 160000);

    const int window = 480;
    int peak = 0;
    double peak_energy = 0.0;
    for (int n = 0; n + window < 160000; ++n) {
        double energy = 0.0;
        for (int k = 0; k < window; ++k) {
            const double v = out.left[static_cast<std::size_t>(n + k)];
            energy += v * v;
        }
        if (energy > peak_energy) {
            peak_energy = energy;
            peak = n + window / 2;
        }
    }

    const double grain_samples = 0.050 * kFs;
    CHECK(std::abs(static_cast<double>(peak) - 96000.0) <= grain_samples);
}

// ─────────────────────────────────────────────────────────────────────────
// T-5 — determinism, including the part a sequential RNG would fail.
// ─────────────────────────────────────────────────────────────────────────

namespace {

void configure_stress(GranularEngine64& engine, const std::vector<double>& source,
                      GrainSource mode) {
    engine.prepare(kFs);
    engine.set_source(mode);
    engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
    engine.set_density_hz(600.0);
    engine.set_grain_ms(60.0);
    engine.set_async_jitter(0.7);
    engine.set_position_spray_ms(120.0);
    engine.set_pitch_spray_semitones(5.0);
    engine.set_pan_spray(1.0);
    engine.set_max_grains(32);  // guarantees stealing at this density
    engine.reset();
}

}  // namespace

TEST_CASE("Renders are bit-identical and block-size independent", "[granular][determinism]") {
    const auto source = noise_buffer(48000, 999u);
    const int n = 24000;

    SECTION("repeat and reset") {
        GranularEngine64 a;
        GranularEngine64 b;
        configure_stress(a, source, GrainSource::buffer);
        configure_stress(b, source, GrainSource::buffer);
        const auto first = render(a, n);
        const auto second = render(b, n);
        CHECK(first.left == second.left);
        CHECK(first.right == second.right);

        a.reset();
        const auto third = render(a, n);
        CHECK(first.left == third.left);
        CHECK(first.right == third.right);

        // And the stress config really does steal, or this proves nothing about
        // the keying surviving voice-steal.
        CHECK(a.steal_count() > 0);
    }

    SECTION("buffer mode block size") {
        GranularEngine64 big;
        GranularEngine64 small;
        configure_stress(big, source, GrainSource::buffer);
        configure_stress(small, source, GrainSource::buffer);
        const auto one_block = render(big, n);
        const auto per_sample = render(small, n, 1);
        CHECK(one_block.left == per_sample.left);
        CHECK(one_block.right == per_sample.right);
        CHECK(big.steal_count() == small.steal_count());
    }

    SECTION("live mode block size through the interleaved overload") {
        GranularEngine64 big;
        GranularEngine64 small;
        configure_stress(big, source, GrainSource::live_ring);
        configure_stress(small, source, GrainSource::live_ring);

        std::vector<double> big_left(static_cast<std::size_t>(n));
        std::vector<double> big_right(static_cast<std::size_t>(n));
        big.process(source.data(), big_left.data(), big_right.data(), n);

        std::vector<double> small_left(static_cast<std::size_t>(n));
        std::vector<double> small_right(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            small.process(source.data() + i, small_left.data() + i, small_right.data() + i, 1);
        }
        CHECK(big_left == small_left);
        CHECK(big_right == small_right);
    }

    SECTION("the write_live pair is deliberately not block-size independent") {
        // Documented, not a bug to fix in the engine: writing a whole block
        // before rendering any of it puts samples in the ring that a grain
        // rendered at the top of that block can legally read, and rendering
        // sample by sample does not. The interleaved overload above exists
        // precisely because this ambiguity is unresolvable from inside a
        // two-call API.
        GranularEngine64 big;
        GranularEngine64 small;
        configure_stress(big, source, GrainSource::live_ring);
        configure_stress(small, source, GrainSource::live_ring);

        std::vector<double> big_left(static_cast<std::size_t>(n));
        std::vector<double> big_right(static_cast<std::size_t>(n));
        big.write_live(source.data(), n);
        big.process(big_left.data(), big_right.data(), n);

        std::vector<double> small_left(static_cast<std::size_t>(n));
        std::vector<double> small_right(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            small.write_live(source.data() + i, 1);
            small.process(small_left.data() + i, small_right.data() + i, 1);
        }
        CHECK(big_left != small_left);
    }

    SECTION("the seed is actually used") {
        GranularEngine64 a;
        GranularEngine64 b;
        configure_stress(a, source, GrainSource::buffer);
        configure_stress(b, source, GrainSource::buffer);
        b.set_seed(0x12345678u);
        b.reset();
        const auto first = render(a, n);
        const auto second = render(b, n);
        CHECK(first.left != second.left);
    }
}

namespace {

/// Steps an engine one sample at a time and records, for every grain that is
/// actually spawned, the parameters it was born with, keyed by its monotonic
/// grain index.
///
/// A spawn is observable from outside: the grain counter advances by one (never
/// more, because the inter-onset interval has a one-sample floor), and the slot
/// that received the grain either turned active or had its phase reset. When
/// the counter advances and no slot changes, the grain was dropped.
std::vector<std::pair<std::uint64_t, double>> spawned_ratios(GranularEngine64& engine,
                                                             int samples) {
    std::vector<std::pair<std::uint64_t, double>> spawns;
    const int slots = engine.max_grains();
    std::vector<char> was_active(static_cast<std::size_t>(slots), 0);
    std::vector<double> was_phase(static_cast<std::size_t>(slots), 0.0);
    double left = 0.0;
    double right = 0.0;

    for (int n = 0; n < samples; ++n) {
        for (int s = 0; s < slots; ++s) {
            const auto view = engine.grain(s);
            was_active[static_cast<std::size_t>(s)] = view.active ? 1 : 0;
            was_phase[static_cast<std::size_t>(s)] = view.phase;
        }
        const std::uint64_t before = engine.grain_index();
        engine.process(&left, &right, 1);
        const std::uint64_t after = engine.grain_index();
        if (after == before) continue;

        for (int s = 0; s < slots; ++s) {
            const auto view = engine.grain(s);
            const bool fresh = view.active && !was_active[static_cast<std::size_t>(s)];
            const bool reused = view.active && was_active[static_cast<std::size_t>(s)] &&
                                view.phase < was_phase[static_cast<std::size_t>(s)];
            if (fresh || reused) {
                spawns.emplace_back(after - 1, view.ratio);
                break;
            }
        }
    }
    return spawns;
}

}  // namespace

TEST_CASE("Grain draws depend only on the grain index", "[granular][determinism]") {
    // The property the stateless keyed hash exists for, and the one a
    // sequential generator cannot provide: grain k is born with the same pitch
    // whether or not grains before it were dropped, and whatever the pool size.
    //
    // Two engines differing only in budget. The larger never runs out of slots;
    // the smaller drops grains constantly. Every grain index they have in common
    // must carry a bit-identical ratio. Under a sequential generator the
    // dropped grains' draws never happen and every later grain in the smaller
    // engine shifts to a different value.
    const auto source = noise_buffer(48000, 31u);

    auto build = [&source](int budget) {
        auto engine = std::make_unique<GranularEngine64>();
        engine->prepare(kFs);
        engine->set_buffer(source.data(), static_cast<int>(source.size()), 1);
        engine->set_grain_ms(50.0);
        engine->set_density_hz(800.0);  // mean overlap 40: fits 64 slots, not 32
        engine->set_pitch_spray_semitones(12.0);
        engine->set_steal_policy(StealPolicy::drop_new);
        engine->set_max_grains(budget);
        engine->reset();
        return engine;
    };

    auto roomy = build(GranularEngine64::kMaxGrainBudget);
    auto cramped = build(GranularEngine64::kMinGrainBudget);

    const auto roomy_spawns = spawned_ratios(*roomy, 48000);
    const auto cramped_spawns = spawned_ratios(*cramped, 48000);

    REQUIRE(roomy_spawns.size() > 500);
    // The cramped engine must actually be dropping, or this proves nothing.
    REQUIRE(cramped_spawns.size() < roomy_spawns.size() * 9 / 10);

    std::vector<double> by_index(roomy_spawns.back().first + 1, -1.0);
    for (const auto& [index, ratio] : roomy_spawns) {
        by_index[static_cast<std::size_t>(index)] = ratio;
    }

    int compared = 0;
    for (const auto& [index, ratio] : cramped_spawns) {
        if (index >= by_index.size()) continue;
        const double reference = by_index[static_cast<std::size_t>(index)];
        if (reference < 0.0) continue;
        CHECK(ratio == reference);
        ++compared;
    }
    CHECK(compared > 400);
}

// ─────────────────────────────────────────────────────────────────────────
// T-6 — the √N energy law.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Incoherent gain holds level across a density octave", "[granular][gain]") {
    const auto source = noise_buffer(96000, 12345u);
    const double sigma = rms(source);

    auto measure = [&source](double density, double spray) {
        GranularEngine64 engine;
        engine.prepare(kFs);
        engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
        engine.set_window_taper(1.0);
        engine.set_grain_ms(50.0);
        engine.set_density_hz(density);
        engine.set_position_spray_ms(spray);
        engine.set_coherence(Coherence::incoherent);
        engine.reset();
        const auto out = render(engine, 96000);
        return rms(out.left, 4800);
    };

    SECTION("with grains decorrelated the law holds") {
        const double low = measure(200.0, 200.0);
        const double high = measure(400.0, 200.0);
        CHECK(20.0 * std::log10(high / low) == Approx(0.0).margin(0.5));

        // Absolute level, predicted end to end from shipped constants:
        // E[y²] = N̄·g²·w_rms²·σ² and g = 1/(w_rms·√N̄) collapse to σ², then the
        // centre pan takes 1/√2 and a fractional read of white noise keeps only
        // the interpolator's mean sum-of-squares — which is computed here from
        // the shipped kernel, not quoted.
        double noise_gain = 0.0;
        const int steps = 20000;
        for (int i = 0; i < steps; ++i) {
            double h[4];
            hermite_basis(static_cast<double>(i) / steps, h);
            noise_gain += h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + h[3] * h[3];
        }
        noise_gain /= steps;

        const double predicted = sigma * std::sqrt(noise_gain) * std::cos(kPi / 4.0);
        CHECK(20.0 * std::log10(low / predicted) == Approx(0.0).margin(0.5));
    }

    SECTION("without spray the grains are coherent and the law does not apply") {
        // The spec's T-6 configuration. Every grain reads the same source
        // sample at the same instant, so amplitudes add and doubling the
        // density adds 3 dB instead of nothing. Asserted so the requirement
        // that grains be decorrelated is recorded rather than remembered.
        const double low = measure(200.0, 0.0);
        const double high = measure(400.0, 0.0);
        CHECK(20.0 * std::log10(high / low) == Approx(3.01).margin(0.3));
    }
}

TEST_CASE("Coherent and incoherent gains differ by the derived amount",
          "[granular][gain]") {
    GranularEngine64 incoherent;
    GranularEngine64 coherent;
    for (auto* engine : {&incoherent, &coherent}) {
        engine->prepare(kFs);
        engine->set_window_taper(1.0);
        engine->set_grain_ms(50.0);
        engine->set_density_hz(200.0);
    }
    incoherent.set_coherence(Coherence::incoherent);
    coherent.set_coherence(Coherence::coherent);

    REQUIRE(incoherent.mean_overlap() == Approx(10.0));

    // Computed from the engine's own measured window statistics.
    const double expected_incoherent = 1.0 / (incoherent.window_rms() * std::sqrt(10.0));
    const double expected_coherent = 1.0 / (coherent.window_mean() * 10.0);
    CHECK(incoherent.grain_gain() == Approx(expected_incoherent).epsilon(1e-9));
    CHECK(coherent.grain_gain() == Approx(expected_coherent).epsilon(1e-9));

    const double ratio_db =
        20.0 * std::log10(incoherent.grain_gain() / coherent.grain_gain());
    CHECK(ratio_db == Approx(8.24).margin(0.05));
}

// ─────────────────────────────────────────────────────────────────────────
// T-7 — voice budget and steal.
// ─────────────────────────────────────────────────────────────────────────

namespace {

struct StealReport {
    int steals = 0;
    double worst_window = 0.0;
    bool always_picked_oldest = true;
    int max_active = 0;
};

/// Steps one sample at a time, snapshotting phases, so a steal can be observed
/// from outside: the reused slot is the one whose phase went backwards.
StealReport observe_steals(GranularEngine64& engine, int samples, bool expect_oldest) {
    StealReport report;
    const int slots = engine.max_grains();
    std::vector<double> phases(static_cast<std::size_t>(slots), -1.0);
    std::uint64_t previous_steals = engine.steal_count();
    double left = 0.0;
    double right = 0.0;

    for (int n = 0; n < samples; ++n) {
        double highest = -1.0;
        for (int s = 0; s < slots; ++s) {
            const auto view = engine.grain(s);
            phases[static_cast<std::size_t>(s)] = view.active ? view.phase : -1.0;
            highest = std::max(highest, phases[static_cast<std::size_t>(s)]);
        }
        engine.process(&left, &right, 1);
        report.max_active = std::max(report.max_active, engine.active_grain_count());

        if (engine.steal_count() != previous_steals) {
            previous_steals = engine.steal_count();
            for (int s = 0; s < slots; ++s) {
                const double before = phases[static_cast<std::size_t>(s)];
                if (before >= 0.0 && engine.grain(s).phase < before) {
                    ++report.steals;
                    report.worst_window = std::max(report.worst_window, engine.window_at(before));
                    if (expect_oldest && before < highest - 1e-12) {
                        report.always_picked_oldest = false;
                    }
                    break;
                }
            }
        }
    }
    return report;
}

}  // namespace

TEST_CASE("Voice budget is never exceeded and stealing is deterministic",
          "[granular][pool]") {
    const std::vector<double> dc(48000, 1.0);

    auto build = [&dc](double overlap, StealPolicy policy) {
        auto engine = std::make_unique<GranularEngine64>();
        engine->prepare(kFs);
        engine->set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
        engine->set_window_taper(1.0);
        engine->set_grain_ms(50.0);
        engine->set_density_hz(overlap / 0.050);
        engine->set_max_grains(32);
        engine->set_steal_policy(policy);
        engine->reset();
        return engine;
    };

    SECTION("the pool is a hard ceiling") {
        auto engine = build(64.0, StealPolicy::oldest);
        const auto report = observe_steals(*engine, 48000, true);
        CHECK(report.max_active <= 32);
        CHECK(report.steals > 0);
        // The policy always recycles the grain furthest through its window —
        // the best candidate available, whatever its window value happens to
        // be at that overlap.
        CHECK(report.always_picked_oldest);
    }

    SECTION("stealing is click-free while demand only just exceeds the budget") {
        // At an overlap barely above the budget the oldest grain really is
        // nearly finished, and the spec's ε_steal ≤ 0.05 holds.
        auto engine = build(33.0, StealPolicy::oldest);
        const auto report = observe_steals(*engine, 48000, true);
        REQUIRE(report.steals > 0);
        CHECK(report.worst_window <= 0.05);
    }

    SECTION("quietest never steals louder than oldest") {
        // Where `oldest` stops being quiet — the recycled grain sits at its
        // window peak once the overlap is twice the budget — `quietest` is the
        // policy that still is. This comparison is the stable invariant; the
        // absolute window value at steal time is a function of the overlap.
        auto by_age = build(64.0, StealPolicy::oldest);
        auto by_level = build(64.0, StealPolicy::quietest);
        const auto aged = observe_steals(*by_age, 24000, true);
        const auto quiet = observe_steals(*by_level, 24000, false);
        REQUIRE(aged.steals > 0);
        REQUIRE(quiet.steals > 0);
        CHECK(quiet.worst_window <= aged.worst_window);
    }

    SECTION("drop_new never cuts a sounding grain") {
        auto engine = build(64.0, StealPolicy::drop_new);
        const auto report = observe_steals(*engine, 48000, false);
        CHECK(report.max_active <= 32);
        CHECK(report.steals == 0);
        CHECK(engine->steal_count() == 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-8 — headroom.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Per-grain gain never exceeds unity", "[granular][gain]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    for (double taper : {0.0, 0.5, 1.0}) {
        engine.set_window_taper(taper);
        for (double grain : {1.0, 10.0, 50.0, 200.0, 500.0}) {
            engine.set_grain_ms(grain);
            for (double density : {0.1, 1.0, 40.0, 400.0, 2000.0}) {
                engine.set_density_hz(density);
                for (auto coherence : {Coherence::incoherent, Coherence::coherent}) {
                    engine.set_coherence(coherence);
                    CHECK(engine.grain_gain() <= 1.0);
                    CHECK(engine.grain_gain() > 0.0);
                }
            }
        }
    }
}

TEST_CASE("Worst-case output stays inside the derived headroom bound",
          "[granular][gain]") {
    // The registry figure has to be a bound this suite asserts. The provable
    // one is `max_grains × peak interpolator kernel gain`: at most
    // `max_grains` grains sound at once, each grain's gain is clamped to 1, the
    // window peaks at 1, and a pan gain is at most 1 — but a 4-point cubic read
    // of a unit-bounded signal is NOT bounded by 1. Its kernel L1 peaks at a
    // half-sample offset, and leaving that factor out understates the ceiling.
    double kernel_peak = 0.0;
    double kernel_peak_at = 0.0;
    for (int i = 0; i <= 200000; ++i) {
        const double t = static_cast<double>(i) / 200000.0;
        double h[4];
        hermite_basis(t, h);
        const double l1 = std::abs(h[0]) + std::abs(h[1]) + std::abs(h[2]) + std::abs(h[3]);
        if (l1 > kernel_peak) {
            kernel_peak = l1;
            kernel_peak_at = t;
        }
    }
    CHECK(kernel_peak == Approx(1.25).epsilon(1e-9));
    CHECK(kernel_peak_at == Approx(0.5).margin(1e-4));

    const double bound = static_cast<double>(GranularEngine64::kMaxGrainBudget) * kernel_peak;

    const std::vector<double> dc(48000, 1.0);
    double reachable = 0.0;
    for (double taper : {0.0, 0.5, 1.0}) {
        for (double grain : {1.0, 20.0, 50.0, 200.0}) {
            for (double density : {100.0, 640.0, 1280.0, 2000.0}) {
                GranularEngine64 engine;
                engine.prepare(kFs);
                engine.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
                engine.set_window_taper(taper);
                engine.set_grain_ms(grain);
                engine.set_density_hz(density);
                engine.set_max_grains(GranularEngine64::kMaxGrainBudget);
                engine.reset();
                const auto out = render(engine, 24000);
                for (double v : out.left) reachable = std::max(reachable, std::abs(v));
                for (double v : out.right) reachable = std::max(reachable, std::abs(v));
            }
        }
    }
    CHECK(reachable < bound);

    // And the bound is loose, because the normalization law fights the overlap
    // that would be needed to reach it: more simultaneous grains means a
    // smaller per-grain gain. Recorded so nobody mistakes the registry ceiling
    // for an operating level.
    CHECK(reachable > 4.0);
    CHECK(reachable < 0.2 * bound);
}

// ─────────────────────────────────────────────────────────────────────────
// T-9 / T-10 — latency and live causality.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("The engine reports zero latency", "[granular][latency]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    CHECK(engine.latency_samples() == 0);
    engine.set_source(GrainSource::live_ring);
    CHECK(engine.latency_samples() == 0);

    // Nothing is buffered ahead: a rectangular grain on a DC source produces
    // output on the very first sample. A tapered window necessarily starts at
    // zero — that is what a window is — so the "energy at sample 0" claim is a
    // statement about scheduling latency, and only a rectangular window can
    // express it.
    const std::vector<double> dc(4800, 1.0);
    GranularEngine64 immediate;
    immediate.prepare(kFs);
    immediate.set_source(GrainSource::buffer);
    immediate.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
    immediate.set_window_taper(0.0);
    immediate.set_grain_ms(50.0);
    immediate.set_density_hz(20.0);
    immediate.reset();
    const auto out = render(immediate, 16);
    CHECK(std::abs(out.left[0]) > 0.0);
}

TEST_CASE("Live grains only ever read written ring samples", "[granular][live]") {
    // A ramp makes the read index observable: with a rectangular window, unity
    // per-grain gain and centre pan, a lone grain's output IS the source value
    // it read, and the source value at absolute index n is n + 1. Anything the
    // ring has never held reads back as 0.
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(0.0);
    engine.set_grain_ms(20.0);
    engine.set_density_hz(30.0);
    engine.set_max_grains(32);
    engine.set_position(0.25);
    engine.set_position_spray_ms(300.0);
    engine.set_pitch_semitones(GranularEngine64::kMaxPitchSemitones);
    engine.reset();
    REQUIRE(engine.grain_gain() == Approx(1.0));

    const double pan = std::cos(kPi / 4.0);
    const int samples = 300000;
    int observed = 0;
    int unwritten = 0;
    double worst_ahead = -1e18;
    double worst_lag = 0.0;

    for (int i = 0; i < samples; ++i) {
        const double x = static_cast<double>(i + 1);
        double left = 0.0;
        double right = 0.0;
        engine.process(&x, &left, &right, 1);
        if (engine.active_grain_count() != 1) continue;
        ++observed;
        const double read_value = left / pan;
        if (read_value <= 0.0) ++unwritten;
        worst_ahead = std::max(worst_ahead, read_value - static_cast<double>(i + 1));
        worst_lag = std::max(worst_lag, static_cast<double>(i + 1) - read_value);
    }

    REQUIRE(observed > 1000);
    CHECK(unwritten == 0);
    CHECK(worst_ahead <= 0.0);
    // Never older than the valid region the derived guard leaves behind.
    CHECK(worst_lag <=
          static_cast<double>(engine.ring_length() - engine.causality_guard_samples()));
}

TEST_CASE("The causality guard is derived from the declared ranges",
          "[granular][live]") {
    GranularEngine64 engine;
    engine.prepare(kFs);

    // guard = fs · (max position spray + max grain length × max pitch-up ratio)
    const double r_max = std::exp2(GranularEngine64::kMaxPitchSemitones / 12.0);
    const double expected =
        std::ceil(kFs * (GranularEngine64::kMaxPositionSprayMs * 0.001 +
                         GranularEngine64::kMaxGrainMs * 0.001 * r_max));
    CHECK(static_cast<double>(engine.causality_guard_samples()) == Approx(expected));
    CHECK(r_max == Approx(4.0));

    // The ring has to clear the guard by at least one full grain, or there
    // would be no valid region left for a grain span to live in. That is why
    // the ring-length range floor is where it is.
    const int valid = engine.ring_length() - engine.causality_guard_samples();
    CHECK(valid > 0);
    CHECK(static_cast<double>(valid) >= GranularEngine64::kMaxGrainMs * 0.001 * kFs);
}

TEST_CASE("Live mode can spawn grains at the declared duration and pitch maxima",
          "[granular][live][ranges]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(0.0);
    engine.set_grain_ms(GranularEngine64::kMaxGrainMs);
    engine.set_density_hz(20.0);
    engine.set_position(0.5);
    engine.set_position_spray_ms(GranularEngine64::kMaxPositionSprayMs);
    engine.set_pitch_semitones(GranularEngine64::kMaxPitchSemitones);
    engine.set_max_grains(GranularEngine64::kMaxGrainBudget);
    engine.set_seed(0x51A7E5u);
    engine.reset();

    // Run long enough to fill and wrap the live ring. The previous placement
    // bounds left no valid interval at this legal corner, so every spawn was
    // silently discarded forever even after all source history was available.
    const auto input = noise_buffer(static_cast<int>(6.0 * kFs), 0xBADC0DEu);
    std::vector<double> left(input.size());
    std::vector<double> right(input.size());
    int max_active = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        engine.process(&input[i], &left[i], &right[i], 1);
        max_active = std::max(max_active, engine.active_grain_count());
    }

    CHECK(max_active > 0);
    CHECK(rms(left, static_cast<int>(4.0 * kFs)) > 1e-6);
}

TEST_CASE("Freeze holds a stationary texture while input continues",
          "[granular][live]") {
    GranularEngine64 engine;
    engine.prepare(kFs);
    engine.set_source(GrainSource::live_ring);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(200.0);
    engine.set_position(0.25);
    engine.reset();

    const auto input = noise_buffer(480000, 7u);
    std::vector<double> left(480000);
    std::vector<double> right(480000);

    engine.process(input.data(), left.data(), right.data(), 240000);
    engine.set_stretch(0.0);  // freeze: hold the captured centre
    engine.process(input.data() + 240000, left.data() + 240000, right.data() + 240000, 240000);

    auto segment_rms = [&left](int from, int to) {
        double sum = 0.0;
        for (int i = from; i < to; ++i) sum += left[static_cast<std::size_t>(i)] * left[static_cast<std::size_t>(i)];
        return std::sqrt(sum / static_cast<double>(to - from));
    };

    const double reference = segment_rms(300000, 340000);
    for (int start : {340000, 380000, 420000}) {
        CHECK(20.0 * std::log10(segment_rms(start, start + 40000) / reference) ==
              Approx(0.0).margin(0.5));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-11 and parameter hygiene.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Granular engine allocates nothing after prepare", "[granular][rt]") {
    const auto source = noise_buffer(48000, 5u);
    GranularEngine engine;
    engine.prepare(kFs);
    engine.set_buffer(nullptr, 0, 1);

    std::vector<float> input(512);
    std::vector<float> left(512);
    std::vector<float> right(512);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(source[i]);
    }
    std::vector<float> buffer_source(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        buffer_source[i] = static_cast<float>(source[i]);
    }

    pulp::test::RtAllocationProbe probe;

    engine.set_buffer(buffer_source.data(), static_cast<int>(buffer_source.size()), 1);
    for (int block = 0; block < 8; ++block) {
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    }
    engine.set_source(GrainSource::live_ring);
    for (int block = 0; block < 8; ++block) {
        engine.write_live(input.data(), static_cast<int>(input.size()));
        engine.process(left.data(), right.data(), static_cast<int>(left.size()));
        engine.process(input.data(), left.data(), right.data(), static_cast<int>(left.size()));
    }

    // Every setter on the control surface, including the two that rebuild the
    // window table.
    engine.set_density_hz(700.0);
    engine.set_grain_ms(17.0);
    engine.set_async_jitter(0.5);
    engine.set_max_grains(64);
    engine.set_steal_policy(StealPolicy::quietest);
    engine.set_window_taper(0.3);
    engine.set_window_trapezoid(true);
    engine.set_pitch_semitones(-7.0);
    engine.set_pitch_spray_semitones(3.0);
    engine.set_pan_spray(0.8);
    engine.set_coherence(Coherence::coherent);
    engine.set_interp(GrainInterp::linear);
    engine.set_level_db(-6.0);
    engine.set_mix(0.4);
    engine.set_position(0.6);
    engine.set_position_spray_ms(80.0);
    engine.set_stretch(2.0);
    engine.set_seed(1234u);
    engine.reset();

    CHECK(probe.allocation_count() == 0);
}

TEST_CASE("Granular parameters clamp to their declared ranges", "[granular][params]") {
    GranularEngine64 engine;
    engine.prepare(kFs);

    engine.set_density_hz(1e9);
    CHECK(engine.density_hz() == Approx(GranularEngine64::kMaxDensityHz));
    engine.set_density_hz(-1.0);
    CHECK(engine.density_hz() == Approx(GranularEngine64::kMinDensityHz));

    engine.set_grain_ms(1e9);
    CHECK(engine.grain_ms() == Approx(GranularEngine64::kMaxGrainMs));
    engine.set_grain_ms(0.0);
    CHECK(engine.grain_ms() == Approx(GranularEngine64::kMinGrainMs));

    engine.set_max_grains(1000);
    CHECK(engine.max_grains() == GranularEngine64::kMaxGrainBudget);
    engine.set_max_grains(1);
    CHECK(engine.max_grains() == GranularEngine64::kMinGrainBudget);

    engine.set_pitch_semitones(100.0);
    CHECK(engine.pitch_semitones() == Approx(GranularEngine64::kMaxPitchSemitones));

    engine.set_stretch(99.0);
    CHECK(engine.stretch() == Approx(GranularEngine64::kMaxStretch));
    engine.set_stretch(-1.0);
    CHECK(engine.stretch() == Approx(0.0));

    engine.set_async_jitter(5.0);
    CHECK(engine.async_jitter() == Approx(1.0));
    engine.set_position(5.0);
    CHECK(engine.position() == Approx(1.0));

    // Lowering the budget must not leave a grain sounding in a slot the pool no
    // longer scans.
    const std::vector<double> dc(48000, 1.0);
    engine.set_buffer(dc.data(), static_cast<int>(dc.size()), 1);
    engine.set_max_grains(64);
    engine.set_grain_ms(200.0);
    engine.set_density_hz(1000.0);
    engine.reset();
    render(engine, 24000);
    REQUIRE(engine.active_grain_count() > 32);
    engine.set_max_grains(32);
    CHECK(engine.active_grain_count() <= 32);
    for (int slot = 32; slot < GranularEngine64::kMaxGrainBudget; ++slot) {
        CHECK_FALSE(engine.grain(slot).active);
    }
}
