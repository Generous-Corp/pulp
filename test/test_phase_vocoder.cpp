// test_phase_vocoder.cpp — offline phase-vocoder time-stretch / pitch-shift.
// Verifies the defining properties: time-stretch changes length but preserves
// pitch; pitch-shift preserves length but changes pitch.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/fft.hpp>
#include <pulp/signal/phase_vocoder.hpp>

#include <cmath>
#include <complex>
#include <vector>

using pulp::signal::Fft;
using pulp::signal::PhaseVocoder;
using pulp::signal::PhaseVocoderConfig;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq_hz, double sample_rate, std::size_t n) {
    std::vector<float> out(n);
    const double w = 2.0 * kPi * freq_hz / sample_rate;
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<float>(std::sin(w * static_cast<double>(i)));
    return out;
}

// Dominant frequency (Hz) of a buffer, measured from a window taken at its
// center to avoid stretch/edge transients.
double dominant_hz(const std::vector<float>& buf, double sample_rate) {
    const int N = 8192;
    if (static_cast<int>(buf.size()) < N) return 0.0;
    const std::size_t start = (buf.size() - N) / 2;
    std::vector<std::complex<float>> spec(N);
    for (int i = 0; i < N; ++i) {
        // Hann window to sharpen the peak.
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (N - 1)));
        spec[i] = std::complex<float>(
            static_cast<float>(buf[start + i] * w), 0.0f);
    }
    Fft fft(N);
    fft.forward(spec.data());
    int peak = 1;
    double peak_mag = 0.0;
    for (int k = 1; k < N / 2; ++k) {
        const double m = std::abs(spec[k]);
        if (m > peak_mag) {
            peak_mag = m;
            peak = k;
        }
    }
    return static_cast<double>(peak) * sample_rate / N;
}

}  // namespace

TEST_CASE("PhaseVocoder time-stretch changes length, preserves pitch",
          "[signal][phasevocoder]") {
    const double sr = 44100.0;
    const std::size_t n = 44100;  // 1 second
    const double f0 = 440.0;
    const auto input = sine(f0, sr, n);

    PhaseVocoder pv;  // default 2048 / hop 512

    SECTION("stretch 2x") {
        const auto out = pv.time_stretch(input, 2.0);
        const double ratio = static_cast<double>(out.size()) / n;
        CAPTURE(ratio);
        REQUIRE(ratio > 1.9);
        REQUIRE(ratio < 2.05);
        REQUIRE(std::abs(dominant_hz(out, sr) - f0) < 12.0);  // pitch unchanged
    }

    SECTION("compress 0.5x") {
        const auto out = pv.time_stretch(input, 0.5);
        const double ratio = static_cast<double>(out.size()) / n;
        CAPTURE(ratio);
        REQUIRE(ratio > 0.47);
        REQUIRE(ratio < 0.53);
        REQUIRE(std::abs(dominant_hz(out, sr) - f0) < 12.0);
    }
}

TEST_CASE("PhaseVocoder pitch-shift preserves length, changes pitch",
          "[signal][phasevocoder]") {
    const double sr = 44100.0;
    const std::size_t n = 44100;
    const double f0 = 330.0;
    const auto input = sine(f0, sr, n);

    PhaseVocoder pv;

    SECTION("up one octave") {
        const auto out = pv.pitch_shift(input, 12.0);
        REQUIRE(out.size() == n);  // length preserved exactly
        REQUIRE(std::abs(dominant_hz(out, sr) - 2.0 * f0) < 22.0);
    }

    SECTION("down one octave") {
        const auto out = pv.pitch_shift(input, -12.0);
        REQUIRE(out.size() == n);
        REQUIRE(std::abs(dominant_hz(out, sr) - 0.5 * f0) < 22.0);
    }

    SECTION("zero semitones is identity") {
        const auto out = pv.pitch_shift(input, 0.0);
        REQUIRE(out.size() == n);
    }
}
