// Tests for the deterministic colourable noise source.
//
// The class makes three claims and each one gets a measurement rather than a
// smoke test: the named colours really have the power-spectral slopes they are
// named after, the slopes hold at more than one sample rate, and a reset
// reproduces the sequence exactly. Slope is measured by integrating power in
// octave bands from a Welch-averaged periodogram and fitting a line to the
// band energies in dB against log2 of the band centre — which is the
// definition of "dB per octave" rather than a proxy for it.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/noise_source.hpp>

#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace {

using pulp::signal::NoiseColor;
using pulp::signal::NoiseSource64;

constexpr double kPi = 3.14159265358979323846;

// Naive DFT magnitude-squared over one Hann-windowed frame. The frames here
// are 4096 points and there are a handful of them, so an O(N^2) transform is a
// few tens of millions of operations -- fast enough for a test and free of any
// dependency on which FFT backend happens to be configured.
std::vector<double> power_spectrum(const std::vector<double>& frame) {
    const std::size_t n = frame.size();
    std::vector<double> windowed(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                              static_cast<double>(n - 1));
        windowed[i] = frame[i] * w;
    }
    std::vector<double> power(n / 2 + 1, 0.0);
    for (std::size_t k = 0; k < power.size(); ++k) {
        double re = 0.0, im = 0.0;
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) {
            re += windowed[i] * std::cos(w * static_cast<double>(i));
            im -= windowed[i] * std::sin(w * static_cast<double>(i));
        }
        power[k] = re * re + im * im;
    }
    return power;
}

// Welch-averaged power spectrum over `frames` non-overlapping blocks.
std::vector<double> averaged_spectrum(NoiseSource64& src, std::size_t frame_size,
                                      int frames) {
    std::vector<double> accumulated(frame_size / 2 + 1, 0.0);
    std::vector<double> frame(frame_size);
    for (int f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < frame_size; ++i) frame[i] = src.process();
        const auto p = power_spectrum(frame);
        for (std::size_t k = 0; k < accumulated.size(); ++k) accumulated[k] += p[k];
    }
    for (auto& v : accumulated) v /= frames;
    return accumulated;
}

// Least-squares slope of band energy (dB) against log2(band centre), i.e. the
// spectral tilt in dB per octave over the measured range.
double slope_db_per_octave(const std::vector<double>& power, double sample_rate,
                           double low_hz, double high_hz) {
    const std::size_t n_bins = power.size();
    const double bin_hz = 0.5 * sample_rate / static_cast<double>(n_bins - 1);

    std::vector<double> x, y;
    for (double f = low_hz; f * 2.0 <= high_hz * 1.0001; f *= 2.0) {
        const double lo = f, hi = f * 2.0;
        const auto first = static_cast<std::size_t>(std::ceil(lo / bin_hz));
        const auto last = static_cast<std::size_t>(std::floor(hi / bin_hz));
        if (last <= first || last >= n_bins) continue;
        double energy = 0.0;
        for (std::size_t k = first; k <= last; ++k) energy += power[k];
        // Per-band energy divided by band width gives power density, whose
        // tilt is the slope the colour names claim.
        const double density = energy / static_cast<double>(last - first + 1);
        x.push_back(std::log2(std::sqrt(lo * hi)));
        y.push_back(10.0 * std::log10(density + 1e-30));
    }
    REQUIRE(x.size() >= 4);

    const double mean_x = std::accumulate(x.begin(), x.end(), 0.0) / x.size();
    const double mean_y = std::accumulate(y.begin(), y.end(), 0.0) / y.size();
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        num += (x[i] - mean_x) * (y[i] - mean_y);
        den += (x[i] - mean_x) * (x[i] - mean_x);
    }
    return num / den;
}

double measured_slope(NoiseColor color, double sample_rate, double low_hz, double high_hz) {
    NoiseSource64 src;
    src.prepare(sample_rate);
    src.set_color(color);
    src.reset();
    const auto power = averaged_spectrum(src, 4096, 8);
    return slope_db_per_octave(power, sample_rate, low_hz, high_hz);
}

double rms_of(NoiseColor color, double sample_rate, int n) {
    NoiseSource64 src;
    src.prepare(sample_rate);
    src.set_color(color);
    src.reset();
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = src.process();
        sum += v * v;
    }
    return std::sqrt(sum / n);
}

}  // namespace

TEST_CASE("White noise is spectrally flat", "[signal][noise]") {
    // A tolerance of 1 dB/octave over seven octaves is tight: an unintended
    // one-pole anywhere in the path would show up as several dB of tilt.
    REQUIRE(std::fabs(measured_slope(NoiseColor::white, 48000.0, 100.0, 12800.0)) < 1.0);
}

TEST_CASE("Pink noise falls at three decibels per octave", "[signal][noise]") {
    const double slope = measured_slope(NoiseColor::pink, 48000.0, 100.0, 12800.0);
    REQUIRE(slope < -2.4);
    REQUIRE(slope > -3.6);
}

TEST_CASE("Brown noise falls at six decibels per octave", "[signal][noise]") {
    const double slope = measured_slope(NoiseColor::brown, 48000.0, 100.0, 12800.0);
    REQUIRE(slope < -5.4);
    REQUIRE(slope > -6.6);
}

TEST_CASE("Blue noise rises at three decibels per octave", "[signal][noise]") {
    const double slope = measured_slope(NoiseColor::blue, 48000.0, 100.0, 6400.0);
    REQUIRE(slope > 2.4);
    REQUIRE(slope < 3.6);
}

TEST_CASE("Violet noise rises at six decibels per octave", "[signal][noise]") {
    const double slope = measured_slope(NoiseColor::violet, 48000.0, 100.0, 6400.0);
    REQUIRE(slope > 5.4);
    REQUIRE(slope < 6.6);
}

TEST_CASE("Colour slopes are the same at a different sample rate", "[signal][noise]") {
    // The colour filters are specified in Hz, so the same band of audio must
    // come out with the same tilt whatever rate it is rendered at. A
    // per-sample coefficient table would fail this.
    const double pink_44 = measured_slope(NoiseColor::pink, 44100.0, 100.0, 6400.0);
    const double pink_96 = measured_slope(NoiseColor::pink, 96000.0, 100.0, 6400.0);
    REQUIRE(std::fabs(pink_44 - pink_96) < 0.6);

    const double brown_44 = measured_slope(NoiseColor::brown, 44100.0, 100.0, 6400.0);
    const double brown_96 = measured_slope(NoiseColor::brown, 96000.0, 100.0, 6400.0);
    REQUIRE(std::fabs(brown_44 - brown_96) < 0.6);
}

TEST_CASE("Every colour lands on the same output level", "[signal][noise]") {
    // Switching colour must not jump the level, or a colour control doubles as
    // an unwanted gain control.
    const double white = rms_of(NoiseColor::white, 48000.0, 200000);
    for (auto color : {NoiseColor::pink, NoiseColor::brown, NoiseColor::blue,
                       NoiseColor::violet}) {
        const double r = rms_of(color, 48000.0, 200000);
        REQUIRE(r > white * 0.7);
        REQUIRE(r < white * 1.4);
    }
}

TEST_CASE("Reset reproduces the sequence exactly", "[signal][noise]") {
    NoiseSource64 src;
    src.prepare(48000.0);
    src.set_color(NoiseColor::pink);

    src.reset();
    std::vector<double> first(2048);
    for (auto& v : first) v = src.process();

    src.reset();
    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE(src.process() == first[i]);
    }
}

TEST_CASE("A different seed gives a different sequence", "[signal][noise]") {
    NoiseSource64 a, b;
    a.prepare(48000.0);
    b.prepare(48000.0);
    b.set_seed(0xABCDEF01u);
    a.reset();
    b.reset();

    int differing = 0;
    for (int i = 0; i < 1024; ++i) {
        if (a.process() != b.process()) ++differing;
    }
    REQUIRE(differing > 1000);
}

TEST_CASE("A zero seed is remapped rather than freezing the generator",
          "[signal][noise]") {
    // Xorshift is degenerate at zero: without the remap the source would emit a
    // constant forever, which is a silent failure in a voice.
    NoiseSource64 src;
    src.prepare(48000.0);
    src.set_seed(0);
    REQUIRE(src.seed() == NoiseSource64::default_seed);
    src.reset();

    int nonzero = 0;
    for (int i = 0; i < 256; ++i) {
        if (src.process() != 0.0) ++nonzero;
    }
    REQUIRE(nonzero > 250);
}

TEST_CASE("White output stays inside its declared range", "[signal][noise]") {
    NoiseSource64 src;
    src.prepare(48000.0);
    src.reset();
    for (int i = 0; i < 100000; ++i) {
        const double v = src.white();
        REQUIRE(v >= -1.0);
        REQUIRE(v < 1.0);
    }
}

TEST_CASE("Noise generation allocates nothing on the audio thread",
          "[signal][noise][rt-safety]") {
    NoiseSource64 src;
    src.prepare(48000.0);
    src.set_color(NoiseColor::pink);
    src.reset();

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 8192; ++i) sink += src.process();
        src.reset();
        sink += src.white();
        allocations = probe.allocation_count();
    }

    REQUIRE(std::isfinite(sink));  // consume the result so the loop cannot be elided
    REQUIRE(allocations == 0);
}
