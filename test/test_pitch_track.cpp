#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/audio/analysis/pitch_track.hpp>

#include <cmath>
#include <initializer_list>
#include <random>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::test::audio::cents_between;
using pulp::test::audio::estimate_frequency;
using pulp::test::audio::estimate_pitch;
using pulp::test::audio::PitchTrack;
using pulp::test::audio::PitchTrackOptions;
using pulp::test::audio::track_pitch;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;

// A steady sine at f0.
std::vector<float> make_tone(double f0, int n, double amp = 0.5,
                             double sr = kSampleRate) {
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * f0 * i / sr));
    return x;
}

// Additive bandlimited sawtooth (1/k harmonics up to the anti-alias ceiling) —
// the canonical harmonically-dense oscillator waveform, fundamental-dominant.
std::vector<float> make_saw(double f0, int n, double sr = kSampleRate) {
    const int k_max = static_cast<int>(0.45 * sr / f0);
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k <= k_max; ++k)
            s += std::sin(2.0 * kPi * k * f0 * i / sr) / k;
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * s);
    }
    return x;
}

// Bright signal: a strong fundamental plus an upper-harmonic formant band, like
// a resonant/hard oscillator. The fundamental is the loudest partial (so an
// FFT-peak estimator locks to it), but the fast upper wiggle injects spurious
// zero crossings that defeat a zero-crossing detector.
std::vector<float> make_formant(double f0, int n, double sr = kSampleRate) {
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        double s = std::sin(2.0 * kPi * f0 * i / sr);
        for (int k = 9; k <= 13; ++k)
            s += 0.5 * std::sin(2.0 * kPi * k * f0 * i / sr);
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.3 * s);
    }
    return x;
}

// A sum of sinusoidal partials {harmonic index, amplitude} on fundamental f0.
// The tool for octave-guard cases where a HARMONIC — not the fundamental — is
// the loudest partial, so the FFT peak lands an octave (or more) high. The
// per-partial phase offset keeps the sum from being an artificially symmetric
// waveform.
std::vector<float> make_partials(
    double f0, int n, std::initializer_list<std::pair<int, double>> partials,
    double sr = kSampleRate) {
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (const auto& [k, a] : partials)
            s += a * std::sin(2.0 * kPi * k * f0 * i / sr + 0.1 * k);
        x[static_cast<std::size_t>(i)] = static_cast<float>(s);
    }
    return x;
}

} // namespace

TEST_CASE("Pitch estimate is sub-cent accurate across the band", "[audio][pitch]") {
    // Coherent (k = f0·N/fs integer) and non-coherent (fractional k) tones. If
    // the estimator only worked on bin-coherent tones it would be a bin counter,
    // not a pitch estimator — the non-coherent frequencies (82.41, 261.63,
    // 3333.7) fall between bins and are the falsifying cases.
    const double kFreqs[] = {55.0, 82.41, 220.0, 261.63, 440.0, 1000.0, 3333.7};
    const int n = 1 << 15;
    for (double f0 : kFreqs) {
        const auto x = make_tone(f0, n);
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        INFO("f0 = " << f0);
        REQUIRE(est.voiced);
        // Measured error is < 0.002 cent; assert a floor 25x looser so the test
        // is robust across FFT backends but still proves cents-level accuracy.
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 0.05);
    }
}

TEST_CASE("Pitch estimate beats the zero-crossing detector on dense material",
          "[audio][pitch]") {
    // This is the reason the estimator exists: the shipped zero-crossing
    // estimate_frequency disclaims harmonically-dense signals, and a bright
    // oscillator IS dense. On the formant signal the zero-crossing detector
    // locks to an upper partial (~3·f0, an octave-and-a-fifth high) while the
    // projection estimator stays within a hundredth of a cent.
    const int n = 1 << 15;
    for (double f0 : {110.0, 220.0, 440.0}) {
        const auto x = make_formant(f0, n);
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        const auto zc = estimate_frequency(std::span<const float>(x), kSampleRate);
        INFO("f0 = " << f0 << " zero-crossing = " << zc.hz
                     << " pitch = " << est.hz);
        REQUIRE(est.voiced);
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 0.5);
        // The zero-crossing detector is off by more than a whole semitone (100
        // cents); in practice it lands ~1900 cents high here.
        REQUIRE(zc.hz > 0.0);
        REQUIRE(std::abs(cents_between(zc.hz, f0)) > 100.0);
    }
}

TEST_CASE("Pitch estimate is accurate on a bandlimited sawtooth",
          "[audio][pitch]") {
    const int n = 1 << 15;
    for (double f0 : {110.0, 220.0, 440.0, 880.0}) {
        const auto x = make_saw(f0, n);
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        INFO("f0 = " << f0);
        REQUIRE(est.voiced);
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 0.5);
    }
}

TEST_CASE("Pitch estimator refuses signals with no clear fundamental",
          "[audio][pitch]") {
    const int n = 1 << 15;

    SECTION("silence is refused, not given a confident wrong pitch") {
        std::vector<float> zero(static_cast<std::size_t>(n), 0.0f);
        const auto est = estimate_pitch(std::span<const float>(zero), kSampleRate);
        REQUIRE_FALSE(est.voiced);
        REQUIRE(est.hz == 0.0);
    }

    SECTION("white noise is refused") {
        std::vector<float> noise(static_cast<std::size_t>(n));
        std::mt19937 gen(1);
        std::normal_distribution<float> dist(0.0f, 0.3f);
        for (auto& v : noise)
            v = dist(gen);
        const auto est = estimate_pitch(std::span<const float>(noise), kSampleRate);
        REQUIRE_FALSE(est.voiced);
    }

    SECTION("lowpass (pitchless but energetic) noise is refused") {
        // Energetic enough to clear the silence floor, so only the harmonic
        // confidence gate can reject it — proving the gate does real work.
        std::vector<float> noise(static_cast<std::size_t>(n));
        std::mt19937 gen(2);
        std::normal_distribution<float> dist(0.0f, 0.3f);
        float prev = 0.0f;
        for (auto& v : noise) {
            prev = 0.98f * prev + 0.02f * dist(gen);
            v = prev * 8.0f;
        }
        const auto est = estimate_pitch(std::span<const float>(noise), kSampleRate);
        REQUIRE_FALSE(est.voiced);
    }
}

TEST_CASE("Pitch estimator does not drop a pure tone to a subharmonic",
          "[audio][pitch]") {
    // The octave-correction descends only to a subharmonic that carries real
    // energy; a pure tone has none at f/2, so it must stay on the fundamental.
    const int n = 1 << 15;
    const auto x = make_tone(440.0, n);
    const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
    REQUIRE(est.voiced);
    REQUIRE_THAT(est.hz, WithinAbs(440.0, 1.0));
}

TEST_CASE("Pitch estimator recovers f0 when a harmonic is the loudest partial",
          "[audio][pitch]") {
    // A rolled-off or resonant oscillator can carry more energy in an upper
    // partial than in the fundamental, so the loudest FFT bin lands an octave (or
    // a twelfth, or higher) above the true f0. The estimator must descend to the
    // fundamental, never certify the harmonic as the pitch.
    const int n = 1 << 15;
    const double f0 = 220.0;

    SECTION("2nd harmonic loudest, fundamental at 30%") {
        const auto x =
            make_partials(f0, n, {{1, 0.3}, {2, 1.0}, {3, 0.25}, {4, 0.2}});
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        REQUIRE(est.voiced);
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 1.0);
    }
    SECTION("3rd harmonic loudest, fundamental at 30%") {
        const auto x =
            make_partials(f0, n, {{1, 0.3}, {2, 0.2}, {3, 1.0}, {4, 0.15}});
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        REQUIRE(est.voiced);
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 1.0);
    }
    SECTION("5th harmonic loudest, odd-only series") {
        // A square-ish odd-harmonic series whose loudest partial is the 5th. An
        // earlier guard descended only as far as coarse/4, so it never tested the
        // true fundamental (coarse/5) and certified the 5th harmonic ~2786 cents
        // high at confidence 0.775 — a confident wrong number. It must recover f0.
        const auto x = make_partials(f0, n, {{1, 0.4}, {3, 0.3}, {5, 1.0}, {7, 0.2}});
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        REQUIRE(est.voiced);
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 1.0);
    }
}

TEST_CASE("Pitch estimator does not octave-error across the adoption boundary",
          "[audio][pitch]") {
    // The earlier guard adopted a subharmonic only when its single tooth cleared
    // a fixed 10%-amplitude cliff, an unproven threshold: a fundamental at 8% of
    // the loudest harmonic fell just under it and was certified an octave high at
    // confidence 0.994, while a 12% fundamental resolved correctly. Straddle that
    // old cliff — a fundamental at 8% and at 12% of a loud 2nd harmonic must BOTH
    // resolve to f0, and in no case become a confident octave error.
    const int n = 1 << 15;
    const double f0 = 220.0;
    for (double frac : {0.08, 0.12}) {
        const auto x = make_partials(f0, n, {{1, frac}, {2, 1.0}});
        const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
        INFO("fundamental at " << frac << " of the 2nd harmonic, est = "
                               << est.hz << " conf = " << est.confidence);
        REQUIRE(est.voiced);
        REQUIRE(std::abs(cents_between(est.hz, f0)) < 2.0);
        // The specific failure the fix removes: a confident lock an octave up.
        REQUIRE(std::abs(cents_between(est.hz, 2.0 * f0)) > 100.0);
    }
}

TEST_CASE("Pitch estimator reports the loudest partial for a missing fundamental",
          "[audio][pitch]") {
    // Documented residual (see pitch_track.hpp): a signal with energy only at
    // 2·f0 and 3·f0 has no tone at f0 to lock onto, so the estimate honestly
    // reports the loudest PRESENT partial (2·f0) rather than fabricating the
    // absent virtual fundamental. Pin that behavior so it never silently drifts
    // into a fabricated f0.
    const int n = 1 << 15;
    const double f0 = 220.0;
    const auto x = make_partials(f0, n, {{2, 1.0}, {3, 0.6}});
    const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
    if (est.voiced) {
        // A real, present partial (the loudest, 2·f0) — never the absent f0.
        REQUIRE(std::abs(cents_between(est.hz, 2.0 * f0)) < 2.0);
        REQUIRE(std::abs(cents_between(est.hz, f0)) > 100.0);
    }
}

TEST_CASE("f0(t) trajectory recovers a known linear glide", "[audio][pitch]") {
    // A 220 -> 440 Hz linear glide over ~1.37 s. The extracted trajectory must
    // follow it: every voiced frame lands within a few cents of the true
    // instantaneous frequency at the frame's center time.
    const double f0 = 220.0, f1 = 440.0;
    const int n = 1 << 16;
    std::vector<float> x(static_cast<std::size_t>(n));
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double f = f0 + (f1 - f0) * (static_cast<double>(i) / n);
        phase += 2.0 * kPi * f / kSampleRate;
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(phase));
    }

    PitchTrackOptions opts;
    opts.window_length = 4096;
    opts.hop_length = 2048;
    const PitchTrack track = track_pitch(std::span<const float>(x), kSampleRate, opts);

    REQUIRE(track.points.size() > 8);
    const auto voiced = track.voiced_hz();
    // Essentially every frame of a clean glide should be voiced.
    REQUIRE(voiced.size() >= track.points.size() - 1);

    double worst_cents = 0.0;
    for (const auto& p : track.points) {
        if (!p.voiced)
            continue;
        const double f_true = f0 + (f1 - f0) * (p.time_s * kSampleRate / n);
        worst_cents = std::max(worst_cents, std::abs(cents_between(p.hz, f_true)));
    }
    INFO("worst |error| across the glide = " << worst_cents << " cents");
    REQUIRE(worst_cents < 3.0);
}

TEST_CASE("f0(t) trajectory recovers a known sinusoidal vibrato",
          "[audio][pitch]") {
    // Vibrato: f(t) = 440 + 20·sin(2π·5·t). The trajectory must recover the
    // center pitch and the modulation depth (a windowed estimator slightly
    // under-reads the extremes, so the depth tolerance is one-sided-loose).
    const double center = 440.0, depth = 20.0, rate = 5.0;
    const int n = 1 << 16;
    std::vector<float> x(static_cast<std::size_t>(n));
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const double f = center + depth * std::sin(2.0 * kPi * rate * t);
        phase += 2.0 * kPi * f / kSampleRate;
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(phase));
    }

    PitchTrackOptions opts;
    opts.window_length = 2048;
    opts.hop_length = 512;
    const PitchTrack track = track_pitch(std::span<const float>(x), kSampleRate, opts);

    const auto voiced = track.voiced_hz();
    REQUIRE(voiced.size() > 16);
    double lo = voiced.front(), hi = voiced.front(), sum = 0.0;
    for (double hz : voiced) {
        lo = std::min(lo, hz);
        hi = std::max(hi, hz);
        sum += hz;
    }
    const double mean = sum / static_cast<double>(voiced.size());
    const double recovered_depth = 0.5 * (hi - lo);
    INFO("mean = " << mean << " recovered depth = " << recovered_depth);
    REQUIRE_THAT(mean, WithinAbs(center, 4.0));
    // Recovered depth sits below the true depth (window averaging) but must be
    // clearly present and not overshoot.
    REQUIRE(recovered_depth > 0.7 * depth);
    REQUIRE(recovered_depth < 1.1 * depth);
}

TEST_CASE("Pitch estimator resolves a DCO-style quantization error",
          "[audio][pitch]") {
    // A DCO produces f_clk / N for integer N, so its achievable pitch is
    // quantized. Linearizing the divider rounding bounds the worst-case error by
    // |e| <= 865.617·f_note/f_clk
    // cents. With f_note = 440 Hz and f_clk = 1 MHz the nearest divider gives
    // ~-0.21 cents of detuning; the estimator must measure that few-cent error,
    // not round it away — that sensitivity is what gates the DCO pitch table.
    const double f_note = 440.0;
    const double f_clk = 1.0e6;
    const double divider = std::round(f_clk / f_note);
    const double f_dco = f_clk / divider;
    const double true_error_cents = cents_between(f_dco, f_note);
    const double quantization_bound_cents = 865.617 * f_note / f_clk;

    // Sanity: the physical DCO error obeys the divider-quantization bound.
    REQUIRE(std::abs(true_error_cents) <= quantization_bound_cents);
    // The quantization error is real (a few tenths of a cent), not zero.
    REQUIRE(std::abs(true_error_cents) > 0.1);

    const int n = 1 << 16;
    const auto x = make_tone(f_dco, n);
    const auto est = estimate_pitch(std::span<const float>(x), kSampleRate);
    REQUIRE(est.voiced);
    const double measured_error_cents = cents_between(est.hz, f_note);
    INFO("true = " << true_error_cents << "c  measured = " << measured_error_cents
                   << "c  bound = " << quantization_bound_cents << "c");
    // The estimator recovers the DCO detuning to within a hundredth of a cent.
    REQUIRE_THAT(measured_error_cents, WithinAbs(true_error_cents, 0.02));
}

TEST_CASE("Pitch estimate is deterministic", "[audio][pitch]") {
    const int n = 1 << 15;
    const auto x = make_tone(333.0, n);
    const auto a = estimate_pitch(std::span<const float>(x), kSampleRate);
    const auto b = estimate_pitch(std::span<const float>(x), kSampleRate);
    REQUIRE(a.voiced);
    REQUIRE(a.hz == b.hz);
    REQUIRE(a.confidence == b.confidence);
}
