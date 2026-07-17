// Tests for the noise-based PulpKit voices: the handclap and the maracas.
//
// These voices are not tonal, so the tests are statistical rather than
// pitch-based: render the voice offline and measure the numbers that define its
// character against the reference instrument's measured values.
//
//   Handclap  -- a resonant band-pass at ~1150 Hz (the measured reference
//                centre), a three-flam attack in the first ~30 ms, a diffuse
//                tail with T60 ~1.0 s, and a broadband spectrum.
//   Maracas   -- a short (~80 ms) bright, high-passed noise burst.
//
// Every claim is a measured quantity: the flam count from a short-time RMS
// envelope, the resonance from a Goertzel bank, T60 from the -60 dB crossing,
// and the output bound from the rendered peak. A per-sample allocation probe
// holds the audio-thread contract for both voices.
//
// The spectrum is measured with a bank of Hann-windowed Goertzel bins (no FFT
// dependency): the voices are broadband, so a log-spaced bank of magnitudes is
// enough to locate the band-pass resonance, measure spectral flatness, and
// separate bright (maracas) from mid-focused (clap).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include "pulp-kit/voices/clap_voice.hpp"
#include "pulp-kit/voices/maracas_voice.hpp"

#include <cmath>
#include <vector>

namespace {

using pulp::examples::ClapVoice;
using pulp::examples::MaracasVoice;

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

template <typename Voice>
std::vector<float> render(Voice& v, double seconds) {
    std::vector<float> y(static_cast<std::size_t>(kFs * seconds));
    for (auto& s : y) s = v.process();
    return y;
}

// Hann-windowed Goertzel magnitude (linear, single-sided) at frequency f.
double goertzel(const std::vector<float>& x, double f) {
    const std::size_t N = x.size();
    const double w = 2.0 * kPi * f / kFs;
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

// A log-spaced bank of Goertzel magnitudes over the audible range.
struct SpectrumBank {
    std::vector<double> freq;
    std::vector<double> mag;
};

SpectrumBank spectrum(const std::vector<float>& x) {
    SpectrumBank b;
    constexpr int kBins = 48;
    for (int i = 0; i < kBins; ++i) {
        const double f = 150.0 * std::pow(16000.0 / 150.0, i / double(kBins - 1));
        b.freq.push_back(f);
        b.mag.push_back(goertzel(x, f));
    }
    return b;
}

// Spectral flatness (geometric mean / arithmetic mean of the bank magnitudes):
// ~0 for a pure tone, toward 1 for white noise.
double flatness(const SpectrumBank& b) {
    double log_sum = 0.0, lin_sum = 0.0;
    for (double m : b.mag) {
        log_sum += std::log(m + 1e-20);
        lin_sum += m;
    }
    const double n = static_cast<double>(b.mag.size());
    return std::exp(log_sum / n) / (lin_sum / n + 1e-20);
}

// Magnitude-weighted spectral centroid over the bank (Hz).
double centroid(const SpectrumBank& b) {
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < b.freq.size(); ++i) {
        num += b.freq[i] * b.mag[i];
        den += b.mag[i];
    }
    return num / (den + 1e-20);
}

// Frequency of the loudest bin in the bank (the band-pass resonance).
double dominant_hz(const SpectrumBank& b) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < b.mag.size(); ++i)
        if (b.mag[i] > b.mag[best]) best = i;
    return b.freq[best];
}

// Count local maxima in a 1 ms short-time RMS envelope over the first
// `window_ms` milliseconds -- the clap's flam pulses. A peak must exceed 15% of
// the window maximum so decay ripple is not miscounted.
int flam_peaks(const std::vector<float>& x, int window_ms) {
    const int hop = static_cast<int>(kFs * 0.001);
    std::vector<double> env;
    double emax = 0.0;
    for (int t = 0; t < window_ms; ++t) {
        double acc = 0.0;
        int n = 0;
        for (std::size_t i = std::size_t(t) * hop;
             i < std::size_t(t + 1) * hop && i < x.size(); ++i) {
            acc += double(x[i]) * double(x[i]);
            ++n;
        }
        const double rms = n ? std::sqrt(acc / n) : 0.0;
        env.push_back(rms);
        emax = std::max(emax, rms);
    }
    int peaks = 0;
    for (std::size_t t = 1; t + 1 < env.size(); ++t)
        if (env[t] > env[t - 1] && env[t] >= env[t + 1] && env[t] > emax * 0.15)
            ++peaks;
    return peaks;
}

// T60: time of the last sample above peak/1000 (-60 dB), in seconds.
double t60_s(const std::vector<float>& x) {
    float peak = 0.0f;
    std::size_t peak_i = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float a = std::fabs(x[i]);
        if (a > peak) { peak = a; peak_i = i; }
    }
    const float thr = peak / 1000.0f;
    std::size_t last = peak_i;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > thr) last = i;
    return double(last) / kFs;
}

float peak_abs(const std::vector<float>& x) {
    float p = 0.0f;
    for (float v : x) p = std::max(p, std::fabs(v));
    return p;
}

}  // namespace

TEST_CASE("Handclap renders a three-flam attack into a diffuse tail",
          "[pulp-kit][noise-voice][clap]") {
    ClapVoice v;
    v.prepare(kFs);
    v.trigger(1.0f);
    const std::vector<float> x = render(v, 2.0);

    // The defining feature of a clap: several closely spaced amplitude pulses
    // (the flams) in the first ~30 ms, not a single onset.
    const int flams = flam_peaks(x, 30);
    CAPTURE(flams);
    REQUIRE(flams >= 3);

    // Resonant band-pass centred on the reference's measured ~1150 Hz.
    const SpectrumBank spec = spectrum(x);
    const double dom = dominant_hz(spec);
    CAPTURE(dom);
    REQUIRE(dom > 900.0);
    REQUIRE(dom < 1600.0);

    // Broadband, not a tone: flatness is well away from zero.
    const double flat = flatness(spec);
    CAPTURE(flat);
    REQUIRE(flat > 0.25);

    // A diffuse tail near the reference's T60 ~1.0 s.
    const double t60 = t60_s(x);
    CAPTURE(t60);
    REQUIRE(t60 > 0.70);
    REQUIRE(t60 < 1.40);
}

TEST_CASE("Handclap output stays inside its declared bound",
          "[pulp-kit][noise-voice][clap]") {
    ClapVoice v;
    v.prepare(kFs);
    v.trigger(1.0f);
    const std::vector<float> x = render(v, 2.0);

    const float peak = peak_abs(x);
    CAPTURE(peak, v.peak_bound());
    REQUIRE(peak <= v.peak_bound());
    // Calibrated to the reference peak (~0.42); a non-trivial, audible level.
    REQUIRE(peak > 0.25f);
    REQUIRE(peak < 0.65f);
}

TEST_CASE("Maracas is a short bright broadband burst",
          "[pulp-kit][noise-voice][maracas]") {
    MaracasVoice v;
    v.prepare(kFs);
    v.trigger(1.0f);
    const std::vector<float> x = render(v, 0.6);

    // Short: a shaker is gone almost as soon as it starts.
    const double t60 = t60_s(x);
    CAPTURE(t60);
    REQUIRE(t60 > 0.03);
    REQUIRE(t60 < 0.20);

    // Bright and high-passed: the centroid sits high and the energy above 4 kHz
    // dwarfs the energy below 1 kHz.
    const SpectrumBank spec = spectrum(x);
    const double cen = centroid(spec);
    CAPTURE(cen);
    REQUIRE(cen > 4000.0);

    double hi = 0.0, lo = 0.0;
    for (std::size_t i = 0; i < spec.freq.size(); ++i) {
        if (spec.freq[i] > 4000.0) hi += spec.mag[i];
        if (spec.freq[i] < 1000.0) lo += spec.mag[i];
    }
    CAPTURE(hi, lo);
    REQUIRE(hi > lo * 5.0);
}

TEST_CASE("Maracas output stays inside its declared bound",
          "[pulp-kit][noise-voice][maracas]") {
    MaracasVoice v;
    v.prepare(kFs);
    v.trigger(1.0f);
    const std::vector<float> x = render(v, 0.6);

    const float peak = peak_abs(x);
    CAPTURE(peak, v.peak_bound());
    REQUIRE(peak <= v.peak_bound());
    REQUIRE(peak > 0.1f);
}

TEST_CASE("Handclap process allocates nothing",
          "[pulp-kit][noise-voice][clap][rt-safety]") {
    ClapVoice v;
    v.prepare(kFs);
    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            if (block % 16 == 0) v.trigger(0.8f);
            if (block % 40 == 0) v.set_gain(1.5f);
            for (int n = 0; n < 64; ++n) (void)v.process();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Maracas process allocates nothing",
          "[pulp-kit][noise-voice][maracas][rt-safety]") {
    MaracasVoice v;
    v.prepare(kFs);
    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            if (block % 8 == 0) v.trigger(0.9f);
            if (block % 40 == 0) v.set_t60(0.06f);
            for (int n = 0; n < 64; ++n) (void)v.process();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}
