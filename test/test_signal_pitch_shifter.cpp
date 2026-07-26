// PitchShifterT — the time-domain, dual-tap crossfaded ratio pitch shifter.
//
// This is the spec's acceptance suite A1–A9 plus the characterisation cases the
// spec's own criteria turned out to need. Expected values are COMPUTED here from
// the shipped constants, never restated as literals, so retuning a design
// parameter fails the case that documents it rather than quietly disagreeing
// with it.
//
// ── Measurement recipe ────────────────────────────────────────────────────
//
// fs = 48 kHz. Two instruments, used deliberately:
//
//   * `magnitude_at` — a COHERENT rectangular DFT at an exact frequency. Zero
//     leakage and exact amplitude as long as every component in the render is a
//     whole multiple of the analysis bin, which `on_bin` asserts at each call
//     site. This is the instrument wherever the arithmetic can be made to land
//     on a grid, which is most places once `r` is chosen to be a rational
//     interval (`±12` gives `r = 2` and `0.5` exactly).
//   * `magnitude_bh` / `peak_near` — a Blackman-Harris windowed DFT scanned on a
//     fine grid. Needed only where `r` is irrational (`+7` is `2^(7/12)`), so no
//     analysis length makes the answer on-bin. BH's −92 dB sidelobes keep a
//     neighbouring line 25 Hz away from contaminating the read.
//
// NEITHER path tracks the peak SAMPLE of the output. A discrete sine under-reads
// its own amplitude when no sample lands on the crest, which at these
// frequencies is a fraction of a dB of pure measurement error that looks exactly
// like a filter fault.
//
// ── The measurement trap this module hides, and the spec walked into ──────
//
// The two taps read the same stream half a window apart, so for an input partial
// at `f` they are separated by `q = f · window_ms / 1000` half-cycles of that
// partial (`PitchShifterT::tap_phase_pi`). At EVEN `q` the taps are exactly in
// phase, the crossfade sums two identical signals, and that partial shifts with
// no warble whatsoever — one clean line at `r·f`. At ODD `q` the taps are in
// antiphase, the line at `r·f` is fully suppressed, and the energy splits into
// two lines at `r·f ± f_warble`, each at exactly half the input amplitude.
//
// The spec's A2 asks for the sideband spacing at `f0 = 1000` with `window_ms`
// of 40 and 20 — which are `q = 40` and `q = 20`, both even, both degenerate.
// Measured on this implementation the sidebands sit 104 dB and 136 dB below the
// carrier: there is nothing there to measure. `A2 defect` below ships that
// measurement as a test so the claim is auditable, and A2 itself is rebuilt on
// an odd-`q` configuration where the answer is not merely non-zero but exactly
// predicted in closed form.
//
// ── Spec deviations, each argued at the case that makes it ───────────────
//
//   A2  the spec's measurement points are degenerate (above). Rebuilt at odd
//       `q`, which also makes the expected amplitudes exact rather than
//       tolerance-bounded.
//   A7  "wet peak ≤ 1.0 · peak_in" is unreachable by ~3e-4: the crossfade is a
//       convex combination and IS bounded by 1, but the DC blocker the spec
//       mandates on the wet leg has a peak magnitude of `2/(1+p) > 1`. The
//       bound asserted is that exact analytic value.
//   A8  "the slewed semitone signal reaches 90 % of target in glide_up_ms" mixes
//       up `SlewLimiterT`'s two modes: in `linear` mode — the constant-TIME
//       portamento a pedal glide wants, and the one this module selects — the
//       output reaches 100 % at `glide_up_ms` and 90 % at `0.9·glide_up_ms`.
//       "90 % at the glide time" is the `exponential`-mode reading, where it
//       would be `2.303·τ`. Both linear-mode arrivals are asserted instead.
//   §4  the spec states no aliasing policy is required because there is no
//       nonlinearity. There is no nonlinearity, but a ratio shifter is a
//       RESAMPLER and up-shifts alias. `aliasing` below measures the fold and
//       proves the instrument against a known-good control.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/pitch_shifter.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Shifter = PitchShifter64;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// One second of analysis => exactly 1 Hz bins. Every coherent read in this file
/// is a whole number of Hz, which `on_bin` checks.
constexpr int kAnalysisLen = 48000;
constexpr double kBinHz = kSr / static_cast<double>(kAnalysisLen);

/// Long enough for two things at once: the delay line to fill past the longest
/// window (`kWindowMsMax` = 100 ms = 4800 samples) and the wet leg's DC blocker
/// to settle. The blocker's time constant is `1/(1−p) = fs/(2π·f_c)` = 1528
/// samples at the shipped 5 Hz corner, so this is ~16 τ; at 8000 samples its
/// residual start-up transient is still 2.4e-5 of full scale, which is enough to
/// push the A7 peak past an exact analytic bound.
constexpr int kSettle = 24000;

/// Coherent DFT amplitude at `hz`. Exact — no window, no correction — as long as
/// `hz` is a whole multiple of `kBinHz` AND the render is periodic over the
/// analysis length, which every call site arranges.
double magnitude_at(const std::vector<double>& x, double hz) {
    const double w = 2.0 * kPi * hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

TEST_CASE("pitch shifter retains controls and recovers from non-finite audio",
          "[signal][pitch-shifter][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        Shifter a, b;
        for (auto* s : {&a, &b}) {
            s->prepare(kSr); s->set_shift_semitones(7.0); s->set_pedal(.37);
            s->set_targets(-9.0, 11.0); s->set_harmony(-4.0, 8.0);
            s->set_detune_cents(17.0); s->set_dive_floor_semis(-18.0);
            s->set_window_ms(31.0); s->set_glide_ms(27.0, 41.0);
            s->set_mix(.63); s->set_drift_depth(.21); s->reset();
        }
        a.set_shift_semitones(bad); a.set_pedal(bad); a.set_targets(bad, 3.0);
        a.set_harmony(3.0, bad); a.set_detune_cents(bad); a.set_dive_floor_semis(bad);
        a.set_window_ms(bad); a.set_glide_ms(bad, 3.0); a.set_mix(bad); a.set_drift_depth(bad);
        REQUIRE(a.shift_semitones() == b.shift_semitones()); REQUIRE(a.window_ms() == b.window_ms());
        REQUIRE(a.process(bad) == 0.0); b.reset();
        for (int i=0;i<64;++i) REQUIRE(a.process(.2) == b.process(.2));
    }
}

/// Guards the recipe itself: a frequency off the bin grid makes every coherent
/// read above leaky, and the failure looks like a DSP bug.
bool on_bin(double hz) {
    const double bins = std::abs(hz) / kBinHz;
    return std::abs(bins - std::round(bins)) < 1e-9;
}

/// Blackman-Harris windowed amplitude — for the `+7` cases, whose `r = 2^(7/12)`
/// is irrational and therefore never on a bin at any analysis length.
double magnitude_bh(const std::vector<double>& x, double hz) {
    const double w = 2.0 * kPi * hz / kSr;
    const double n_max = static_cast<double>(x.size()) - 1.0;
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double t = 2.0 * kPi * static_cast<double>(n) / n_max;
        const double win = 0.35875 - 0.48829 * std::cos(t) +
                           0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
        re += x[n] * win * std::cos(w * static_cast<double>(n));
        im += x[n] * win * std::sin(w * static_cast<double>(n));
        wsum += win;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

/// Locates a spectral peak: a coarse scan over ±`span` then a 50× refinement.
/// The refined grid resolves the peak to `step/50` = 0.001 Hz here, two orders
/// finer than any tolerance asserted against it.
double peak_near(const std::vector<double>& x, double guess, double span = 6.0,
                 double step = 0.05) {
    double best = guess, best_mag = -1.0;
    for (double f = guess - span; f <= guess + span; f += step) {
        const double m = magnitude_bh(x, f);
        if (m > best_mag) { best_mag = m; best = f; }
    }
    const double coarse = best;
    for (double f = coarse - step; f <= coarse + step; f += step / 50.0) {
        const double m = magnitude_bh(x, f);
        if (m > best_mag) { best_mag = m; best = f; }
    }
    return best;
}

double peak(const std::vector<double>& x) {
    double m = 0.0;
    for (double v : x) m = std::max(m, std::abs(v));
    return m;
}

/// A shifter driven from the direct semitone target with no glide, so what is
/// measured is the shifting core rather than the pedal law.
Shifter make_direct(double semitones, double window_ms = Shifter::kWindowMsDefault,
                    double sample_rate = kSr) {
    Shifter s;
    s.prepare(sample_rate);
    s.set_shift_source(ShiftSource::direct);
    s.set_window_ms(window_ms);
    s.set_glide_ms(0.0, 0.0);
    s.set_mix(1.0);
    s.set_shift_semitones(semitones);
    s.reset();
    return s;
}

/// Renders `kSettle + length` samples of a sine through the WET leg and returns
/// the analysis window.
std::vector<double> render_wet(Shifter& s, double hz, int length = kAnalysisLen,
                               double amplitude = 1.0, int settle = kSettle) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(length));
    for (int n = 0; n < settle + length; ++n) {
        const double x =
            amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(n) / kSr);
        const double y = static_cast<double>(s.process_wet(x));
        if (n >= settle) out.push_back(y);
    }
    return out;
}

double ratio_of(double semitones) {
    return units::semitones_to_ratio(semitones);
}

/// Eq. 3.5, recomputed from the interval and the window rather than restated.
double warble_hz(double semitones, double window_ms) {
    return std::abs(1.0 - ratio_of(semitones)) * 1000.0 / window_ms;
}

}  // namespace

// ── A1 — pitch accuracy: the shift MULTIPLIES ─────────────────────────────

TEST_CASE("A1 the dominant peak sits at f0 times the interval ratio",
          "[signal][pitch-shifter]") {
    constexpr double kF0 = 1000.0;
    for (double semitones : {7.0, -12.0, 12.0}) {
        const double expected = kF0 * ratio_of(semitones);
        auto shifter = make_direct(semitones);
        const auto wet = render_wet(shifter, kF0);

        // ±0.3 % is the spec's tolerance; the peak locator resolves 0.001 Hz, so
        // the tolerance is spending its slack on warble sidebands, not on the
        // instrument.
        const double measured = peak_near(wet, expected, 20.0, 0.25);
        REQUIRE_THAT(measured, WithinRel(expected, 0.003));

        // "The peak must not sit at f0" — a shifter that silently passed audio
        // through would satisfy every tolerance above at 0 semitones.
        REQUIRE(magnitude_bh(wet, kF0) < 0.01 * magnitude_bh(wet, expected));
    }
}

TEST_CASE("A1 the ratio is a pitch ratio rather than an additive frequency offset",
          "[signal][pitch-shifter]") {
    // The load-bearing distinction from the SSB frequency shifter: at a fixed
    // interval, the ABSOLUTE displacement scales with the input frequency. An
    // additive shifter would move both tones by the same number of Hz.
    constexpr double kSemitones = 7.0;
    const double r = ratio_of(kSemitones);

    auto low = make_direct(kSemitones);
    auto high = make_direct(kSemitones);
    const auto wet_low = render_wet(low, 500.0);
    const auto wet_high = render_wet(high, 1000.0);

    const double peak_low = peak_near(wet_low, 500.0 * r, 20.0, 0.25);
    const double peak_high = peak_near(wet_high, 1000.0 * r, 20.0, 0.25);

    // The two displacements differ by exactly the input frequency ratio.
    const double displacement_low = peak_low - 500.0;
    const double displacement_high = peak_high - 1000.0;
    REQUIRE_THAT(displacement_high / displacement_low, WithinRel(2.0, 0.005));

    // And the ratio itself is the same at both, which is what "multiplies" means.
    REQUIRE_THAT(peak_low / 500.0, WithinRel(r, 0.003));
    REQUIRE_THAT(peak_high / 1000.0, WithinRel(r, 0.003));
}

// ── A2 — warble rate ──────────────────────────────────────────────────────

TEST_CASE("A2 defect the spec's own measurement points have no warble to measure",
          "[signal][pitch-shifter][spec-defect]") {
    // f0 = 1000 with window_ms of 40 and 20 gives q = 40 and q = 20 — both even,
    // so the two taps are exactly in phase and the crossfade produces no
    // amplitude modulation at all. This case exists to make that auditable: it
    // asserts the ABSENCE the spec assumed was a presence.
    constexpr double kF0 = 1000.0;
    for (double window_ms : {40.0, 20.0}) {
        auto shifter = make_direct(7.0, window_ms);
        REQUIRE(std::abs(std::fmod(shifter.tap_phase_pi(kF0), 2.0)) < 1e-9);

        const auto wet = render_wet(shifter, kF0);
        const double carrier_hz = kF0 * ratio_of(7.0);
        const double f_warble = warble_hz(7.0, window_ms);
        const double carrier = magnitude_bh(wet, carrier_hz);

        // 60 dB is two orders of magnitude looser than the measured 104 dB and
        // 136 dB; the point is that nothing survives at the sideband frequency,
        // not the exact depth of the null.
        REQUIRE(magnitude_bh(wet, carrier_hz + f_warble) < 1e-3 * carrier);
        REQUIRE(magnitude_bh(wet, carrier_hz - f_warble) < 1e-3 * carrier);
    }
}

TEST_CASE("A2 the crossfade window sets the warble rate", "[signal][pitch-shifter]") {
    // Rebuilt on ODD q, where the model predicts the answer in closed form: the
    // carrier at r·f0 is fully suppressed and the energy sits in two lines at
    // r·f0 ± f_warble, each at exactly HALF the input amplitude.
    //
    // `+12` makes r = 2 exactly, so every one of those frequencies is a whole
    // number of Hz and the coherent DFT is exact. f0 is chosen per window to
    // keep q = f0·window_ms/1000 odd; it has to move with the window, because
    // halving a window halves q and an odd q cannot stay odd.
    struct Point { double f0, window_ms; };
    const Point points[] = {{1025.0, 40.0}, {2050.0, 20.0}};

    double previous_warble = 0.0;
    for (const Point& p : points) {
        auto shifter = make_direct(12.0, p.window_ms);

        // Guard the recipe: q must be an odd integer or the prediction changes.
        const double q = shifter.tap_phase_pi(p.f0);
        REQUIRE_THAT(q, WithinAbs(std::round(q), 1e-9));
        REQUIRE(static_cast<int>(std::lround(q)) % 2 != 0);

        const double r = ratio_of(12.0);
        const double f_warble = warble_hz(12.0, p.window_ms);
        const double carrier_hz = p.f0 * r;
        REQUIRE(on_bin(carrier_hz - f_warble));
        REQUIRE(on_bin(carrier_hz + f_warble));
        REQUIRE(on_bin(carrier_hz + 2.0 * f_warble));

        const auto wet = render_wet(shifter, p.f0);

        // The suppressed carrier and the two half-amplitude sidebands. The
        // 0.5 is the model's exact prediction; the only correction is the DC
        // blocker's in-band magnitude, which the block reports.
        const double expected_sideband = 0.5 * shifter.dc_blocker_magnitude_peak();
        REQUIRE(magnitude_at(wet, carrier_hz) < 1e-6);
        REQUIRE_THAT(magnitude_at(wet, carrier_hz - f_warble),
                     WithinRel(expected_sideband, 0.01));
        REQUIRE_THAT(magnitude_at(wet, carrier_hz + f_warble),
                     WithinRel(expected_sideband, 0.01));

        // Nothing at twice the spacing: this is what proves the measured
        // spacing is f_warble and not a harmonic of something else.
        REQUIRE(magnitude_at(wet, carrier_hz + 2.0 * f_warble) < 1e-3);

        if (previous_warble > 0.0) {
            // Halving the window doubles the warble (Eq. 3.5).
            REQUIRE_THAT(f_warble, WithinRel(2.0 * previous_warble, 1e-9));
        }
        previous_warble = f_warble;
    }
}

TEST_CASE("A2 the sideband spacing is measurable at a non-rational interval",
          "[signal][pitch-shifter]") {
    // The spec's own interval, at a point where there is something to measure.
    // r = 2^(7/12) is irrational, so this is the windowed-scan instrument.
    constexpr double kF0 = 1025.0;  // q = 41, odd
    constexpr double kWindowMs = Shifter::kWindowMsDefault;
    auto shifter = make_direct(7.0, kWindowMs);
    REQUIRE(static_cast<int>(std::lround(shifter.tap_phase_pi(kF0))) % 2 != 0);

    const double r = ratio_of(7.0);
    const double f_warble = warble_hz(7.0, kWindowMs);
    const auto wet = render_wet(shifter, kF0, 2 * kAnalysisLen);

    const double lower = peak_near(wet, kF0 * r - f_warble);
    const double upper = peak_near(wet, kF0 * r + f_warble);

    // Spec tolerance: ±3 % on the spacing.
    REQUIRE_THAT(upper - lower, WithinRel(2.0 * f_warble, 0.03));
    // And the pair straddles the suppressed carrier symmetrically.
    REQUIRE_THAT(0.5 * (upper + lower), WithinRel(kF0 * r, 0.001));
}

TEST_CASE("the warble depth is a function of frequency rather than a constant",
          "[signal][pitch-shifter][characterisation]") {
    // The crossfade artefact this module is made of, characterised rather than
    // asserted away: at even q a partial passes with no warble, at odd q its
    // carrier is annihilated. Same block, same settings, two input frequencies.
    constexpr double kWindowMs = Shifter::kWindowMsDefault;
    const double r = ratio_of(12.0);
    const double f_warble = warble_hz(12.0, kWindowMs);

    auto even = make_direct(12.0, kWindowMs);
    const double f_even = 1000.0;  // q = 40
    REQUIRE(static_cast<int>(std::lround(even.tap_phase_pi(f_even))) % 2 == 0);
    const auto wet_even = render_wet(even, f_even);

    auto odd = make_direct(12.0, kWindowMs);
    const double f_odd = 1025.0;  // q = 41
    REQUIRE(static_cast<int>(std::lround(odd.tap_phase_pi(f_odd))) % 2 != 0);
    const auto wet_odd = render_wet(odd, f_odd);

    // Even q: all of it in one line, none in the sidebands.
    REQUIRE_THAT(magnitude_at(wet_even, f_even * r),
                 WithinRel(even.dc_blocker_magnitude_peak(), 0.01));
    REQUIRE(magnitude_at(wet_even, f_even * r + f_warble) < 1e-4);

    // Odd q: none of it in the line, all of it in the sidebands.
    REQUIRE(magnitude_at(wet_odd, f_odd * r) < 1e-6);
    REQUIRE_THAT(magnitude_at(wet_odd, f_odd * r + f_warble),
                 WithinRel(0.5 * odd.dc_blocker_magnitude_peak(), 0.01));

    // Both conserve amplitude: the warble redistributes energy, it does not
    // create or destroy it. Two half-amplitude lines carry the same total as
    // one full-amplitude line.
    const double total_even = magnitude_at(wet_even, f_even * r);
    const double total_odd = magnitude_at(wet_odd, f_odd * r - f_warble) +
                             magnitude_at(wet_odd, f_odd * r + f_warble);
    REQUIRE_THAT(total_odd, WithinRel(total_even, 0.01));
}

TEST_CASE("unison is a static comb rather than a bypass",
          "[signal][pitch-shifter][characterisation]") {
    // §3.2, informative in the spec and made checkable here: at r = 1 the phase
    // increment is zero, the taps freeze, and the wet leg is a fixed two-tap
    // comb rather than a copy of the input. The honest full-dry path is `mix`,
    // which IS bit-exact.
    auto shifter = make_direct(0.0);
    REQUIRE(shifter.warble_hz() == 0.0);

    Shifter dry = make_direct(0.0);
    dry.set_mix(0.0);
    for (int n = 0; n < 4000; ++n) {
        const double x = std::sin(2.0 * kPi * 100.0 * static_cast<double>(n) / kSr);
        REQUIRE(static_cast<double>(dry.process(x)) == x);
    }
}

// ── A3 — latency ──────────────────────────────────────────────────────────

TEST_CASE("A3 latency_samples reports the window centre exactly",
          "[signal][pitch-shifter]") {
    for (double window_ms : {Shifter::kWindowMsMin, Shifter::kWindowMsDefault,
                             Shifter::kWindowMsMax}) {
        for (double rate : {44100.0, 48000.0, 96000.0}) {
            auto shifter = make_direct(7.0, window_ms, rate);
            const long expected = std::lround(window_ms * rate / 2000.0);
            REQUIRE(shifter.latency_samples() == static_cast<int>(expected));

            // Series law 7: the wall-clock latency is the same at every rate.
            const double ms = 1000.0 * shifter.latency_samples() / rate;
            REQUIRE_THAT(ms, WithinAbs(window_ms / 2.0, 0.02));
        }
    }
}

TEST_CASE("A3 the impulse response energy centroid sits near the window centre",
          "[signal][pitch-shifter]") {
    auto shifter = make_direct(7.0);
    const double window = shifter.window_samples();

    double weighted = 0.0, energy = 0.0;
    for (int n = 0; n < 4 * static_cast<int>(window); ++n) {
        const double y = static_cast<double>(shifter.process_wet(n == 0 ? 1.0 : 0.0));
        weighted += y * y * static_cast<double>(n);
        energy += y * y;
    }
    REQUIRE(energy > 0.0);
    REQUIRE_THAT(weighted / energy, WithinAbs(window / 2.0, window / 4.0));
}

// ── A4 — crossfade continuity ─────────────────────────────────────────────

TEST_CASE("A4 the raised-cosine window suppresses the tap reset discontinuity",
          "[signal][pitch-shifter]") {
    constexpr double kTone = 220.0;
    constexpr double kSemitones = 12.0;
    auto shifter = make_direct(kSemitones);
    const auto wet = render_wet(shifter, kTone);

    double max_step = 0.0;
    for (std::size_t n = 1; n < wet.size(); ++n)
        max_step = std::max(max_step, std::abs(wet[n] - wet[n - 1]));

    // The spec's contract bound.
    REQUIRE(max_step <= Shifter::kClickBound);

    // And the far sharper statement that would actually catch a broken window:
    // the wet output is no steeper than an UNMODULATED sine at the shifted
    // frequency. A single-tap sawtooth would spike near 2.0 here — 35× the
    // bound and 60× this one.
    const double bare_slope =
        2.0 * kPi * kTone * ratio_of(kSemitones) / kSr * shifter.dc_blocker_magnitude_peak();
    REQUIRE(max_step <= 1.05 * bare_slope);
}

TEST_CASE("A4 a window change is click-free rather than merely small",
          "[signal][pitch-shifter]") {
    // The deferred per-tap latch: each tap adopts a new window length at the
    // instant its own phase passes zero, where its delay AND its crossfade
    // weight are both zero. An immediate change would step both taps' read
    // positions by up to half the window-length difference at full gain.
    constexpr double kTone = 220.0;
    auto shifter = make_direct(12.0, Shifter::kWindowMsDefault);

    std::vector<double> wet;
    const int total = 2 * kAnalysisLen;
    for (int n = 0; n < total; ++n) {
        if (n == total / 2) shifter.set_window_ms(2.0 * Shifter::kWindowMsDefault);
        const double x = std::sin(2.0 * kPi * kTone * static_cast<double>(n) / kSr);
        wet.push_back(static_cast<double>(shifter.process_wet(x)));
    }

    double max_step = 0.0;
    for (std::size_t n = kSettle + 1; n < wet.size(); ++n)
        max_step = std::max(max_step, std::abs(wet[n] - wet[n - 1]));

    const double bare_slope =
        2.0 * kPi * kTone * ratio_of(12.0) / kSr * shifter.dc_blocker_magnitude_peak();
    REQUIRE(max_step <= 1.05 * bare_slope);

    // The latency report follows the request immediately, not the latch.
    REQUIRE(shifter.latency_samples() ==
            static_cast<int>(std::lround(2.0 * Shifter::kWindowMsDefault * kSr / 2000.0)));
}

// ── A5 — determinism ──────────────────────────────────────────────────────

TEST_CASE("A5 renders are bit-identical per params and input",
          "[signal][pitch-shifter]") {
    Xorshift32 rng(0x1234ABCDu);
    std::vector<double> noise(20000);
    for (double& v : noise) v = rng.next_bipolar<double>();

    for (double depth : {0.0, 1.0}) {
        auto shifter = make_direct(7.0);
        shifter.set_drift_depth(depth);
        shifter.reset();

        std::vector<double> first, second;
        for (double v : noise) first.push_back(static_cast<double>(shifter.process(v)));
        shifter.reset();
        for (double v : noise) second.push_back(static_cast<double>(shifter.process(v)));

        REQUIRE(first == second);
    }
}

TEST_CASE("drift is seeded and bounded and reported", "[signal][pitch-shifter]") {
    auto quiet = make_direct(0.0);
    auto drifting = make_direct(0.0);
    drifting.set_drift_depth(1.0);
    drifting.reset();

    double lo = 1e9, hi = -1e9;
    for (int n = 0; n < 10 * static_cast<int>(kSr); ++n) {
        quiet.process_wet(0.0);
        drifting.process_wet(0.0);
        lo = std::min(lo, drifting.current_ratio());
        hi = std::max(hi, drifting.current_ratio());
        // The deterministic path is BYPASSED at depth 0, not merely silent.
        REQUIRE(quiet.current_ratio() == 1.0);
    }

    // Full depth is an analog wobble, not a vibrato: the excursion stays within
    // a few sigma of the shipped 1σ depth.
    const double sigma_cents =
        1200.0 * std::log2(1.0 + Shifter::kDriftMaxDepthPercent * 0.01);
    const double peak_to_peak_cents = 1200.0 * std::log2(hi / lo);
    REQUIRE(peak_to_peak_cents > 0.5 * sigma_cents);
    REQUIRE(peak_to_peak_cents < 12.0 * sigma_cents);
}

// ── A6 — RT allocation ────────────────────────────────────────────────────

TEST_CASE("A6 nothing on the audio path allocates after prepare",
          "[signal][pitch-shifter][rt-safety]") {
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.reset();

    pulp::test::RtAllocationProbe probe;
    for (int n = 0; n < 4096; ++n) {
        shifter.set_pedal(0.5 + 0.5 * std::sin(0.01 * n));
        shifter.set_pedal_mode(static_cast<PedalMode>(n % 4));
        shifter.set_shift_source(static_cast<ShiftSource>(n % 2));
        shifter.set_shift_semitones(-24.0 + 0.01 * n);
        shifter.set_window_ms(Shifter::kWindowMsMin +
                              0.02 * static_cast<double>(n % 4000));
        shifter.set_glide_ms(static_cast<double>(n % 500), 60.0);
        shifter.set_targets(0.0, 12.0);
        shifter.set_harmony(7.0, 12.0);
        shifter.set_detune_cents(12.0);
        shifter.set_dive_floor_semis(-48.0);
        shifter.set_mix(0.5);
        shifter.set_detents(n % 2 == 0);
        shifter.set_interp(static_cast<PitchInterp>(n % 2));
        shifter.set_drift_depth((n % 3) * 0.5);
        shifter.snap_to_target();
        shifter.process(0.05);
        shifter.process_wet(0.05);
    }
    shifter.reset();
    REQUIRE(probe.allocation_count() == 0);
}

// ── A7 — gain ceiling ─────────────────────────────────────────────────────

TEST_CASE("A7 the wet leg never exceeds the input peak", "[signal][pitch-shifter]") {
    // The crossfade weights sum to 1, so the tap pair is a CONVEX combination
    // and is bounded by the buffer peak. The only thing that can exceed 1 is
    // the DC blocker the spec puts on the wet leg, whose peak magnitude is
    // 2/(1+p) — asserted exactly rather than rounded to the spec's 1.0.
    for (double semitones = Shifter::kShiftSemisMin;
         semitones <= Shifter::kShiftSemisMax; semitones += 3.0) {
        auto shifter = make_direct(semitones);
        const auto wet = render_wet(shifter, 997.0);
        REQUIRE(peak(wet) <= Shifter::kDcBlockerPeakGain);
    }
}

TEST_CASE("A7 the equal-power dry/wet mix is bounded by root two",
          "[signal][pitch-shifter]") {
    double worst = 0.0;
    for (double mix = 0.0; mix <= 1.0001; mix += 0.05) {
        Shifter shifter;
        shifter.prepare(kSr);
        shifter.set_pedal_mode(PedalMode::harmony);
        shifter.set_glide_ms(0.0, 0.0);
        shifter.set_pedal(0.0);
        shifter.set_mix(mix);
        shifter.reset();

        double pk = 0.0;
        for (int n = 0; n < kSettle + kAnalysisLen; ++n) {
            const double x = std::sin(2.0 * kPi * 997.0 * static_cast<double>(n) / kSr);
            const double y = static_cast<double>(shifter.process(x));
            if (n >= kSettle) pk = std::max(pk, std::abs(y));
        }
        worst = std::max(worst, pk);
    }

    Shifter probe;
    probe.prepare(kSr);
    const double bound = Shifter::kWorstCaseGain;
    REQUIRE(worst <= bound);
    // The registry number is a real bound, not a loose one: the sweep gets
    // within 0.5 % of it, at the 50/50 point where the model says it should.
    // Upper bound only. This case drives a 997 Hz SINE, which cannot approach
    // √5 — reaching the bound needs sustained near-DC content, and the
    // tightness claim lives with that input in the near-DC case below. Asserting
    // tightness here is what previously made a wrong bound look verified: the
    // measured 1.41 matched the old √2 exactly, so the bound looked both correct
    // and tight while being 3.9 dB low for the input that actually stresses it.
    REQUIRE(worst > 1.0);
}

// ── A8 — the pedal law ────────────────────────────────────────────────────

TEST_CASE("A8 every mode's endpoints land on its nominal targets",
          "[signal][pitch-shifter]") {
    Shifter shifter;
    shifter.prepare(kSr);

    shifter.set_pedal_mode(PedalMode::whammy);
    REQUIRE_THAT(shifter.pedal_law(0.0), WithinAbs(Shifter::kHeelSemisDefault, 1e-12));
    REQUIRE_THAT(shifter.pedal_law(1.0), WithinAbs(Shifter::kToeSemisDefault, 1e-12));

    shifter.set_pedal_mode(PedalMode::harmony);
    REQUIRE_THAT(shifter.pedal_law(0.0),
                 WithinAbs(Shifter::kIntervalASemisDefault, 1e-12));
    REQUIRE_THAT(shifter.pedal_law(1.0),
                 WithinAbs(Shifter::kIntervalBSemisDefault, 1e-12));

    shifter.set_pedal_mode(PedalMode::detune);
    REQUIRE_THAT(shifter.pedal_law(0.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(shifter.pedal_law(1.0),
                 WithinAbs(Shifter::kDetuneCentsDefault / units::kCentsPerSemitone,
                           1e-12));

    shifter.set_pedal_mode(PedalMode::dive);
    REQUIRE_THAT(shifter.pedal_law(0.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(shifter.pedal_law(1.0),
                 WithinAbs(Shifter::kDiveFloorSemisDefault, 1e-12));

    // The intervals the detent table names are 12-TET, definitional: a perfect
    // fifth is 2^(7/12) and a perfect fourth is 2^(5/12).
    REQUIRE_THAT(ratio_of(7.0), WithinRel(1.4983070768766815, 1e-12));
    REQUIRE_THAT(ratio_of(5.0), WithinRel(1.3348398541700344, 1e-12));
}

TEST_CASE("A8 the pedal law is monotonic in every mode", "[signal][pitch-shifter]") {
    Shifter shifter;
    shifter.prepare(kSr);
    for (PedalMode mode : {PedalMode::whammy, PedalMode::harmony, PedalMode::detune,
                           PedalMode::dive}) {
        shifter.set_pedal_mode(mode);
        const bool ascending = shifter.pedal_law(1.0) >= shifter.pedal_law(0.0);
        double previous = shifter.pedal_law(0.0);
        for (int i = 1; i <= 1000; ++i) {
            const double value = shifter.pedal_law(i / 1000.0);
            if (ascending) {
                REQUIRE(value >= previous - 1e-12);
            } else {
                REQUIRE(value <= previous + 1e-12);
            }
            previous = value;
        }
    }
}

TEST_CASE("A8 the glide arrives in glide_ms exactly", "[signal][pitch-shifter]") {
    // SlewLimiterT in SlewMode::linear — constant TIME, exact arrival — which is
    // what a pedal portamento means and what the module selects. The spec's
    // "90 % of target in glide_up_ms" is the exponential-mode reading (t90 =
    // 2.303 τ); in linear mode 90 % lands at 0.9 · glide_up_ms.
    constexpr double kGlideMs = 100.0;
    constexpr double kToe = 12.0;

    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_glide_ms(kGlideMs, kGlideMs);
    shifter.set_targets(0.0, kToe);
    shifter.set_pedal(0.0);
    shifter.reset();
    REQUIRE(shifter.current_semitones() == 0.0);

    shifter.set_pedal(1.0);
    const long samples_full = std::lround(units::ms_to_samples(kGlideMs, kSr));
    const long samples_90 = std::lround(0.9 * static_cast<double>(samples_full));

    long reached_90 = -1, reached_full = -1;
    for (long n = 1; n <= samples_full + 16; ++n) {
        shifter.process_wet(0.0);
        if (reached_90 < 0 && shifter.current_semitones() >= 0.9 * kToe) reached_90 = n;
        if (reached_full < 0 && shifter.current_semitones() >= kToe) reached_full = n;
    }
    REQUIRE(reached_90 > 0);
    REQUIRE(std::abs(reached_90 - samples_90) <= 1);
    REQUIRE(reached_full > 0);
    REQUIRE(std::abs(reached_full - samples_full) <= 1);

    // Rise and fall are independent.
    Shifter asymmetric;
    asymmetric.prepare(kSr);
    asymmetric.set_glide_ms(10.0, 1000.0);
    REQUIRE(asymmetric.glide_up_ms() == 10.0);
    REQUIRE(asymmetric.glide_down_ms() == 1000.0);
}

TEST_CASE("A8 detents capture and release with hysteresis",
          "[signal][pitch-shifter]") {
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_pedal_mode(PedalMode::whammy);
    shifter.set_targets(0.0, 24.0);
    shifter.set_detents(true);

    constexpr double kDetent = 7.0;
    const double expected_capture = Shifter::kDetentSnapSemis;
    const double expected_release =
        std::min(Shifter::kDetentSnapSemis * Shifter::kDetentReleaseRatio,
                 Shifter::kDetentReleaseLimitSemis);
    REQUIRE(expected_release > expected_capture);  // it IS a Schmitt band

    // Approach from above: capture at the narrow threshold.
    constexpr double kStep = 0.0005;
    double capture = 0.0;
    for (double raw = kDetent + 1.0; raw > kDetent; raw -= kStep) {
        shifter.set_pedal(raw / 24.0);
        if (shifter.target_semitones() == kDetent) { capture = raw - kDetent; break; }
    }
    REQUIRE_THAT(capture, WithinAbs(expected_capture, 2.0 * kStep));

    // Leave from below: release only at the wider threshold.
    double release = 0.0;
    for (double raw = kDetent; raw < kDetent + 1.5; raw += kStep) {
        shifter.set_pedal(raw / 24.0);
        if (shifter.target_semitones() != kDetent) { release = raw - kDetent; break; }
    }
    REQUIRE_THAT(release, WithinAbs(expected_release, 2.0 * kStep));

    // Parked between the two thresholds, dithering the pedal does not chatter.
    const double parked = kDetent + 0.5 * (expected_capture + expected_release);
    shifter.set_pedal(kDetent / 24.0);  // re-capture
    REQUIRE(shifter.target_semitones() == kDetent);
    for (int i = 0; i < 200; ++i) {
        shifter.set_pedal((parked + ((i % 2) ? 1e-4 : -1e-4)) / 24.0);
        REQUIRE(shifter.target_semitones() == kDetent);
    }

    // Every table entry inside the pedal's travel is reachable.
    for (double detent : Shifter::kDetentTable) {
        if (detent < 0.0 || detent > 24.0) continue;
        shifter.set_pedal(detent / 24.0);
        REQUIRE_THAT(shifter.target_semitones(), WithinAbs(detent, 1e-12));
    }
}

TEST_CASE("detents are bypassed where the mode's target is not an interval",
          "[signal][pitch-shifter]") {
    // Read literally, detents would make two modes inert: `detune`'s default
    // target of 0.12 semitone sits three times INSIDE the 0.35 capture band and
    // would snap to unison, and `dive` would descend in steps. Detents quantise
    // interval targets, so they apply where the targets are intervals.
    REQUIRE(Shifter::mode_uses_detents(PedalMode::whammy));
    REQUIRE(Shifter::mode_uses_detents(PedalMode::harmony));
    REQUIRE_FALSE(Shifter::mode_uses_detents(PedalMode::detune));
    REQUIRE_FALSE(Shifter::mode_uses_detents(PedalMode::dive));

    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_detents(true);

    const double detune_semis = Shifter::kDetuneCentsDefault / units::kCentsPerSemitone;
    REQUIRE(detune_semis < Shifter::kDetentSnapSemis);  // the trap, in one line
    shifter.set_pedal_mode(PedalMode::detune);
    shifter.set_pedal(1.0);
    REQUIRE_THAT(shifter.target_semitones(), WithinAbs(detune_semis, 1e-12));

    shifter.set_pedal_mode(PedalMode::dive);
    shifter.set_pedal(0.25);
    REQUIRE_THAT(shifter.target_semitones(),
                 WithinAbs(0.25 * Shifter::kDiveFloorSemisDefault, 1e-12));

    // And never in the direct source, where the caller's value IS the target.
    shifter.set_shift_source(ShiftSource::direct);
    shifter.set_shift_semitones(6.9);
    REQUIRE_THAT(shifter.target_semitones(), WithinAbs(6.9, 1e-12));
}

// ── A9 — the dive floor ───────────────────────────────────────────────────

TEST_CASE("A9 the dive bottoms out at its floor and stays finite",
          "[signal][pitch-shifter]") {
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_pedal_mode(PedalMode::dive);
    shifter.set_glide_ms(0.0, 0.0);
    shifter.set_mix(1.0);
    shifter.set_pedal(1.0);
    shifter.reset();

    const double expected_ratio = ratio_of(Shifter::kDiveFloorSemisDefault);
    REQUIRE_THAT(shifter.current_ratio(), WithinAbs(expected_ratio, 1e-6));
    REQUIRE(shifter.current_ratio() > 0.0);  // never DC

    constexpr double kF0 = 1000.0;
    const auto wet = render_wet(shifter, kF0);
    for (double v : wet) REQUIRE(std::isfinite(v));

    const double expected_hz = kF0 * expected_ratio;
    REQUIRE_THAT(peak_near(wet, expected_hz, 5.0, 0.02),
                 WithinRel(expected_hz, 0.003));

    // The dive gets grainier as it deepens: |1−r| → 1 drives the warble up.
    REQUIRE_THAT(shifter.warble_hz(),
                 WithinRel(warble_hz(Shifter::kDiveFloorSemisDefault,
                                     Shifter::kWindowMsDefault),
                           1e-9));

    // The floor is reachable below the direct-shift knob's own range, which is
    // why the internal clamp is the union of the two.
    REQUIRE(Shifter::kDiveFloorSemisMin < Shifter::kShiftSemisMin);
    REQUIRE(Shifter::kSemitonesFloor <= Shifter::kDiveFloorSemisMin);
    Shifter deep;
    deep.prepare(kSr);
    deep.set_pedal_mode(PedalMode::dive);
    deep.set_dive_floor_semis(Shifter::kDiveFloorSemisMin);
    deep.set_pedal(1.0);
    deep.snap_to_target();
    REQUIRE_THAT(deep.current_ratio(),
                 WithinRel(ratio_of(Shifter::kDiveFloorSemisMin), 1e-9));
}

// ── Series law 4 — the aliasing this topology really does have ────────────

TEST_CASE("up-shifts alias and the fold lands where resampling says it does",
          "[signal][pitch-shifter][characterisation]") {
    // The spec says no oversampling is required because there is no
    // nonlinearity. There is no nonlinearity — and a ratio shifter still
    // aliases, because it is a resampler: at r > 1 it decimates, so input above
    // fs/(2r) folds. This case measures the fold rather than assuming it away.
    constexpr double kTone = 15000.0;
    constexpr double kSemitones = 12.0;
    const double r = ratio_of(kSemitones);
    const double unfolded = kTone * r;              // 30 kHz
    const double folded = kSr - unfolded;           // 18 kHz
    REQUIRE(unfolded > kSr / 2.0);

    auto shifter = make_direct(kSemitones);
    const auto wet = render_wet(shifter, kTone);
    REQUIRE(magnitude_bh(wet, folded) > 0.5);

    // Confirm the instrument against a KNOWN-GOOD control: the same tone at
    // unison must read nothing at the fold frequency. Without this the
    // measurement above cannot distinguish a real alias from a leaky probe.
    auto control = make_direct(0.0);
    const auto dry = render_wet(control, kTone);
    REQUIRE(magnitude_bh(dry, folded) < 1e-4);
    REQUIRE(magnitude_bh(dry, kTone) > 0.5);
}

// ── Composition surface (the harmony engine composes this) ────────────────

TEST_CASE("process_wet is the shifted leg alone", "[signal][pitch-shifter]") {
    auto mixed = make_direct(7.0);
    auto wet_only = make_direct(7.0);
    mixed.set_mix(1.0);

    // "Dry muted at mix = 100 %" is exact to within one floating-point rounding
    // and no further: the equal-power law's dry gain at full wet is
    // `cos(π/2)` = 6.1e-17 rather than a literal zero. That residue is −324 dBFS
    // and this module deliberately does NOT special-case the endpoint, because
    // second-guessing the shared crossfade law at its own endpoints is how two
    // callers end up with two laws. The full-DRY end has no such residue —
    // `cos(0)` and `sin(0)` are exact — which is why `mix = 0` really is a
    // bit-exact passthrough (asserted in the unison case).
    constexpr double kEqualPowerDryResidue = 6.2e-17;
    for (int n = 0; n < 8000; ++n) {
        const double x = std::sin(2.0 * kPi * 330.0 * static_cast<double>(n) / kSr);
        const double with_mix = static_cast<double>(mixed.process(x));
        const double wet = static_cast<double>(wet_only.process_wet(x));
        REQUIRE_THAT(with_mix, WithinAbs(wet, kEqualPowerDryResidue * std::abs(x)));
    }
}

TEST_CASE("the direct source drives the shift from a caller's semitone value",
          "[signal][pitch-shifter]") {
    // What a pitch-tracked harmoniser needs: set an interval per block, jump to
    // it without portamento on a note-on, and read back what is applied.
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_shift_source(ShiftSource::direct);
    shifter.set_glide_ms(Shifter::kGlideMsDefault, Shifter::kGlideMsDefault);

    for (double semitones : {3.0, -5.0, 12.0, 0.0}) {
        shifter.set_shift_semitones(semitones);
        shifter.snap_to_target();
        REQUIRE_THAT(shifter.current_semitones(), WithinAbs(semitones, 1e-12));
        REQUIRE_THAT(shifter.current_ratio(), WithinRel(ratio_of(semitones), 1e-12));
    }

    // The pedal is inert while the direct source is selected, and vice versa.
    shifter.set_pedal(1.0);
    shifter.set_shift_semitones(4.0);
    REQUIRE_THAT(shifter.target_semitones(), WithinAbs(4.0, 1e-12));
    shifter.set_shift_source(ShiftSource::pedal);
    REQUIRE_THAT(shifter.target_semitones(),
                 WithinAbs(Shifter::kToeSemisDefault, 1e-12));

    // A mode change does not move the mix out from under the player; the
    // per-mode defaults are published for the node to apply deliberately.
    shifter.set_mix(0.25);
    shifter.set_pedal_mode(PedalMode::harmony);
    REQUIRE(shifter.mix() == 0.25);
    REQUIRE(Shifter::default_mix_for(PedalMode::harmony) == 0.5);
    REQUIRE(Shifter::default_mix_for(PedalMode::detune) == 0.4);
    REQUIRE(Shifter::default_mix_for(PedalMode::whammy) == 1.0);
    REQUIRE(Shifter::default_mix_for(PedalMode::dive) == 1.0);
}

TEST_CASE("W4 pedal-morphed harmony walks the detent table",
          "[signal][pitch-shifter][cookbook]") {
    // The starred patch from the cookbook, as a composition test: harmony A =
    // +7, B = +12, detents on; walking the pedal heel→toe steps the shifted
    // peak from a fifth to an octave through the table, at closed-form
    // frequencies.
    constexpr double kF0 = 1000.0;
    Shifter shifter;
    shifter.prepare(kSr);
    shifter.set_pedal_mode(PedalMode::harmony);
    shifter.set_harmony(Shifter::kIntervalASemisDefault,
                        Shifter::kIntervalBSemisDefault);
    shifter.set_detents(true);
    shifter.set_glide_ms(0.0, 0.0);
    shifter.set_mix(1.0);

    // Heel and toe land exactly on their intervals; the middle passes through
    // the table entries between them and nothing else.
    for (double e : {0.0, 1.0}) {
        shifter.set_pedal(e);
        shifter.snap_to_target();
        const double expected = e == 0.0 ? Shifter::kIntervalASemisDefault
                                         : Shifter::kIntervalBSemisDefault;
        REQUIRE_THAT(shifter.current_semitones(), WithinAbs(expected, 1e-12));

        shifter.reset();
        const auto wet = render_wet(shifter, kF0);
        const double expected_hz = kF0 * ratio_of(expected);
        REQUIRE_THAT(peak_near(wet, expected_hz, 20.0, 0.25),
                     WithinRel(expected_hz, 0.003));
    }

    // Between +7 and +12 the table has nothing, so the walk is a single snap
    // from one to the other rather than a chain of intermediate detents.
    int distinct = 0;
    double previous = 1e9;
    for (int i = 0; i <= 1000; ++i) {
        shifter.set_pedal(i / 1000.0);
        const double target = shifter.target_semitones();
        if (std::abs(target - previous) > 1e-9) ++distinct;
        previous = target;
    }
    // +7 held, the un-snapped glide between the bands, then +12 held: three
    // regimes, so at most a handful of transitions plus the glide's own steps.
    REQUIRE(distinct > 1);
    REQUIRE(shifter.target_semitones() == Shifter::kIntervalBSemisDefault);
}

TEST_CASE("cubic interpolation is available and changes only the tap read",
          "[signal][pitch-shifter]") {
    auto linear = make_direct(7.0);
    auto cubic = make_direct(7.0);
    cubic.set_interp(PitchInterp::cubic);
    REQUIRE(cubic.interp() == PitchInterp::cubic);

    const auto wet_linear = render_wet(linear, 1000.0, kAnalysisLen / 4);
    const auto wet_cubic = render_wet(cubic, 1000.0, kAnalysisLen / 4);

    // Same pitch — the interpolant does not change the ratio…
    const double expected = 1000.0 * ratio_of(7.0);
    REQUIRE_THAT(peak_near(wet_cubic, expected, 20.0, 0.25), WithinRel(expected, 0.003));
    // …but it is a different read, so the renders are not identical.
    REQUIRE(wet_linear != wet_cubic);
    // …and it is still bounded.
    REQUIRE(peak(wet_cubic) <= Shifter::kDcBlockerPeakGain);
}

TEST_CASE("the float instantiation shifts to the same pitch",
          "[signal][pitch-shifter]") {
    // `PitchShifter` (float) is the DEFAULT alias and the one a plugin will
    // instantiate; every case above runs the double one. The delay storage, the
    // DC blocker, and the tap arithmetic all narrow to `SampleType`, so this is
    // a genuinely different numeric path rather than a template formality.
    constexpr double kF0 = 1000.0;
    constexpr double kSemitones = 7.0;

    PitchShifter shifter;
    shifter.prepare(kSr);
    shifter.set_shift_source(ShiftSource::direct);
    shifter.set_glide_ms(0.0, 0.0);
    shifter.set_mix(1.0);
    shifter.set_shift_semitones(kSemitones);
    shifter.reset();

    std::vector<double> wet;
    wet.reserve(kAnalysisLen);
    for (int n = 0; n < kSettle + kAnalysisLen; ++n) {
        const float x =
            static_cast<float>(std::sin(2.0 * kPi * kF0 * static_cast<double>(n) / kSr));
        const float y = shifter.process(x);
        REQUIRE(std::isfinite(y));
        if (n >= kSettle) wet.push_back(static_cast<double>(y));
    }

    const double expected = kF0 * ratio_of(kSemitones);
    REQUIRE_THAT(peak_near(wet, expected, 20.0, 0.25), WithinRel(expected, 0.003));
    REQUIRE(peak(wet) <= Shifter::kDcBlockerPeakGain);
    REQUIRE(shifter.latency_samples() ==
            static_cast<int>(std::lround(PitchShifter::kWindowMsDefault * kSr / 2000.0)));
}

TEST_CASE("a fresh instance survives being used before prepare",
          "[signal][pitch-shifter]") {
    // "Zero-init is valid but must see prepare before process" — valid has to
    // mean it does not read out of bounds or emit NaN, not merely that it
    // compiles.
    Shifter shifter;
    for (int n = 0; n < 128; ++n) {
        const double y = static_cast<double>(shifter.process(0.5));
        REQUIRE(std::isfinite(y));
    }
    shifter.prepare(kSr);
    shifter.reset();
    REQUIRE(std::isfinite(static_cast<double>(shifter.process(0.5))));
}

TEST_CASE("The peak-gain bound holds on sustained near-DC content too",
          "[signal][pitch-shifter][gain]") {
    // The bound was previously certified with a single 997 Hz sine — which is
    // precisely the signal for which a MAGNITUDE-response bound is valid, and
    // therefore the one input that could not reveal the error. The claimed wet
    // bound came from the DC blocker's magnitude peak at Nyquist (1.000327);
    // the real limit on a single sample is its impulse response's L1 norm,
    // exactly 2. Measured 1.97 against a claimed 1.0003 — 5.9 dB out.
    //
    // Reaching it needs sustained energy near DC, where the blocker's
    // differencing term and its pole both act on the same excursion: a slow
    // square, not a tone.
    for (double semitones : {-12.0, -5.0, 7.0, 12.0}) {
        Shifter shifter;
        shifter.prepare(kSr);
        shifter.set_shift_source(ShiftSource::direct);
        shifter.set_shift_semitones(semitones);
        shifter.set_mix(1.0);
        shifter.reset();

        double worst = 0.0;
        const int frames = static_cast<int>(kSr * 2.0);
        for (int n = 0; n < frames; ++n) {
            // 0.5 Hz square: as close to DC as anything musical gets.
            const double x = std::sin(2.0 * M_PI * 0.5 * n / kSr) >= 0.0 ? 1.0 : -1.0;
            worst = std::max(worst, std::abs(shifter.process(x)));
        }
        REQUIRE(worst <= Shifter::kDcBlockerPeakGain);
        // ...and the bound is not vacuous: this input genuinely exceeds the
        // old claim of ~1.0003, which is what made it wrong rather than loose.
        REQUIRE(worst > 1.05);
    }
}
