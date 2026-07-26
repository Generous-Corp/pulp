// VcaCompressorT — the dbx/Blackmer lineage: true-RMS, log-domain, one time
// constant.
//
// The spec's acceptance suite, tests 1–10. Expected values are computed from
// the shipped constants and the closed-form equations, never restated as bare
// literals — so moving a constant fails the test that documents it rather than
// silently disagreeing with it.
//
// Like the feedforward suite, several cases measure the DETECTOR
// (`mean_square()`, `level_db()`, `gain_reduction_db()`) rather than inferring
// it from the audio: dividing output by an input that crosses zero every half
// cycle turns a clean measurement into a noisy one and hides exactly the
// step-response detail these tests exist to check.
//
// Three cases are DELIBERATELY not what the spec text asks for. Each carries
// the arithmetic that shows the spec's own criterion is unachievable by any
// correct implementation of the specified topology, and asserts the value the
// topology actually produces instead:
//
//   * Test 3 (true-RMS accuracy)  — spec asks ±0.1 dB of true RMS; the
//     specified `k > 1` detector settles ~1.5 dB high on a sine, by a closed
//     form derived here and asserted against.
//   * Test 4 (time-constant)      — spec measures "the release direction" with
//     a step UP, which is the attack branch. Both directions are measured.
//   * Test 6 (negative-ratio floor) — spec's stated operating point produces
//     −67.5 dB, not the −96 dB clamp it predicts. Both are asserted.
//
// A test that would pass equally against `FeedforwardCompressorT` is not
// testing this module. Tests 3, 4, 5 and the memoryless-gain-computer case all
// fail against that class by construction.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vca_compressor.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Vca = VcaCompressorT<double>;
constexpr double kSr = 48000.0;
constexpr double kToneHz = 1000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// The spec's soft-knee static characteristic, written out independently of the
/// header so the test checks the header's arithmetic rather than calling it and
/// agreeing with itself.
double reference_static_curve(double x, double t, double r, double w) {
    const double over = x - t;
    if (2.0 * over < -w) return x;
    if (w > 0.0 && 2.0 * std::abs(over) <= w) {
        const double num = over + w * 0.5;
        return x + (1.0 / r - 1.0) * num * num / (2.0 * w);
    }
    return t + over / r;
}

/// A compressor at a stated operating point, detector left at the default.
Vca curve_probe(double t, double r, double w) {
    Vca c;
    c.prepare(kSr);
    c.set_threshold_db(t);
    c.set_ratio(r);
    c.set_knee_db(w);
    c.set_makeup_db(0.0);
    c.set_mix(1.0);
    return c;
}

/// The detector's steady-state offset above the true mean square, as a fraction
/// of it, for a sine.
///
/// Ground truth, derived here and not from the implementation. At equilibrium
/// the mean-square state ȳ drifts neither up nor down over a cycle, so the
/// charge and discharge integrals balance:
///
///     attack_a · ⟨(p − ȳ)⁺⟩ = release_a · ⟨(ȳ − p)⁺⟩
///
/// With p = m(1 − cos φ) and c = (ȳ − m)/m this reduces to
///
///     f(c) = c·r/(1 − r),   f(c) = (√(1 − c²) − c·arccos(c))/π,   r = release_a/attack_a
///
/// f is strictly decreasing on [0, 1] from 1/π to 0, and the right-hand side is
/// strictly increasing from 0, so the root is unique and bisection is safe.
double sine_equilibrium_offset(double coef_ratio) {
    const auto h = [coef_ratio](double c) {
        return (std::sqrt(1.0 - c * c) - c * std::acos(c)) / M_PI -
               c * coef_ratio / (1.0 - coef_ratio);
    };
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (h(mid) > 0.0 ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

/// Mean of the detector's mean-square state over a whole number of tone cycles,
/// after `settle_s` seconds of the same tone.
double settled_mean_square_sine(Vca& c, double peak, double settle_s) {
    const double w = units::hz_to_radians_per_sample(kToneHz, kSr);
    const auto settle = static_cast<int>(kSr * settle_s);
    for (int i = 0; i < settle; ++i) c.process(peak * std::sin(w * i));

    // 100 ms at 1 kHz / 48 kHz is exactly 100 tone periods, so the average is
    // over whole cycles and carries no partial-cycle bias.
    const auto window = static_cast<int>(kSr * 0.1);
    double sum = 0.0;
    for (int i = 0; i < window; ++i) {
        c.process(peak * std::sin(w * (settle + i)));
        sum += c.mean_square();
    }
    return sum / window;
}

/// Samples for the detector's mean square to cross `fraction` of the way from
/// `from` to `to`, driving a constant-magnitude input.
///
/// The input is DC (or, equivalently for the detector, a square wave) rather
/// than a sine ON PURPOSE. A sine's instantaneous power swings between 0 and
/// its peak every half cycle, so once the state approaches the mean the
/// direction branch flips twice per cycle and the measured "attack" becomes a
/// blend of both coefficients — which would bias the very ratio test 5 exists
/// to measure. A constant-power step exercises one branch cleanly for the whole
/// travel, which is what "the attack time" means.
int samples_to_fraction(Vca& c, double input, double from, double to, double fraction,
                        int max_samples) {
    const double target = from + fraction * (to - from);
    const bool rising = to > from;
    for (int i = 0; i < max_samples; ++i) {
        c.process(input);
        const double y = c.mean_square();
        if (rising ? (y >= target) : (y <= target)) return i;
    }
    return -1;
}

/// Samples for the detector's mean square to travel from 10 % to 90 % of the
/// way from `from` to `to`, in ONE pass.
///
/// One pass rather than two calls to `samples_to_fraction`, because the second
/// call would resume from the state the first left behind and return a count
/// measured from THERE — an error proportional to the time constant, which
/// therefore cancels in the ratio and survives test 5's headline assertion
/// while corrupting both absolute times. The absolute checks are what catch it,
/// which is why they are there.
int span_10_90(Vca& c, double input, double from, double to, int max_samples) {
    const bool rising = to > from;
    const double low = from + 0.1 * (to - from);
    const double high = from + 0.9 * (to - from);
    int at_low = -1;
    for (int i = 0; i < max_samples; ++i) {
        c.process(input);
        const double y = c.mean_square();
        if (at_low < 0 && (rising ? y >= low : y <= low)) at_low = i;
        if (at_low >= 0 && (rising ? y >= high : y <= high)) return i - at_low;
    }
    return -1;
}

/// A detector settled on a constant-magnitude input, ready for a step.
Vca settled_detector(double time_ms, double k, double input, double seconds) {
    Vca c;
    c.prepare(kSr);
    c.set_time_ms(time_ms);
    c.set_attack_release_ratio_k(k);
    c.reset();
    const auto n = static_cast<int>(kSr * seconds);
    for (int i = 0; i < n; ++i) c.process(input);
    return c;
}

}  // namespace

// ── 1. Static gain computer, hard knee, closed form ───────────────────────

TEST_CASE("1 the hard-knee gain computer matches the closed form",
          "[vca-compressor][curve]") {
    constexpr double t = -20.0, r = 4.0;
    auto c = curve_probe(t, r, 0.0);

    // The spec's worked example: a −8 dB level, 4:1 over a −20 dB threshold.
    // (level − T)(1 − 1/R) is the textbook identity; the expected value is that
    // expression, not the −9 dB it evaluates to.
    constexpr double level = -8.0;
    const double expected = -(level - t) * (1.0 - 1.0 / r);
    REQUIRE_THAT(c.gain_computer_db(level), WithinAbs(expected, 0.001));
    REQUIRE_THAT(c.static_curve_db(level), WithinAbs(level + expected, 0.001));

    // ...and the whole curve, on both branches.
    for (int i = 0; i <= 120; ++i) {
        const double x = -60.0 + i;
        REQUIRE_THAT(c.static_curve_db(x), WithinAbs(reference_static_curve(x, t, r, 0.0), 1e-9));
    }

    // Below threshold the computer is exactly transparent — no "nearly".
    for (double x : {-60.0, -40.0, -21.0, -20.0}) REQUIRE(c.gain_computer_db(x) == 0.0);
}

TEST_CASE("1 the dB→linear VCA is the exact exponential law",
          "[vca-compressor][curve]") {
    // §3.4: the Blackmer cell's control law collapses to 10^(g/20) with no free
    // parameters, and gain is exactly unity at the neutral operating point.
    Vca c = curve_probe(-20.0, 4.0, 0.0);
    c.reset();
    REQUIRE(c.gain_reduction_db() == 0.0);
    REQUIRE(c.current_gain_linear() == units::db_to_linear(0.0));

    const double level = -8.0;
    const double reduction = c.gain_computer_db(level);
    REQUIRE_THAT(units::db_to_linear(reduction), WithinRel(std::pow(10.0, reduction / 20.0), 1e-15));
}

// ── 2. Soft-knee ("OverEasy") continuity ──────────────────────────────────

TEST_CASE("2 the knee is continuous at both edges and matches the hard branch there",
          "[vca-compressor][curve]") {
    constexpr double t = -20.0, r = 4.0, w = 10.0;
    auto soft = curve_probe(t, r, w);
    auto hard = curve_probe(t, r, 0.0);

    // Lower edge: the curve is still transparent.
    REQUIRE_THAT(soft.gain_computer_db(t - w * 0.5), WithinAbs(0.0, 0.001));
    // Upper edge: it has met the hard-knee line. −3.75 dB at these settings; the
    // expectation is the hard-knee computer's own value, not that literal.
    const double edge = t + w * 0.5;
    REQUIRE_THAT(soft.gain_computer_db(edge), WithinAbs(hard.gain_computer_db(edge), 0.01));

    // C⁰ across both edges, at every width the catalog allows.
    for (double width : {0.0, 6.0, 10.0, 24.0}) {
        auto c = curve_probe(t, r, width);
        for (double e : {t - width * 0.5, t + width * 0.5}) {
            REQUIRE_THAT(c.static_curve_db(e + 0.001) - c.static_curve_db(e - 0.001),
                         WithinAbs(0.0, 0.01));
        }
    }
}

TEST_CASE("2 the knee eases in below threshold, which is what OverEasy means",
          "[vca-compressor][curve]") {
    // The spec's second worked example, at x = −18 inside a 10 dB knee.
    constexpr double t = -20.0, r = 4.0, w = 10.0, x = -18.0;
    auto soft = curve_probe(t, r, w);
    auto hard = curve_probe(t, r, 0.0);

    const double over = x - t;
    const double expected = (1.0 / r - 1.0) * (over + w * 0.5) * (over + w * 0.5) / (2.0 * w);
    REQUIRE_THAT(soft.gain_computer_db(x), WithinAbs(expected, 1e-9));

    // More reduction than the hard knee would apply at the same point: the
    // curve is already easing in, rather than waiting for a corner.
    REQUIRE(soft.gain_computer_db(x) < hard.gain_computer_db(x));

    // And well past the knee the two agree again.
    REQUIRE_THAT(soft.gain_computer_db(0.0), WithinAbs(hard.gain_computer_db(0.0), 1e-9));
}

// ── 3. True RMS — and the bias the direction split imposes on it ──────────

TEST_CASE("3 a constant-power signal reads its exact true RMS",
          "[vca-compressor][detector]") {
    // The detector integrates x², so on any signal whose instantaneous power is
    // constant it converges on that power exactly, for every k. This is the
    // clean statement of "true RMS": no fudge factor, no crest-factor guess.
    for (double amplitude : {1.0, 0.5, 0.05}) {
        for (double k : {Vca::kRatioKMin, Vca::kRatioKDefault, Vca::kRatioKMax}) {
            Vca c = settled_detector(30.0, k, amplitude, 1.0);
            REQUIRE_THAT(c.level_db(), WithinAbs(units::linear_to_db(amplitude), 0.001));
        }
    }
}

TEST_CASE("3 the sine reading sits above true RMS by the closed-form offset",
          "[vca-compressor][detector]") {
    // SPEC DEFECT, adjudicated here. Test 3 asks for a 1 kHz sine at −6 dBFS
    // peak to read its true RMS (−9.031 dBFS) within ±0.1 dB. No correct
    // implementation of §3.1 can do that: the direction-switched pole charges k
    // times faster than it discharges, so on a signal whose instantaneous power
    // varies it settles ABOVE the mean square. The offset is not free — it is
    // the root of the balance equation in `sine_equilibrium_offset` — so what is
    // asserted is that closed form, plus the size of the disagreement with the
    // spec's own criterion.
    constexpr double peak_db = -6.0;
    constexpr double time_ms = 30.0;
    const double peak = units::db_to_linear(peak_db);
    const double true_rms_db = peak_db + 20.0 * std::log10(1.0 / std::sqrt(2.0));

    Vca c;
    c.prepare(kSr);
    c.set_time_ms(time_ms);
    c.set_attack_release_ratio_k(Vca::kRatioKDefault);
    c.reset();

    // The coefficients the closed form needs, computed from `units::` rather
    // than read off the object — and the object asserted to agree, so this is a
    // wiring check as well as a value check.
    const double attack_a = units::ms_to_onepole_coef(time_ms / Vca::kRatioKDefault, kSr);
    const double release_a = units::ms_to_onepole_coef(time_ms, kSr);
    REQUIRE_THAT(c.attack_coef(), WithinRel(attack_a, 1e-12));
    REQUIRE_THAT(c.release_coef(), WithinRel(release_a, 1e-12));

    const double offset = sine_equilibrium_offset(release_a / attack_a);
    const double predicted_db = 10.0 * std::log10(peak * peak * 0.5 * (1.0 + offset));

    const double measured = settled_mean_square_sine(c, peak, 2.0);
    const double measured_db = 10.0 * std::log10(measured);

    // The ripple the closed form neglects is second order at τ ≫ the tone
    // period; 0.05 dB is the whole budget for it.
    REQUIRE_THAT(measured_db, WithinAbs(predicted_db, 0.05));

    // The spec's criterion, quantified: the disagreement is ~1.5 dB, an order of
    // magnitude past its ±0.1 dB. Asserting a LOWER bound on the error is what
    // makes this a defect report rather than a loosened tolerance.
    REQUIRE(measured_db - true_rms_db > 1.0);
    REQUIRE_THAT(measured_db - true_rms_db,
                 WithinAbs(10.0 * std::log10((1.0 + offset) / 1.0), 0.05));
}

TEST_CASE("3 the detector is RMS, not peak: equal-peak sine and square differ",
          "[vca-compressor][detector]") {
    // A peak detector reads a sine and a square of the same peak identically.
    // An RMS one cannot: their mean squares differ by 3.01 dB. This detector
    // splits the difference by exactly its own equilibrium offset, so the gap is
    // predicted, non-zero, and provably not a peak reading.
    constexpr double amplitude = 0.5;
    constexpr double time_ms = 30.0;

    Vca square_reader = settled_detector(time_ms, Vca::kRatioKDefault, amplitude, 1.0);

    Vca sine_reader;
    sine_reader.prepare(kSr);
    sine_reader.set_time_ms(time_ms);
    sine_reader.reset();
    const double sine_ms = settled_mean_square_sine(sine_reader, amplitude, 2.0);

    const double attack_a = units::ms_to_onepole_coef(time_ms / Vca::kRatioKDefault, kSr);
    const double release_a = units::ms_to_onepole_coef(time_ms, kSr);
    const double offset = sine_equilibrium_offset(release_a / attack_a);

    const double gap_db = square_reader.level_db() - 10.0 * std::log10(sine_ms);
    REQUIRE_THAT(gap_db, WithinAbs(-10.0 * std::log10((1.0 + offset) * 0.5), 0.05));
    REQUIRE(gap_db > 1.0);
}

// ── 4. Time-constant exactness ────────────────────────────────────────────

TEST_CASE("4 both directions cross 63.2 % at exactly their own time constant",
          "[vca-compressor][detector]") {
    // SPEC RECIPE FIX. Test 4 says to step the mean square "from 0 to a fixed
    // value" and measure "the release direction" — but a step UP is the ATTACK
    // branch by construction (§3.1 selects on `p > y`). Measuring release needs
    // a step DOWN from a settled state. Both directions are measured here, each
    // against its own τ.
    //
    // For y += a(x − y) with a = 1 − exp(−1/N), y[n] = 1 − (1 − a)^n, so the
    // 1 − 1/e crossing is at exactly n = N = τ·fs. The ±1 sample tolerance is
    // the discrete-crossing quantisation, nothing else.
    constexpr double time_ms = 30.0;
    constexpr double amplitude = 0.5;
    const double power = amplitude * amplitude;

    for (double k : {Vca::kRatioKMin, Vca::kRatioKDefault, Vca::kRatioKMax}) {
        const double attack_n = units::ms_to_samples(time_ms / k, kSr);
        const double release_n = units::ms_to_samples(time_ms, kSr);

        Vca attack;
        attack.prepare(kSr);
        attack.set_time_ms(time_ms);
        attack.set_attack_release_ratio_k(k);
        attack.reset();
        const int measured_attack = samples_to_fraction(attack, amplitude, 0.0, power,
                                                        1.0 - 1.0 / M_E, static_cast<int>(kSr));
        REQUIRE(measured_attack > 0);
        REQUIRE_THAT(static_cast<double>(measured_attack), WithinAbs(attack_n, 1.0));

        Vca release = settled_detector(time_ms, k, amplitude, 1.0);
        const double start = release.mean_square();
        const int measured_release = samples_to_fraction(release, 0.0, start, 0.0,
                                                         1.0 - 1.0 / M_E, static_cast<int>(kSr));
        REQUIRE(measured_release > 0);
        REQUIRE_THAT(static_cast<double>(measured_release), WithinAbs(release_n, 1.0));
    }
}

// ── 5. The defining test: one knob, one fixed ratio ───────────────────────

TEST_CASE("5 release time over attack time equals k, at every k",
          "[vca-compressor][detector][lineage]") {
    // §0.1(2)'s architectural claim in falsifiable form. Not "attack equals
    // release" — a diode network is not symmetric either — but "ONE control and
    // one FIXED internal ratio govern both directions". A compressor with
    // independent attack/release knobs cannot satisfy this at three values of k
    // without three coincidences.
    constexpr double time_ms = 30.0;
    constexpr double amplitude = 0.5;
    const double power = amplitude * amplitude;

    for (double k : {Vca::kRatioKMin, Vca::kRatioKDefault, Vca::kRatioKMax}) {
        Vca attack;
        attack.prepare(kSr);
        attack.set_time_ms(time_ms);
        attack.set_attack_release_ratio_k(k);
        attack.reset();
        const int attack_1090 = span_10_90(attack, amplitude, 0.0, power, static_cast<int>(kSr));
        REQUIRE(attack_1090 > 0);

        Vca release = settled_detector(time_ms, k, amplitude, 1.0);
        const double start = release.mean_square();
        const int release_1090 = span_10_90(release, 0.0, start, 0.0, static_cast<int>(kSr));
        REQUIRE(release_1090 > 0);

        REQUIRE_THAT(static_cast<double>(release_1090) / attack_1090, WithinRel(k, 0.05));

        // Each direction's absolute 10–90 % time is τ·ln 9, so the ratio above
        // is not two errors cancelling. ±2 samples is the discrete-crossing
        // quantisation at both ends, nothing else.
        REQUIRE_THAT(static_cast<double>(attack_1090),
                     WithinAbs(units::ms_to_samples(time_ms / k, kSr) * std::log(9.0), 2.0));
        REQUIRE_THAT(static_cast<double>(release_1090),
                     WithinAbs(units::ms_to_samples(time_ms, kSr) * std::log(9.0), 2.0));
    }
}

TEST_CASE("5 the gain computer is memoryless — all ballistics live in the detector",
          "[vca-compressor][detector][lineage]") {
    // The architectural difference from the house's modern compressor, asserted
    // rather than described. There is no smoother after the gain computer, so
    // with the detector held still (constant-power input, settled) a threshold
    // change lands IN FULL on the very next sample. A design that smooths the
    // gain signal downstream takes an attack time to get there and fails this.
    constexpr double amplitude = 0.5;
    Vca c = settled_detector(30.0, Vca::kRatioKDefault, amplitude, 1.0);
    c.set_ratio(4.0);
    c.set_knee_db(0.0);
    c.set_threshold_db(-20.0);
    c.process(amplitude);

    const double level = c.level_db();
    REQUIRE_THAT(c.gain_reduction_db(), WithinAbs(c.gain_computer_db(level), 1e-12));
    const double before = c.gain_reduction_db();

    c.set_threshold_db(-40.0);
    c.process(amplitude);
    // The detector has not moved (constant power, already settled), so the whole
    // change is the static curve's.
    REQUIRE_THAT(c.level_db(), WithinAbs(level, 1e-12));
    REQUIRE_THAT(c.gain_reduction_db(), WithinAbs(c.gain_computer_db(level), 1e-12));
    REQUIRE(c.gain_reduction_db() < before - 10.0);
}

// ── 6. Negative-ratio "infinity+" and its floor ───────────────────────────

TEST_CASE("6 the negative-ratio floor clamps exactly, where it actually engages",
          "[vca-compressor][negative-ratio]") {
    // SPEC DEFECT, adjudicated here. Test 6 drives `T + 60` dB with R = −8 and
    // ceiling 96 and predicts a clamp at −96 dB. The curve's own arithmetic says
    // otherwise: reduction = (x − T)(1/R − 1) = 60 · (−1.125) = −67.5 dB, which
    // is 28.5 dB short of the floor. The floor first engages at
    // (x − T) = ceiling / |1/R − 1| = 85.33 dB. Both points are asserted — the
    // one the spec names (to show the clamp is NOT premature) and the one where
    // the clamp is real.
    constexpr double t = -20.0, neg_r = -8.0;
    const double ceiling = Vca::kCeilingDbDefault;

    Vca c;
    c.prepare(kSr);
    c.set_threshold_db(t);
    c.set_knee_db(0.0);
    c.set_negative_ratio_mode(true);
    c.set_neg_ratio_amount(neg_r);
    c.set_ceiling_db(ceiling);
    REQUIRE(c.active_ratio() == neg_r);

    const double slope = 1.0 / neg_r - 1.0;  // −1.125 at R = −8
    const double engage_over = ceiling / -slope;

    // The spec's own operating point: unclamped, and nowhere near the floor.
    const double spec_point = t + 60.0;
    REQUIRE_THAT(c.gain_computer_unclamped_db(spec_point), WithinAbs(60.0 * slope, 0.001));
    REQUIRE_THAT(c.gain_computer_db(spec_point), WithinAbs(60.0 * slope, 0.001));
    REQUIRE(c.gain_computer_db(spec_point) > -ceiling);
    REQUIRE(60.0 < engage_over);  // ...which is why it is not clamped.

    // Past the engagement point the clamp is exact, finite, and flat.
    for (double extra : {0.001, 10.0, 60.0}) {
        const double x = t + engage_over + extra;
        REQUIRE(std::isfinite(c.gain_computer_db(x)));
        REQUIRE(c.gain_computer_db(x) == -ceiling);
        REQUIRE(c.gain_computer_unclamped_db(x) < -ceiling);
    }

    // The clamp tracks the ceiling control rather than a baked constant.
    for (double ceil_db : {Vca::kCeilingDbMin, Vca::kCeilingDbMax}) {
        c.set_ceiling_db(ceil_db);
        REQUIRE(c.gain_computer_db(t + ceil_db / -slope + 20.0) == -ceil_db);
    }
}

TEST_CASE("6 above the knee, louder input really does get quieter output",
          "[vca-compressor][negative-ratio]") {
    // What "infinity+" claims to do, checked on the curve rather than assumed
    // from the sign of R.
    Vca c;
    c.prepare(kSr);
    c.set_threshold_db(-20.0);
    c.set_knee_db(0.0);
    c.set_negative_ratio_mode(true);
    c.set_neg_ratio_amount(-4.0);

    double previous = c.static_curve_db(-20.0);
    for (double x = -19.0; x <= 0.0; x += 1.0) {
        const double y = c.static_curve_db(x);
        REQUIRE(y < previous);
        previous = y;
    }

    // Turning the mode off restores the positive ratio the user was on, rather
    // than leaving the negative amount in force.
    c.set_ratio(4.0);
    c.set_negative_ratio_mode(false);
    REQUIRE(c.active_ratio() == 4.0);
    REQUIRE(c.static_curve_db(0.0) > c.static_curve_db(-10.0));
}

// ── 7. Worst-case gain: the Forge registry invariant ──────────────────────

TEST_CASE("7 the gain command is bounded above by makeup alone",
          "[vca-compressor][gain]") {
    // Series law 8. Feedforward: the detector reads the input, so no loop can
    // lift the gain past the makeup ceiling. Asserted at the exact operating
    // point the registry row cites, then not exceeded across a parameter sweep.
    const double bound = units::db_to_linear(Vca::kMakeupDbMax);

    Vca c;
    c.prepare(kSr);
    c.set_makeup_db(Vca::kMakeupDbMax);
    c.reset();
    for (int i = 0; i < static_cast<int>(kSr); ++i) REQUIRE(c.process(0.0) == 0.0);
    // Digital silence: no reduction, so the gain command IS the makeup, exactly.
    REQUIRE(c.gain_reduction_db() == 0.0);
    REQUIRE(c.current_gain_linear() == bound);

    // ...and it is reached in the audio, at a ratio that cannot reduce.
    c.set_ratio(1.0);
    c.set_knee_db(0.0);
    c.set_threshold_db(-60.0);
    c.reset();
    const double w = units::hz_to_radians_per_sample(kToneHz, kSr);
    double observed = 0.0;
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i)
        observed = std::max(observed, std::abs(c.process(std::sin(w * i))));
    REQUIRE_THAT(observed, WithinRel(bound, 1e-9));

    // Not exceeded anywhere else in the parameter space, including the mode
    // whose curve is unbounded below.
    Xorshift32 rng{20260725u};
    for (double threshold : {-60.0, -20.0, 0.0}) {
        for (double ratio : {1.0, 4.0, Vca::kRatioMax}) {
            for (double knee : {0.0, 24.0}) {
                for (bool negative : {false, true}) {
                    for (double mix : {0.0, 0.5, 1.0}) {
                        Vca s;
                        s.prepare(kSr);
                        s.set_threshold_db(threshold);
                        s.set_ratio(ratio);
                        s.set_knee_db(knee);
                        s.set_negative_ratio_mode(negative);
                        s.set_neg_ratio_amount(Vca::kNegRatioMin);
                        s.set_makeup_db(Vca::kMakeupDbMax);
                        s.set_mix(mix);
                        s.set_time_ms(Vca::kTimeMsMin);
                        s.reset();
                        rng.reset();
                        for (int i = 0; i < 4800; ++i) {
                            const double x = rng.next_bipolar<double>();
                            const double y = s.process(x);
                            REQUIRE(std::isfinite(y));
                            REQUIRE(std::abs(y) <= bound * std::abs(x) + 1e-12);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("7 mix 0 is the delayed dry signal, bit for bit", "[vca-compressor][gain]") {
    // The dry path is delayed by the SAME lookahead as the wet one, which is
    // what keeps a parallel blend from comb-filtering.
    Vca c;
    c.prepare(kSr);
    c.set_lookahead_ms(2.0);
    c.set_mix(0.0);
    c.set_makeup_db(Vca::kMakeupDbMax);  // must not reach the output at mix 0
    c.set_threshold_db(-60.0);
    c.set_ratio(Vca::kRatioMax);
    c.reset();

    const int latency = c.latency_samples();
    std::vector<double> in;
    Xorshift32 rng{7u};
    for (int i = 0; i < 4096; ++i) in.push_back(rng.next_bipolar<double>());
    for (int i = 0; i < static_cast<int>(in.size()); ++i) {
        const double y = c.process(in[i]);
        if (i >= latency) REQUIRE(y == in[i - latency]);
    }
}

// ── 8. Determinism ────────────────────────────────────────────────────────

TEST_CASE("8 render, reset, re-render is bit-identical", "[vca-compressor][determinism]") {
    const auto render = [](VcaCompressorT<float>& c, int n) {
        Xorshift32 rng{20260725u};
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(n));
        const double w = units::hz_to_radians_per_sample(kToneHz, kSr);
        for (int i = 0; i < n; ++i) {
            // 1 kHz sine plus a seeded noise burst, per the spec's signal.
            double x = 0.5 * std::sin(w * i);
            if (i > n / 3 && i < 2 * n / 3) x += 0.3 * rng.next_bipolar<double>();
            out.push_back(c.process(static_cast<float>(x)));
        }
        return out;
    };

    for (bool negative : {false, true}) {
        VcaCompressorT<float> c;
        c.prepare(kSr);
        c.set_threshold_db(-24.0);
        c.set_ratio(6.0);
        c.set_negative_ratio_mode(negative);
        c.set_neg_ratio_amount(-6.0);
        c.set_knee_db(10.0);
        c.set_time_ms(30.0);
        c.set_lookahead_ms(3.0);
        c.set_makeup_db(6.0);
        c.set_mix(0.75);
        c.reset();

        const auto first = render(c, static_cast<int>(kSr));
        c.reset();
        const auto second = render(c, static_cast<int>(kSr));
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
    }
}

TEST_CASE("8 reset returns the module to a silence-equivalent state",
          "[vca-compressor][determinism]") {
    const auto configure = [](Vca& c) {
        c.prepare(kSr);
        c.set_lookahead_ms(5.0);
        c.set_threshold_db(-30.0);
        c.set_ratio(8.0);
        c.reset();
    };

    Vca swept;
    configure(swept);
    const double w = units::hz_to_radians_per_sample(kToneHz, kSr);
    for (int i = 0; i < static_cast<int>(kSr); ++i) swept.process(std::sin(w * i));
    swept.reset();

    Vca fresh;
    configure(fresh);

    REQUIRE(swept.mean_square() == fresh.mean_square());
    REQUIRE(swept.gain_reduction_db() == fresh.gain_reduction_db());
    for (int i = 0; i < 4800; ++i) REQUIRE(swept.process(0.0) == fresh.process(0.0));
}

// ── 10. Latency ───────────────────────────────────────────────────────────

TEST_CASE("10 latency is the lookahead exactly, and the delay line owns all of it",
          "[vca-compressor][latency]") {
    for (double ms : {0.0, 5.0, Vca::kLookaheadMsMax}) {
        Vca c;
        c.prepare(kSr);
        c.set_lookahead_ms(ms);
        c.set_ratio(1.0);  // no reduction possible: a pure delay
        c.set_knee_db(0.0);
        c.set_makeup_db(0.0);
        c.reset();

        const int expected = static_cast<int>(std::llround(units::ms_to_samples(ms, kSr)));
        REQUIRE(c.latency_samples() == expected);

        int peak_index = -1;
        double peak_value = 0.0;
        for (int i = 0; i < expected + 64; ++i) {
            const double y = c.process(i == 0 ? 0.5 : 0.0);
            if (std::abs(y) > peak_value) {
                peak_value = std::abs(y);
                peak_index = i;
            }
        }
        REQUIRE(peak_index == expected);
        REQUIRE_THAT(peak_value, WithinAbs(0.5, 1e-12));
    }

    // The spec's named case, spelled out: 5 ms at 48 kHz is 240 samples.
    Vca c;
    c.prepare(48000.0);
    c.set_lookahead_ms(5.0);
    REQUIRE(c.latency_samples() == 240);
    // Default is zero latency (series law 5).
    c.set_lookahead_ms(0.0);
    REQUIRE(c.latency_samples() == 0);
}

// ── float/double parity ───────────────────────────────────────────────────

TEST_CASE("the float and double instantiations agree", "[vca-compressor]") {
    VcaCompressorT<float> f;
    VcaCompressorT<double> d;
    for (auto* c : {&f}) (void)c;

    f.prepare(kSr);
    d.prepare(kSr);
    for (int once = 0; once < 1; ++once) {
        f.set_threshold_db(-24.0);  d.set_threshold_db(-24.0);
        f.set_ratio(6.0);           d.set_ratio(6.0);
        f.set_knee_db(10.0);        d.set_knee_db(10.0);
        f.set_time_ms(30.0);        d.set_time_ms(30.0);
        f.set_lookahead_ms(2.0);    d.set_lookahead_ms(2.0);
        f.set_makeup_db(6.0);       d.set_makeup_db(6.0);
    }
    f.reset();
    d.reset();

    REQUIRE(f.latency_samples() == d.latency_samples());

    const double w = units::hz_to_radians_per_sample(kToneHz, kSr);
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i) {
        const double x = 0.7 * std::sin(w * i);
        const double yf = f.process(static_cast<float>(x));
        const double yd = d.process(x);
        REQUIRE_THAT(yf, WithinAbs(yd, 1e-4));
    }
}

// ── 9. RT allocation probe ────────────────────────────────────────────────

TEST_CASE("9 the compressor allocates nothing on the audio thread",
          "[vca-compressor][rt-safety]") {
    VcaCompressorT<float> f;
    VcaCompressorT<double> d;
    f.prepare(kSr);
    d.prepare(kSr);

    std::vector<float> block_f(256, 0.1f);
    std::vector<double> block_d(256, 0.1);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 64; ++i) {
            const double t = static_cast<double>(i);
            f.set_threshold_db(-60.0 + t);
            f.set_ratio(1.0 + 0.3 * t);
            f.set_negative_ratio_mode((i % 2) != 0);
            f.set_neg_ratio_amount(-1.0 - 0.3 * t);
            f.set_knee_db(0.375 * t);
            f.set_time_ms(1.0 + 7.8 * t);
            f.set_attack_release_ratio_k(2.0 + 0.09 * t);
            f.set_makeup_db(-24.0 + 0.75 * t);
            f.set_lookahead_ms(0.15 * t);
            f.set_mix(0.015 * t);
            f.set_ceiling_db(60.0 + 1.3 * t);

            d.set_threshold_db(-60.0 + t);
            d.set_ratio(1.0 + 0.3 * t);
            d.set_negative_ratio_mode((i % 2) != 0);
            d.set_neg_ratio_amount(-1.0 - 0.3 * t);
            d.set_knee_db(0.375 * t);
            d.set_time_ms(1.0 + 7.8 * t);
            d.set_attack_release_ratio_k(2.0 + 0.09 * t);
            d.set_makeup_db(-24.0 + 0.75 * t);
            d.set_lookahead_ms(0.15 * t);
            d.set_mix(0.015 * t);
            d.set_ceiling_db(60.0 + 1.3 * t);

            (void)f.process(0.5f);
            f.process_block(block_f.data(), static_cast<int>(block_f.size()));
            (void)d.process(0.5);
            d.process_block(block_d.data(), static_cast<int>(block_d.size()));

            (void)f.latency_samples();
            (void)f.gain_reduction_db();
            (void)f.level_db();
            (void)f.mean_square();
            (void)f.current_gain_linear();
            (void)f.static_curve_db(-12.0);
            (void)f.gain_computer_db(-12.0);
        }
        f.reset();
        d.reset();
    });
}

TEST_CASE("a NaN sample cannot latch the VCA detector",
          "[vca-compressor][nan-recovery]") {
    for (double sample_rate : {8000.0, 192000.0}) {
        Vca c;
        c.prepare(sample_rate);
        c.set_threshold_db(-30.0);
        c.set_ratio(8.0);
        for (int i = 0; i < static_cast<int>(sample_rate * 0.1); ++i) c.process(0.5);

        c.process(std::numeric_limits<double>::quiet_NaN());
        for (int i = 0; i < 64; ++i) {
            REQUIRE(std::isfinite(c.process(0.25)));
            REQUIRE(std::isfinite(c.mean_square()));
            REQUIRE(std::isfinite(c.gain_reduction_db()));
        }
    }
}

TEST_CASE("non-finite VCA controls retain the last valid configuration",
          "[vca-compressor][nan-recovery]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Vca c, reference;
        for (auto* x : {&c, &reference}) {
            x->prepare(kSr); x->set_threshold_db(-31.0); x->set_ratio(9.0);
            x->set_neg_ratio_amount(0.4); x->set_knee_db(4.0); x->set_time_ms(41.0);
            x->set_attack_release_ratio_k(13.0); x->set_makeup_db(-3.0);
            x->set_lookahead_ms(7.0); x->set_mix(0.72); x->set_ceiling_db(-11.0);
        }
        c.set_threshold_db(bad); c.set_ratio(bad); c.set_neg_ratio_amount(bad);
        c.set_knee_db(bad); c.set_time_ms(bad); c.set_attack_release_ratio_k(bad);
        c.set_makeup_db(bad); c.set_lookahead_ms(bad); c.set_mix(bad); c.set_ceiling_db(bad);
        for (int i = 0; i < 512; ++i) {
            const double sample = 0.25 * std::sin(0.031 * i);
            REQUIRE(c.process(sample) == reference.process(sample));
        }
    }
}

TEST_CASE("VCA audio faults logically clear lookahead and recover exactly",
          "[vca-compressor][nan-recovery][rt-safety]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Vca poisoned, fresh;
        for (auto* x : {&poisoned, &fresh}) {
            x->prepare(kSr); x->set_threshold_db(-28.0); x->set_ratio(7.0);
            x->set_lookahead_ms(8.0); x->set_mix(0.71);
        }
        for (int i = 0; i < 600; ++i) (void)poisoned.process(0.4);
        require_allocates_no_memory([&] { REQUIRE(poisoned.process(bad) == 0.0); });
        fresh.reset();
        for (int i = 0; i < 768; ++i) {
            const double sample = 0.2 * std::sin(0.023 * i);
            REQUIRE(poisoned.process(sample) == fresh.process(sample));
        }
    }
}
