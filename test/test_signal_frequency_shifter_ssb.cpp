// SsbFrequencyShifterT — the Bode/Moog single-sideband frequency shifter.
//
// This is the spec's acceptance suite T1–T10. Expected values are COMPUTED
// here from the shipped constants, never restated as literals, so retuning a
// design parameter fails the test that documents it rather than quietly
// disagreeing with it.
//
// Measurement recipe. fs = 48 kHz; the analysis window is 96000 samples —
// exactly 2 s, so its DFT bin spacing is exactly 0.5 Hz and EVERY test
// frequency in this file is a whole multiple of it. That is the entire reason
// for the choice: a coherent DFT over a whole number of periods has zero
// leakage, so a magnitude read at 445 Hz contains nothing of the 885 Hz tone
// sitting beside it, and a magnitude read at 442.5 Hz — the answer a PITCH
// shifter would give — contains nothing of the 445 Hz tone either. With a
// windowed FFT that second measurement is worth about −18 dB of Dirichlet
// leakage from its neighbour, which is enough to make a correct implementation
// look like it produced a peak where it did not.
//
// A coherent read at exactly the expected frequency also pins the frequency far
// more tightly than a peak-bin search would. A tone 0.73 Hz off (one bin at the
// 65536-sample length the spec suggests) measured over 2 s reads back at 4.7 %
// of its amplitude, so "the magnitude here equals the input amplitude" is
// already a much stronger statement than "the largest bin is this one" — and it
// costs one DFT rather than a full spectrum.
//
// SPEC DEVIATIONS, each argued at the test that makes it:
//   T3  the spec's fixed +250 Hz shift puts the image at DC for its own
//       f0 = 250 Hz sweep point and below zero for f0 = 100 Hz; the shift is
//       50 Hz here so every sweep point has a measurable image.
//   T3  the spec's [20 Hz, 0.45·fs] band is not a property a fixed coefficient
//       table can have at every sample rate; the band edge is normalised and
//       the test computes it from the shipped constant.
//   T7  `latency_samples()` returns `int`, matching the rest of the library,
//       rather than the spec's `double`. The value is 0 either way.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/frequency_shifter_ssb.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Shifter = SsbFrequencyShifter64;
using Network = HilbertQuadratureNetwork;
using Mode = FrequencyShiftMode;

constexpr double kSr = 48000.0;
constexpr int kAnalysisLen = 96000;  // exactly 2 s => 0.5 Hz bins
constexpr int kSettle = 24000;       // 0.5 s; the slowest pole's tau is ~1600
constexpr double kBinHz = kSr / static_cast<double>(kAnalysisLen);
constexpr double kTestAmplitude = 0.5;  // −6 dBFS, the spec's measurement level

/// Coherent DFT magnitude at `hz`. Exact — no window, no correction — as long
/// as `hz` is a whole multiple of `kBinHz`, which every call site checks.
double magnitude_at(const std::vector<double>& x, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

TEST_CASE("SSB rejects non-finite controls and audio without poisoning state",
          "[signal][frequency-shifter][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        Shifter a, b;
        for (auto* s : {&a, &b}) {
            s->prepare(kSr); s->set_shift_hz(317.0); s->set_feedback(0.37);
            s->set_feedback_delay_ms(13.0); s->set_mix(0.73); s->set_stereo_spread(0.41);
        }
        a.set_shift_hz(bad); a.set_feedback(bad); a.set_feedback_delay_ms(bad);
        a.set_mix(bad); a.set_stereo_spread(bad);
        REQUIRE(a.shift_hz() == b.shift_hz());
        for (int i = 0; i < 64; ++i) REQUIRE(a.process(0.2) == b.process(0.2));
        REQUIRE(a.process(bad) == 0.0);
        b.reset();
        for (int i = 0; i < 64; ++i) REQUIRE(a.process(0.2) == b.process(0.2));
    }
}

/// Guards the recipe itself: a frequency that is not on a bin makes every
/// magnitude read above leaky, and the failure looks like a DSP bug.
bool on_bin(double hz) {
    const double bins = std::abs(hz) / kBinHz;
    return std::abs(bins - std::round(bins)) < 1e-9;
}

double rms(const std::vector<double>& x) {
    double sum = 0.0;
    for (double v : x) sum += v * v;
    return std::sqrt(sum / static_cast<double>(x.size()));
}

double peak(const std::vector<double>& x) {
    double m = 0.0;
    for (double v : x) m = std::max(m, std::abs(v));
    return m;
}

/// Renders `settle + analysis` samples of a caller-supplied signal and returns
/// the analysis window.
template <typename ShifterType, typename Signal>
std::vector<double> render(ShifterType& shifter, Signal&& signal, int length = kAnalysisLen) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(length));
    for (int n = 0; n < kSettle + length; ++n) {
        const double y = shifter.process(signal(n));
        if (n >= kSettle) out.push_back(y);
    }
    return out;
}

auto sine(double hz, double amplitude = kTestAmplitude) {
    return [hz, amplitude](int n) {
        return amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kSr);
    };
}

/// A shifter prepared at the measurement rate with the feedback path idle, so
/// what is measured is the feed-forward SSB path alone.
template <typename ShifterType = Shifter>
ShifterType make_shifter(double shift_hz, Mode mode = Mode::up) {
    ShifterType s;
    s.prepare(kSr);
    s.set_mode(mode);
    s.set_shift_hz(shift_hz);
    s.set_feedback(0.0);
    s.set_mix(1.0);
    s.reset();
    return s;
}

/// The image-rejection identity of single-sideband theory, used in both
/// directions below: with an exactly-allpass quadrature pair carrying phase
/// error `theta` from the ideal quarter cycle, the retained sideband has
/// amplitude |cos(theta/2)| and the unwanted one |sin(theta/2)|.
double rejection_db_from_theta(double theta) {
    return -20.0 * std::log10(std::abs(std::tan(0.5 * theta)));
}

double theta_from_rejection_db(double db) {
    return 2.0 * std::atan(std::pow(10.0, -db / 20.0));
}

/// Measures the network's quadrature phase error at normalised frequency
/// `j / length`, which is an exact whole number of periods in the window.
double network_theta(int j, int length = 65536, int skip = 40000) {
    Network net;
    const double w = 2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(length);
    double ire = 0.0, iim = 0.0, qre = 0.0, qim = 0.0;
    for (int n = 0; n < skip + length; ++n) {
        const auto out = net.process(std::sin(w * static_cast<double>(n)));
        if (n < skip) continue;
        const double c = std::cos(w * static_cast<double>(n));
        const double s = std::sin(w * static_cast<double>(n));
        ire += out.in_phase * c;
        iim += out.in_phase * s;
        qre += out.quadrature * c;
        qim += out.quadrature * s;
    }
    // This accumulation's sign convention is the mirror of the textbook
    // e^{-jwn} transform, so the quarter cycle the quadrature branch LAGS by
    // reads as +pi/2 here. Only the deviation matters, and it is signless.
    double error = (std::atan2(qim, qre) - std::atan2(iim, ire)) - 0.5 * std::numbers::pi;
    while (error > std::numbers::pi)
        error -= 2.0 * std::numbers::pi;
    while (error < -std::numbers::pi)
        error += 2.0 * std::numbers::pi;
    return std::abs(error);
}

}  // namespace

// ── T1 — shift accuracy ───────────────────────────────────────────────────

TEST_CASE("T1 the shifter moves a tone by exactly the requested hertz",
          "[signal][frequency-shifter][ssb]") {
    constexpr double kTone = 1000.0;
    for (double shift : {1.0, 5.0, 250.0, -250.0, 2000.0}) {
        for (Mode mode : {Mode::up, Mode::down}) {
            // `up` keeps f + shift, `down` keeps f − shift; the sign of the
            // shift itself composes with that, which is why the expectation is
            // computed rather than tabulated.
            // The spec's own grid crosses zero: `down` at +2000 Hz asks for a
            // tone at −1000 Hz, which is not a place a real signal can be. It
            // reflects to +1000 Hz — the documented fold-over — so the
            // expectation is the reflected frequency, not the signed one.
            const double signed_expected = kTone + (mode == Mode::up ? shift : -shift);
            const double expected = std::abs(signed_expected);
            REQUIRE(on_bin(expected));

            auto shifter = make_shifter(shift, mode);
            const auto out = render(shifter, sine(kTone));

            INFO("shift " << shift << " Hz, mode " << (mode == Mode::up ? "up" : "down"));
            // Essentially all of the input's amplitude arrives at the expected
            // frequency. Two per cent covers the DC blocker's sub-0.1 % boost
            // and the residual image; a tone even one 0.73 Hz bin away would
            // read back at 4.7 % of amplitude and fail by a mile.
            REQUIRE_THAT(magnitude_at(out, expected), WithinRel(kTestAmplitude, 0.02));

            // And nothing is left at the input frequency: this is a shift, not
            // a detune laid alongside the original. Skipped where the reflected
            // sideband lands back ON the input frequency, which is the point of
            // the fold-over test below rather than a failure of this one.
            if (std::abs(expected - kTone) > 2.0 * kBinHz)
                REQUIRE(magnitude_at(out, kTone) < kTestAmplitude * 0.02);
        }
    }
}

TEST_CASE("T1 a shift is additive in hertz across the whole span",
          "[signal][frequency-shifter][ssb]") {
    // The property T1 exists to protect, stated over the range rather than at a
    // point: the same shift applied to different tones moves each by the SAME
    // number of hertz, which is what distinguishes it from a ratio.
    constexpr double kShift = 250.0;
    for (double tone : {200.0, 1000.0, 5000.0}) {
        auto shifter = make_shifter(kShift);
        const auto out = render(shifter, sine(tone));
        INFO("tone " << tone << " Hz");
        REQUIRE_THAT(magnitude_at(out, tone + kShift), WithinRel(kTestAmplitude, 0.02));
    }
}

TEST_CASE("a shift past zero reflects rather than vanishing",
          "[signal][frequency-shifter][ssb]") {
    // Documented, intended, and not a bug (spec section 5): content shifted
    // below 0 Hz comes back up as its own reflection. There is no oversampling
    // fix, because the operation is linear and generates nothing to alias —
    // what crosses the band edge is the shifted spectrum itself.
    constexpr double kTone = 1000.0;
    auto shifter = make_shifter(2000.0, Mode::down);
    const auto out = render(shifter, sine(kTone));
    // 1000 − 2000 = −1000, reflected to +1000: the output lands back on the
    // input frequency at full amplitude.
    REQUIRE_THAT(magnitude_at(out, 1000.0), WithinRel(kTestAmplitude, 0.02));
    // And the OTHER sideband (the image, at 1000 + 2000) is still suppressed —
    // the reflection is the wanted sideband arriving folded, not the network
    // giving up.
    const double db = 20.0 * std::log10(magnitude_at(out, 1000.0) /
                                        magnitude_at(out, 3000.0));
    REQUIRE(db >= Network::kImageRejectDb);

    // The same thing at the top of the band. The shift is the module's own
    // ceiling rather than a chosen number, so this cannot silently become a
    // test of the clamp: 20 kHz + kMaxShiftHz asks for 25 kHz, above Nyquist,
    // which folds to fs − 25 kHz = 23 kHz.
    const double top = 20000.0;
    const double asked = top + Shifter::kMaxShiftHz;
    REQUIRE(asked > 0.5 * kSr);
    auto up = make_shifter(Shifter::kMaxShiftHz, Mode::up);
    const auto folded = render(up, sine(top));
    REQUIRE_THAT(magnitude_at(folded, kSr - asked), WithinRel(kTestAmplitude, 0.05));

    // The ceiling is a clamp, not a wrap: asking for more than kMaxShiftHz
    // gives kMaxShiftHz.
    Shifter clamped;
    clamped.prepare(kSr);
    clamped.set_shift_hz(4.0 * Shifter::kMaxShiftHz);
    REQUIRE_THAT(clamped.shift_hz(), WithinRel(Shifter::kMaxShiftHz, 1e-12));
    clamped.set_shift_hz(-4.0 * Shifter::kMaxShiftHz);
    REQUIRE_THAT(clamped.shift_hz(), WithinRel(-Shifter::kMaxShiftHz, 1e-12));
}

// ── T2 — a shifter is not a pitch shifter ─────────────────────────────────

TEST_CASE("T2 shifting destroys the harmonic ratio a pitch shift would keep",
          "[signal][frequency-shifter][ssb]") {
    // 440 + 880 in, +5 Hz shift. A frequency shifter gives 445 and 885, whose
    // ratio is 1.989 — inharmonic. A PITCH shifter of the same +5 Hz at the
    // fundamental would give 442.5 and 885, ratio 2, still harmonic. 445 vs
    // 442.5 is the whole distinction and it is asserted in both directions.
    constexpr double kFundamental = 440.0;
    constexpr double kOctave = 2.0 * kFundamental;
    constexpr double kShift = 5.0;
    // What a PITCH shifter would answer. It multiplies, so the ratio that puts
    // the octave partial at 885 Hz — the same place the frequency shifter puts
    // it — puts the fundamental at 440·(885/880) = 442.5 Hz rather than 445.
    // That 2.5 Hz is the entire difference between the two effects, and it is
    // five analysis bins wide, so it is measurable rather than argued.
    const double pitch_answer = kFundamental * ((kOctave + kShift) / kOctave);
    REQUIRE_THAT(pitch_answer, WithinAbs(442.5, 1e-9));

    REQUIRE(on_bin(kFundamental + kShift));
    REQUIRE(on_bin(kOctave + kShift));
    REQUIRE(on_bin(pitch_answer));

    auto shifter = make_shifter(kShift);
    const auto out = render(shifter, [](int n) {
        const double t = 2.0 * std::numbers::pi * static_cast<double>(n) / kSr;
        return 0.5 * kTestAmplitude * (std::sin(kFundamental * t) + std::sin(kOctave * t));
    });

    const double each = 0.5 * kTestAmplitude;
    REQUIRE_THAT(magnitude_at(out, kFundamental + kShift), WithinRel(each, 0.03));
    REQUIRE_THAT(magnitude_at(out, kOctave + kShift), WithinRel(each, 0.03));

    // No peak within two bins of the pitch-shifter answer.
    for (int offset = -2; offset <= 2; ++offset)
        REQUIRE(magnitude_at(out, pitch_answer + offset * kBinHz) < each * 0.05);

    // And the ratio really did move off 2:1.
    const double ratio = (kOctave + kShift) / (kFundamental + kShift);
    REQUIRE(ratio < 2.0);
    REQUIRE_THAT(ratio, WithinAbs(885.0 / 445.0, 1e-9));
}

// ── T3 — image rejection ──────────────────────────────────────────────────

TEST_CASE("T3 the shipped table holds the image-rejection floor across its band",
          "[signal][frequency-shifter][ssb]") {
    // The coefficient table's contract measured directly on the network, which
    // is where the whole image-rejection budget is spent (the carrier is exact
    // and contributes none of it). The band is NORMALISED: a fixed allpass
    // table is a function of w = 2*pi*f/fs alone, so it has no absolute band
    // edge in hertz, and the spec's "[20 Hz, 0.45·fs] at every sample rate" is
    // not a property any single table can have — at 96 kHz an absolute 20 Hz
    // edge is 4.6x lower in normalised terms than at 44.1 kHz. The edge is a
    // shipped constant and the sweep is computed from it.
    constexpr int kLength = 65536;
    const int first = static_cast<int>(
        std::ceil(Network::kBandEdgeNormalized * static_cast<double>(kLength)));
    const int last = kLength / 4;  // fs/4, the symmetry point of the design

    std::vector<int> bins;
    for (int j = first; j <= first + 90; ++j) bins.push_back(j);  // dense at the edge
    for (double x = first; x <= static_cast<double>(last); x *= 1.06)
        bins.push_back(static_cast<int>(std::lround(x)));
    bins.push_back(last);

    const double floor_theta = theta_from_rejection_db(Network::kImageRejectDb);
    double worst_db = 1e9;
    int worst_bin = 0;
    for (int j : bins) {
        const double theta = network_theta(j, kLength);
        const double db = rejection_db_from_theta(theta);
        if (db < worst_db) {
            worst_db = db;
            worst_bin = j;
        }
        INFO("f/fs = " << static_cast<double>(j) / kLength);
        REQUIRE(theta <= floor_theta);
    }
    INFO("worst " << worst_db << " dB at f/fs = "
                  << static_cast<double>(worst_bin) / kLength);
    REQUIRE(worst_db >= Network::kImageRejectDb);

    // The design is symmetric about fs/4, so the upper half of the band is the
    // mirror of the swept half rather than a second measurement. Asserted at
    // the far edge so the claim is not taken on faith.
    REQUIRE_THAT(network_theta(first, kLength),
                 WithinAbs(network_theta(kLength / 2 - first, kLength), 1e-6));
}

TEST_CASE("T3 image rejection measured end to end through the shifter",
          "[signal][frequency-shifter][ssb]") {
    // The spec's f0 sweep, but at a 50 Hz shift rather than its 250 Hz. With
    // 250 Hz the image for its own f0 = 250 Hz sweep point lands at exactly
    // 0 Hz — where the DC blocker removes it and the ratio is a measurement of
    // nothing — and for f0 = 100 Hz it lands at −150 Hz and reflects, so the
    // recipe as written cannot be run at two of its six points. 50 Hz keeps
    // every image strictly inside the band. The rejection figure does not
    // depend on the shift: it is set by the network's phase error at the INPUT
    // frequency, which is what the sweep varies.
    constexpr double kShift = 50.0;
    auto measure_profile = [&]<typename ShifterType>() {
        for (double f0 : {100.0, 250.0, 500.0, 1000.0, 4000.0, 10000.0}) {
            REQUIRE(on_bin(f0 + kShift));
            REQUIRE(on_bin(f0 - kShift));

            auto shifter = make_shifter<ShifterType>(kShift);
            const auto out = render(shifter, sine(f0));
            const double desired = magnitude_at(out, f0 + kShift);
            const double image = magnitude_at(out, f0 - kShift);
            const double db = 20.0 * std::log10(desired / image);
            INFO("profile = " << static_cast<int>(ShifterType::kTrigProfile)
                               << ", f0 = " << f0 << " Hz, measured " << db << " dB");
            REQUIRE(db >= Network::kImageRejectDb);
        }
    };
    measure_profile.template operator()<Shifter>();
    measure_profile.template operator()<pulp::signal::PreciseSsbFrequencyShifter64>();
}

// ── T4 — DC and carrier suppression ───────────────────────────────────────

TEST_CASE("T4 DC in produces no carrier whistle, and silence in is silence out",
          "[signal][frequency-shifter][ssb]") {
    // Without the input DC blocker a constant offset is "shifted from 0 Hz" and
    // emerges as a bare tone at the shift frequency — an unbid whistle from a
    // signal with no audio in it at all. The blocker's corner is
    // kDcCornerHz, so a step decays as exp(−2*pi*fc*t) and is far below the
    // floor within the settle window.
    auto measure_profile = [&]<typename ShifterType>() {
        auto shifter = make_shifter<ShifterType>(250.0);
        const auto out = render(shifter, [](int) { return 0.5; });
        const double db = 20.0 * std::log10(std::max(rms(out), 1e-30));
        INFO("profile = " << static_cast<int>(ShifterType::kTrigProfile)
                           << ", residual " << db << " dBFS");
        REQUIRE(db <= -80.0);

        // Zero in, exactly zero out — not merely small. Every stage is linear
        // with zero-initialised state, so silence has nothing to excite.
        ShifterType silent = make_shifter<ShifterType>(250.0);
        silent.set_feedback(Shifter::kMaxFeedback);
        for (int n = 0; n < 4096; ++n) REQUIRE(silent.process(0.0) == 0.0);
    };
    measure_profile.template operator()<Shifter>();
    measure_profile.template operator()<pulp::signal::PreciseSsbFrequencyShifter64>();
}

TEST_CASE("the SSB trig profile is explicit at compile time",
          "[signal][frequency-shifter][ssb]") {
    STATIC_REQUIRE(Shifter::kTrigProfile == pulp::signal::FastTrigProfile::reference);
    STATIC_REQUIRE(pulp::signal::PreciseSsbFrequencyShifter64::kTrigProfile ==
                   pulp::signal::FastTrigProfile::realtime_precise);
}

// ── T5 — determinism ──────────────────────────────────────────────────────

TEST_CASE("T5 a render is bit-identical after reset", "[signal][frequency-shifter][ssb]") {
    // Series law 2. There is no generator in this module to seed — the only
    // randomness anywhere near it is this test's own input signal, which is
    // itself seeded.
    auto automation = [](int n) {
        return 400.0 * std::sin(2.0 * std::numbers::pi * 0.37 * n / kSr) - 60.0;
    };

    // The rewind is part of what is under test, so `run` performs it: setting
    // the automation's value at n = 0 BEFORE the reset is what makes the
    // smoothers start from the same place on both passes. A reset that left a
    // smoother mid-ramp would show up here as a first pass that differs.
    auto run = [automation]<typename ShifterType>(ShifterType& shifter) {
        constexpr int length = static_cast<int>(4.0 * kSr);
        shifter.set_shift_hz(automation(0));
        shifter.reset();
        Xorshift32 rng(0x51ED2701u);
        double pink = 0.0;
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(length));
        for (int n = 0; n < length; ++n) {
            // A one-pole over white noise: broadband, but weighted toward the
            // low end where the network's phase error is largest.
            pink = 0.97 * pink + 0.03 * rng.next_bipolar<double>();
            shifter.set_shift_hz(automation(n));
            out.push_back(shifter.process(0.5 * pink));
        }
        return out;
    };

    auto validate_profile = [&]<typename ShifterType>() {
        ShifterType shifter;
        shifter.prepare(kSr);
        shifter.set_feedback(0.6);
        shifter.set_feedback_delay_ms(8.0);
        const auto first = run(shifter);
        const auto second = run(shifter);

        REQUIRE(first.size() == second.size());
        for (std::size_t n = 0; n < first.size(); ++n) REQUIRE(first[n] == second[n]);
        // Not vacuous: the render has to have contained something.
        REQUIRE(peak(first) > 0.01);
    };
    validate_profile.template operator()<Shifter>();
    validate_profile.template operator()<pulp::signal::PreciseSsbFrequencyShifter64>();
}

// ── T6 — barberpole feedback stays bounded ────────────────────────────────

TEST_CASE("T6 the feedback loop stays bounded at the feedback ceiling",
          "[signal][frequency-shifter][ssb]") {
    auto validate_profile = []<typename ShifterType>() {
        ShifterType shifter;
        shifter.prepare(kSr);
        shifter.set_shift_hz(2.0);
        shifter.set_feedback_delay_ms(8.0);
        shifter.set_feedback(ShifterType::kMaxFeedback);
        shifter.set_mix(1.0);
        shifter.reset();

        const int burst = static_cast<int>(0.1 * kSr);
        const int tail = static_cast<int>(10.0 * kSr);
        double worst = 0.0;
        bool all_finite = true;
        for (int n = 0; n < burst + tail; ++n) {
            const double x = n < burst
                                 ? kTestAmplitude *
                                       std::sin(2.0 * std::numbers::pi * 1000.0 * n / kSr)
                                 : 0.0;
            const double y = shifter.process(x);
            all_finite = all_finite && std::isfinite(y);
            worst = std::max(worst, std::abs(y));
        }
        REQUIRE(all_finite);
        INFO("profile = " << static_cast<int>(ShifterType::kTrigProfile) << ", peak "
                           << worst << " against envelope " << ShifterType::worst_case_gain());
        REQUIRE(worst <= ShifterType::worst_case_gain());
        // The loop has to have rung, or the bound is being met by silence.
        REQUIRE(worst > kTestAmplitude);
    };
    validate_profile.template operator()<Shifter>();
    validate_profile.template operator()<pulp::signal::PreciseSsbFrequencyShifter64>();
}

// ── T7 — latency ──────────────────────────────────────────────────────────

TEST_CASE("T7 the module reports zero latency and its impulse starts at sample 0",
          "[signal][frequency-shifter][ssb]") {
    auto shifter = make_shifter(250.0);
    REQUIRE(shifter.latency_samples() == 0);
    // The in-phase branch carries no bulk delay, so the first output sample is
    // already non-zero. The quadrature branch's one-sample delay is inside the
    // network, not in front of it.
    REQUIRE(std::abs(shifter.process(1.0)) > 1e-3);

    // Group delay IS frequency-dependent — that is dispersion, and reporting it
    // as latency would be wrong. Cross-checked as a bounded, non-zero spread
    // rather than asserted away: an allpass network moves phase, so the energy
    // of an impulse is spread over the next few dozen samples.
    double energy = 0.0;
    for (int n = 0; n < 256; ++n) {
        const double y = shifter.process(0.0);
        energy += y * y;
    }
    REQUIRE(energy > 0.0);
}

// ── T8 — loop-gain envelope ───────────────────────────────────────────────

TEST_CASE("T8 the retained-sideband gain stays inside its budget",
          "[signal][frequency-shifter][ssb]") {
    // The measurement that turns "G_shift <= kGshiftBudget" from an assumption
    // into a fact about the shipped coefficient table. Both Hilbert branches
    // are exactly allpass, so the combine's own gain is |cos(theta/2)| <= 1 and
    // the only element in the loop that can exceed unity is the DC blocker,
    // whose supremum is 2/(1 + p) at Nyquist. That is what the budget buys.
    constexpr double kShift = 50.0;
    double worst = 0.0;
    for (double f0 : {50.0, 200.0, 1000.0, 5000.0, 12000.0, 20000.0}) {
        REQUIRE(on_bin(f0 + kShift));
        auto shifter = make_shifter(kShift);
        const auto out = render(shifter, sine(f0));
        worst = std::max(worst, magnitude_at(out, f0 + kShift) / kTestAmplitude);
    }
    INFO("measured G_shift " << worst);
    REQUIRE(worst <= Shifter::kGshiftBudget);

    // The predicted DC-blocker supremum, from the shipped corner and rate.
    const double pole = std::exp(-2.0 * std::numbers::pi * Shifter::kDcCornerHz / kSr);
    const double dc_supremum = 2.0 / (1.0 + pole);
    REQUIRE(worst <= dc_supremum * 1.001);
    REQUIRE(dc_supremum <= Shifter::kGshiftBudget);
}

TEST_CASE("T8 the loop impulse peak stays under the analytic envelope",
          "[signal][frequency-shifter][ssb]") {
    const double pole = std::exp(-2.0 * std::numbers::pi * Shifter::kDcCornerHz / kSr);
    const double g_shift = 2.0 / (1.0 + pole);  // the measured bound from above

    for (double g : {0.5, 0.7, Shifter::kMaxFeedback}) {
        for (double shift : {0.0, 5.0, 200.0}) {
            Shifter shifter;
            shifter.prepare(kSr);
            shifter.set_shift_hz(shift);
            shifter.set_feedback_delay_ms(8.0);
            shifter.set_feedback(g);
            shifter.set_mix(1.0);
            shifter.reset();

            double worst = 0.0;
            const int length = static_cast<int>(10.0 * kSr);
            for (int n = 0; n < length; ++n)
                worst = std::max(worst, std::abs(shifter.process(n == 0 ? 1.0 : 0.0)));

            const double envelope = 1.0 / (1.0 - g * g_shift);
            INFO("g = " << g << ", shift = " << shift << ", peak " << worst << " vs "
                        << envelope);
            REQUIRE(worst <= envelope);
        }
    }
}

TEST_CASE("T8 the registered worst-case gain is the envelope at the ceiling",
          "[signal][frequency-shifter][ssb]") {
    // Series law 8: the registry cites a tested invariant, not an estimate.
    const double expected = 1.0 / (1.0 - Shifter::kMaxFeedback * Shifter::kGshiftBudget);
    REQUIRE_THAT(Shifter::worst_case_gain(), WithinRel(expected, 1e-12));
    REQUIRE(Shifter::kMaxFeedback * Shifter::kGshiftBudget < 1.0);
}

// ── T9 — real-time allocation probe ───────────────────────────────────────

TEST_CASE("T9 nothing on the audio path allocates after prepare",
          "[signal][frequency-shifter][ssb][rt-safety]") {
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.reset();

    pulp::test::RtAllocationProbe probe;
    for (int n = 0; n < 4096; ++n) {
        shifter.set_shift_hz(-2000.0 + static_cast<double>(n));
        shifter.set_feedback(0.5 + 0.4 * std::sin(0.01 * n));
        shifter.set_feedback_delay_ms(0.1 + 0.01 * static_cast<double>(n % 4000));
        shifter.set_mix(0.5);
        shifter.set_stereo_spread(0.25);
        shifter.set_mode(static_cast<Mode>(n % 4));
        double left = 0.1, right = -0.1;
        shifter.process_stereo(left, right);
        shifter.process(0.05);
    }
    shifter.reset();
    REQUIRE(probe.allocation_count() == 0);
}

// ── T10 — smoothing ───────────────────────────────────────────────────────

TEST_CASE("T10 a fast shift sweep produces no step discontinuity",
          "[signal][frequency-shifter][ssb]") {
    constexpr double kTone = 500.0;
    constexpr double kTopShift = 2000.0;
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_mix(1.0);
    shifter.set_shift_hz(0.0);
    shifter.reset();

    const int length = static_cast<int>(1.0 * kSr);
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(length));
    for (int n = 0; n < length; ++n) {
        shifter.set_shift_hz(kTopShift * static_cast<double>(n) / static_cast<double>(length));
        out.push_back(shifter.process(sine(kTone)(n)));
    }

    // The physically-expected slew: the output is a sinusoid whose frequency
    // never exceeds kTone + kTopShift, so its sample-to-sample step is at most
    // A*2*pi*f/fs. The 10 % allowance covers the residual image riding along
    // and the DC blocker's sub-0.1 % boost.
    const double bound = 1.1 * kTestAmplitude * 2.0 * std::numbers::pi * (kTone + kTopShift) / kSr;
    double worst = 0.0;
    for (std::size_t n = 1; n < out.size(); ++n)
        worst = std::max(worst, std::abs(out[n] - out[n - 1]));
    INFO("worst step " << worst << " against bound " << bound);
    REQUIRE(worst <= bound);

    // Not vacuous: the sweep has to have gone somewhere. A shifter frozen at
    // 0 Hz would pass the bound above trivially.
    REQUIRE(peak(out) > 0.4 * kTestAmplitude);
}

// ── Stereo split (spec section 7's worked example) ────────────────────────

TEST_CASE("stereo split drives the channels apart in proportion to the spread",
          "[signal][frequency-shifter][ssb]") {
    constexpr double kTone = 440.0;
    constexpr double kShift = 5.0;
    for (double spread : {0.0, 0.5, 1.0}) {
        const double left_hz = kTone + spread * kShift;
        const double right_hz = kTone - spread * kShift;
        REQUIRE(on_bin(left_hz));
        REQUIRE(on_bin(right_hz));

        Shifter shifter;
        shifter.prepare(kSr);
        shifter.set_mode(Mode::stereo_split);
        shifter.set_shift_hz(kShift);
        shifter.set_stereo_spread(spread);
        shifter.set_mix(1.0);
        shifter.reset();

        std::vector<double> left, right;
        left.reserve(static_cast<std::size_t>(kAnalysisLen));
        right.reserve(static_cast<std::size_t>(kAnalysisLen));
        for (int n = 0; n < kSettle + kAnalysisLen; ++n) {
            double l = sine(kTone)(n), r = sine(kTone)(n);
            shifter.process_stereo(l, r);
            if (n >= kSettle) {
                left.push_back(l);
                right.push_back(r);
            }
        }

        INFO("spread " << spread);
        REQUIRE_THAT(magnitude_at(left, left_hz), WithinRel(kTestAmplitude, 0.03));
        REQUIRE_THAT(magnitude_at(right, right_hz), WithinRel(kTestAmplitude, 0.03));
        // Separation is linear in the spread: 2*spread*shift hertz apart.
        REQUIRE_THAT(left_hz - right_hz, WithinAbs(2.0 * spread * kShift, 1e-9));
    }
}

TEST_CASE("stereo processing keeps the two channels independent",
          "[signal][frequency-shifter][ssb]") {
    // Each rail has its own network, DC blocker and feedback line; only the
    // carrier is shared. Silence on the right must stay silent no matter what
    // the left is doing, or the two are crosstalking.
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_mode(Mode::dual_mono);
    shifter.set_shift_hz(250.0);
    shifter.set_feedback(0.8);
    shifter.reset();

    double right_energy = 0.0;
    for (int n = 0; n < 48000; ++n) {
        double l = std::sin(2.0 * std::numbers::pi * 1000.0 * n / kSr), r = 0.0;
        shifter.process_stereo(l, r);
        right_energy += std::abs(r);
    }
    REQUIRE(right_energy == 0.0);
}

// ── Tapers (spec section 9's worked examples) ─────────────────────────────

TEST_CASE("the shift taper is continuous, monotone, and hits its worked values",
          "[signal][frequency-shifter][ssb]") {
    using S = Shifter;
    // Every expectation computed from the shipped constants.
    REQUIRE_THAT(S::shift_hz_from_knob(0.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(S::shift_hz_from_knob(0.5 * S::kSplit),
                 WithinRel(0.5 * S::kLinZoneHz, 1e-12));
    REQUIRE_THAT(S::shift_hz_from_knob(S::kSplit), WithinRel(S::kLinZoneHz, 1e-12));
    REQUIRE_THAT(S::shift_hz_from_knob(1.0), WithinRel(S::kMaxShiftHz, 1e-12));

    // The log zone's own law: |df| = kLinZoneHz * (kMaxShiftHz/kLinZoneHz)^r.
    for (double r : {0.2, 0.5, 0.8}) {
        const double knob = S::kSplit + r * (1.0 - S::kSplit);
        const double expected =
            S::kLinZoneHz * std::pow(S::kMaxShiftHz / S::kLinZoneHz, r);
        INFO("r = " << r);
        REQUIRE_THAT(S::shift_hz_from_knob(knob), WithinRel(expected, 1e-9));
    }

    // Continuity at the crossover: both branches meet exactly, no jump for an
    // automation sweep to click on.
    REQUIRE_THAT(S::shift_hz_from_knob(S::kSplit - 1e-9),
                 WithinAbs(S::shift_hz_from_knob(S::kSplit + 1e-9), 1e-4));

    // Strictly increasing across the whole positive half, and odd-symmetric.
    double previous = -1.0;
    for (int i = 0; i <= 1000; ++i) {
        const double knob = static_cast<double>(i) / 1000.0;
        const double hz = S::shift_hz_from_knob(knob);
        REQUIRE(hz > previous);
        REQUIRE_THAT(S::shift_hz_from_knob(-knob), WithinAbs(-hz, 1e-12));
        previous = hz;
    }

    // Round trip.
    for (double knob : {0.05, 0.25, 0.5, 0.6, 0.75, 1.0, -0.4})
        REQUIRE_THAT(S::knob_from_shift_hz(S::shift_hz_from_knob(knob)),
                     WithinAbs(knob, 1e-9));
}

TEST_CASE("the feedback-delay taper is geometric across its travel",
          "[signal][frequency-shifter][ssb]") {
    using S = Shifter;
    REQUIRE_THAT(S::feedback_delay_ms_from_knob(0.0), WithinRel(S::kMinDelayMs, 1e-12));
    REQUIRE_THAT(S::feedback_delay_ms_from_knob(1.0), WithinRel(S::kMaxLoopMs, 1e-12));
    REQUIRE_THAT(S::feedback_delay_ms_from_knob(0.5),
                 WithinRel(std::sqrt(S::kMinDelayMs * S::kMaxLoopMs), 1e-12));
    // Equal knob steps buy equal RATIOS, which is the point of the law.
    const double a = S::feedback_delay_ms_from_knob(0.25) / S::feedback_delay_ms_from_knob(0.0);
    const double b = S::feedback_delay_ms_from_knob(0.5) / S::feedback_delay_ms_from_knob(0.25);
    REQUIRE_THAT(a, WithinRel(b, 1e-12));
}

// ── Behaviour the reconciled prototype used to get wrong ──────────────────

TEST_CASE("a positive shift moves energy up, not down", "[signal][frequency-shifter][ssb]") {
    // The regression this suite exists for. Of the four ways to assign the
    // delayed and undelayed allpass branches to I and Q, two hold a constant
    // quarter cycle across the band and they hold it with OPPOSITE sign — so
    // picking the wrong one produces a shifter that works perfectly and shifts
    // the wrong way. The prototype this module reconciles did exactly that.
    auto shifter = make_shifter(250.0);
    const auto out = render(shifter, sine(1000.0));
    REQUIRE(magnitude_at(out, 1250.0) > 10.0 * magnitude_at(out, 750.0));
}

TEST_CASE("reset clears state without clearing the filter design",
          "[signal][frequency-shifter][ssb]") {
    // The other regression. A network that loses its coefficients on reset
    // degenerates into a chain of two-sample delays, which turns the shifter
    // into a ring modulator: both sidebands, carrier suppressed. That passes
    // every "the original frequency is gone" test, so the assertion has to be
    // about the UNWANTED sideband specifically.
    auto shifter = make_shifter(250.0);
    const auto before = render(shifter, sine(1000.0));
    shifter.reset();
    const auto after = render(shifter, sine(1000.0));

    for (const auto& out : {before, after}) {
        const double rejection =
            20.0 * std::log10(magnitude_at(out, 1250.0) / magnitude_at(out, 750.0));
        REQUIRE(rejection >= Network::kImageRejectDb);
    }
    // And the two renders agree, because reset() is a true rewind.
    REQUIRE(before.size() == after.size());
    for (std::size_t n = 0; n < before.size(); ++n) REQUIRE(before[n] == after[n]);
}

TEST_CASE("a zero shift passes the signal through at full magnitude",
          "[signal][frequency-shifter][ssb]") {
    // At Df = 0 the carrier is constant and the output is the in-phase branch
    // alone — allpass, so magnitude is preserved even though phase is not.
    auto shifter = make_shifter(0.0);
    const auto out = render(shifter, sine(1000.0));
    REQUIRE_THAT(magnitude_at(out, 1000.0), WithinRel(kTestAmplitude, 0.01));
}

TEST_CASE("the sample rate only moves the band edge, not the shift accuracy",
          "[signal][frequency-shifter][ssb]") {
    // The normalised-band claim, stated where it can be checked: the SAME table
    // at 96 kHz shifts by the same number of hertz, and its band edge in hertz
    // is twice what it is at 48 kHz.
    constexpr double kHighRate = 96000.0;
    constexpr double kTone = 1000.0;
    constexpr double kShift = 250.0;

    Shifter shifter;
    shifter.prepare(kHighRate);
    shifter.set_shift_hz(kShift);
    shifter.set_mix(1.0);
    shifter.reset();

    const int length = static_cast<int>(kHighRate * 2.0);  // 2 s => 0.5 Hz bins
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(length));
    for (int n = 0; n < static_cast<int>(kHighRate) + length; ++n) {
        const double y = shifter.process(kTestAmplitude *
                                         std::sin(2.0 * std::numbers::pi * kTone * n / kHighRate));
        if (n >= static_cast<int>(kHighRate)) out.push_back(y);
    }
    const double w = 2.0 * std::numbers::pi * (kTone + kShift) / kHighRate;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < out.size(); ++n) {
        re += out[n] * std::cos(w * static_cast<double>(n));
        im += out[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(out.size());
    REQUIRE_THAT(std::hypot(re * scale, im * scale), WithinRel(kTestAmplitude, 0.02));

    REQUIRE_THAT(Network::band_low_hz(kHighRate),
                 WithinRel(2.0 * Network::band_low_hz(kSr), 1e-12));
}
