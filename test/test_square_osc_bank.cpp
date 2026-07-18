// Tests for the reusable square-wave oscillator bank. The bank's whole job is
// to turn a list of fixed inharmonic frequencies into a clustered tone, so the
// tests are spectral: render the bank, take the spectrum, and prove that every
// configured oscillator shows up as its own partial, that the summed output
// stays inside its declared bound, and that the band-limiting keeps folded
// aliases far below the signal. A per-sample allocation probe holds the
// audio-thread contract.
//
// The spectrum is measured with a Goertzel evaluator rather than a full FFT:
// the frequencies of interest are known in advance (they are the partials we
// asked for and a grid of off-harmonic alias probes), so a handful of targeted
// Goertzel bins are both sufficient and exact at those frequencies.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/square_osc_bank.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using pulp::signal::SquareOscBank;

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Hann-windowed Goertzel magnitude (linear, single-sided) at frequency f.
double goertzel(const std::vector<float>& x, double fs, double f) {
    const std::size_t N = x.size();
    const double w = 2.0 * kPi * f / fs;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t n = 0; n < N; ++n) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * n / (N - 1));
        const double s0 = coeff * s1 - s2 + win * static_cast<double>(x[n]);
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (N * 0.5);
}

double db(double num, double den) {
    return 20.0 * std::log10(num / (den + 1e-20));
}

std::vector<float> render(SquareOscBank& bank, std::size_t n) {
    std::vector<float> y(n);
    for (std::size_t i = 0; i < n; ++i) y[i] = bank.process();
    return y;
}

}  // namespace

TEST_CASE("SquareOscBank recovers every configured partial", "[signal][square-osc-bank]") {
    // A representative six-oscillator inharmonic cluster.
    const std::vector<SquareOscBank::Partial> cluster = {
        {540.0f, 1.0f},  {800.0f, 1.0f},  {1200.0f, 1.0f},
        {1800.0f, 1.0f}, {2600.0f, 1.0f}, {3600.0f, 1.0f}};

    SquareOscBank bank;
    bank.prepare(static_cast<float>(kFs));
    bank.set_partials(cluster);
    bank.set_level(1.0f / static_cast<float>(cluster.size()));
    REQUIRE(bank.size() == cluster.size());

    const std::vector<float> y = render(bank, 1 << 15);

    // Between-partial floor: a frequency that is not near any oscillator's
    // fundamental. Every partial must tower over it.
    const double floor = goertzel(y, kFs, 5100.0);
    for (const auto& p : cluster) {
        const double mag = goertzel(y, kFs, static_cast<double>(p.frequency));
        INFO("partial " << p.frequency << " Hz at " << db(mag, floor) << " dB over floor");
        REQUIRE(db(mag, floor) > 40.0);
    }
}

TEST_CASE("SquareOscBank recovers a two-oscillator pair", "[signal][square-osc-bank]") {
    // Both configured fundamentals must be present.
    const std::vector<SquareOscBank::Partial> pair = {{540.0f, 1.0f}, {800.0f, 1.0f}};

    SquareOscBank bank;
    bank.prepare(static_cast<float>(kFs));
    bank.set_partials(pair);
    bank.set_level(0.5f);

    const std::vector<float> y = render(bank, 1 << 15);
    const double floor = goertzel(y, kFs, 2000.0);
    REQUIRE(db(goertzel(y, kFs, 540.0), floor) > 40.0);
    REQUIRE(db(goertzel(y, kFs, 800.0), floor) > 40.0);
}

TEST_CASE("SquareOscBank output stays within its declared bound", "[signal][square-osc-bank]") {
    const std::vector<SquareOscBank::Partial> cluster = {
        {317.0f, 0.9f}, {733.0f, 0.7f}, {1490.0f, 1.1f}, {2971.0f, 0.5f}};

    SquareOscBank bank;
    bank.prepare(static_cast<float>(kFs));
    bank.set_partials(cluster);
    bank.set_level(0.8f);

    const float bound = bank.peak_bound();
    // The bound is |level| * sum|amp|; the actual peak must never exceed it.
    REQUIRE(bound == Catch::Approx(0.8f * (0.9f + 0.7f + 1.1f + 0.5f)));

    const std::vector<float> y = render(bank, 1 << 16);
    float peak = 0.0f;
    for (float s : y) peak = std::max(peak, std::fabs(s));
    REQUIRE(peak <= bound + 1e-4f);
    // And the bank actually swings hard -- the bound is not vacuously large.
    REQUIRE(peak > 0.4f * bound);
}

TEST_CASE("SquareOscBank band-limiting suppresses folded aliases", "[signal][square-osc-bank]") {
    // A single square at a frequency that does not divide the sample rate, so
    // its high odd harmonics fold to frequencies OFF the true odd-harmonic
    // grid. We measure how much energy lands on the grid (the real signal)
    // versus off it (folded aliases), for the band-limited bank and for a
    // naive sign() square rendered identically.
    const double f0 = 2777.0;
    const std::size_t N = 1 << 15;

    std::vector<float> blep(N);
    {
        SquareOscBank bank;
        bank.prepare(static_cast<float>(kFs));
        const std::vector<SquareOscBank::Partial> one = {{static_cast<float>(f0), 1.0f}};
        bank.set_partials(one);
        blep = render(bank, N);
    }

    std::vector<float> naive(N);
    {
        double phase = 0.0;
        const double dt = f0 / kFs;
        for (std::size_t n = 0; n < N; ++n) {
            naive[n] = phase < 0.5 ? 1.0f : -1.0f;
            phase += dt;
            if (phase >= 1.0) phase -= 1.0;
        }
    }

    auto alias_floor_db = [&](const std::vector<float>& z) {
        double on = 0.0;
        for (int k = 1; k * f0 < kFs / 2.0; k += 2) {
            const double m = goertzel(z, kFs, k * f0);
            on += m * m;
        }
        double off = 0.0;
        int cnt = 0;
        for (double f = 300.0; f < kFs / 2.0 - 300.0; f += 231.0) {
            bool near_harmonic = false;
            for (int k = 1; k * f0 < kFs / 2.0; k += 2) {
                if (std::fabs(f - k * f0) < 80.0) { near_harmonic = true; break; }
            }
            if (near_harmonic) continue;
            const double m = goertzel(z, kFs, f);
            off += m * m;
            ++cnt;
        }
        return 10.0 * std::log10((off / cnt) / (on + 1e-20));
    };

    const double blep_alias = alias_floor_db(blep);
    const double naive_alias = alias_floor_db(naive);
    INFO("blep alias floor " << blep_alias << " dB, naive " << naive_alias << " dB");

    // The band-limited bank keeps folded aliases far below the signal ...
    REQUIRE(blep_alias < -70.0);
    // ... and materially better than a naive square rendered the same way.
    REQUIRE(blep_alias < naive_alias - 20.0);
}

TEST_CASE("SquareOscBank process allocates nothing", "[signal][square-osc-bank][rt-safety]") {
    const std::vector<SquareOscBank::Partial> cluster = {
        {540.0f, 1.0f}, {800.0f, 1.0f}, {1200.0f, 1.0f},
        {1800.0f, 1.0f}, {2600.0f, 1.0f}, {3600.0f, 1.0f}};

    SquareOscBank bank;
    bank.prepare(static_cast<float>(kFs));
    bank.set_partials(cluster);

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            // Retuning, reweighting, level, reset and rendering are all
            // audio-thread operations and must not allocate.
            bank.set_frequency(block % bank.size(), 500.0f + 5.0f * (block % 40));
            bank.set_amplitude(block % bank.size(), 0.5f + 0.005f * (block % 100));
            bank.set_level(0.9f);
            if (block % 32 == 0) bank.reset();
            for (int n = 0; n < 64; ++n) (void)bank.process();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}
