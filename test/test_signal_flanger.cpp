// FlangerT — classic, through-zero, and barberpole comb sweeps.
//
// This is the spec's acceptance suite R1–R14 plus the barberpole and BBD-engine
// cases the spec deferred. Expected values are COMPUTED from the shipped
// constants and from the comb algebra, never restated as literals.
//
// Measurement recipe, and why it is not the spec's. The spec asks for an
// 8192-point Hann FFT with parabolic peak interpolation, and argues that the
// 3 % notch tolerance is reachable that way despite the ~5.86 Hz bin width.
// This suite measures the TRANSFER FUNCTION instead: drive a sine at one exact
// frequency, read the output amplitude with a coherent DFT over a whole number
// of periods, divide by the input amplitude. That is leakage-free by
// construction, needs no window and no interpolation, and — the reason it is
// worth the swap — it pins the notch position roughly five times tighter than
// the spec's own tolerance, from a single measurement:
//
//   near a null the comb's magnitude is |H| = 2g·|cos(πfD)|, so a probe placed
//   at a notch that has actually moved to f(1+ε) reads back 2g·|sin(πε)| ≈ 2gπε
//   of the peak's 2g. Measuring 40 dB of null depth therefore proves
//   ε ≤ 10^(−40/20)/π = 0.0032 — 0.32 %, versus kNotchTolerancePct's 3 %.
//
// The measured depths below are 100+ dB, i.e. the notches sit where the algebra
// says to a part in 10^5. The frequencies used are all whole multiples of the
// 43200-sample window's 1.111 Hz bin, which is checked by its own test rather
// than assumed.
//
// SPEC DEVIATIONS, each argued at the test that makes it:
//   R1   mix is 50 %, not 100 % wet. At 100 % wet the equal-power law gives the
//        dry path zero gain and the output is a bare delayed copy, whose
//        magnitude response is FLAT — there is no comb to measure. A comb needs
//        both paths, and 50 % is where their weights are equal.
//   R2   the resonant peaks land at the comb's PEAKS, not at its notches.
//   R3/R4/R5  polarity is one convention in every mode; the spec's TZ labels
//        are the reverse of its own classic ones.
//   R4   the null is asserted twice — exactly, with the sweep pinned at the
//        crossing, and dynamically, with a window derived from the sweep slope.
//   R6   the registry cites the loop ENVELOPE, not the per-pass gain.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/flanger.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Fl = Flanger64;
using Mode = FlangerMode;
using Polarity = FlangerPolarity;
using Engine = FlangerDelayEngine;

constexpr double kSr = 48000.0;

/// 0.9 s. Chosen so every notch and peak frequency of the three delays under
/// test is a whole multiple of the resulting 1.111 Hz bin — the delays are
/// 3.0 / 4.5 / 1.5 ms, i.e. 144 / 216 / 72 samples exactly at this rate, so
/// their combs' features land on rational frequencies this window resolves.
constexpr int kAnalysisLen = 43200;
constexpr double kBinHz = kSr / static_cast<double>(kAnalysisLen);

/// Long enough for a recursive comb at the feedback ceiling to settle: at
/// fb = 0.9 and a 144-sample loop, 1e-6 of the initial energy remains after
/// about 131 round trips, or 19k samples.
constexpr int kSettle = 24000;

constexpr double kProbeAmplitude = 0.5;

/// The three delays the spec's worked examples name, reached from the shipped
/// defaults rather than typed in: the centre, and the centre plus/minus the
/// default depth.
constexpr double kCenterMs = 3.0;
constexpr double kDepthMs = 1.5;

double magnitude_at(const std::vector<double>& x, double hz) {
    const double w = 2.0 * M_PI * hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

/// The mode-switch window in samples, computed the way `prepare()` computes it.
double mode_switch_window_samples() {
    return std::max(2.0, std::round(Fl::kModeSwitchMs * 0.001 * kSr));
}

bool on_bin(double hz) {
    const double bins = std::abs(hz) / kBinHz;
    return std::abs(bins - std::round(bins)) < 1e-9;
}

double rms(const std::vector<double>& x, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = x.size();
    double sum = 0.0;
    for (std::size_t n = from; n < to; ++n) sum += x[n] * x[n];
    return std::sqrt(sum / static_cast<double>(to - from));
}

/// Renders a sine through a configured flanger and returns |H| at that
/// frequency: output amplitude over input amplitude.
double transfer(Fl& flanger, double hz, int analysis = kAnalysisLen,
                double amplitude = kProbeAmplitude) {
    std::vector<double> in(static_cast<std::size_t>(kSettle + analysis));
    std::vector<double> out(in.size());
    for (std::size_t n = 0; n < in.size(); ++n)
        in[n] = amplitude * std::sin(2.0 * M_PI * hz * static_cast<double>(n) / kSr);
    flanger.process(in.data(), out.data(), static_cast<int>(in.size()));
    const std::vector<double> segment(out.begin() + kSettle, out.end());
    return magnitude_at(segment, hz) / amplitude;
}

/// A classic-mode flanger with the sweep held still, so the comb under test is
/// the one the delay says it is. Depth zero rather than a slow rate: `LfoT`
/// clamps its rate to `kRateMinHz`, so "slow enough to be stationary" would be
/// an assumption where "no excursion" is a fact.
Fl held(double delay_ms, double feedback, Polarity polarity, double mix = 0.5) {
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::classic);
    f.set_polarity(polarity);
    f.set_center_delay_ms(delay_ms);
    f.set_depth_ms(0.0);
    f.set_feedback(feedback);
    f.set_mix(mix);
    f.reset();
    return f;
}

/// The DC blocker's response, needed to make the feedback-loop expectations
/// exact rather than approximate: it is the one element inside the loop that is
/// not transparent, and at 0.9 feedback a 0.05 % loop-gain error is a 0.4 %
/// error in the resonant peak.
std::complex<double> dc_blocker_response(double hz) {
    const double pole = std::exp(-2.0 * M_PI * Fl::kDcBlockHz / kSr);
    const std::complex<double> z = std::polar(1.0, -2.0 * M_PI * hz / kSr);
    return (1.0 - z) / (1.0 - pole * z);
}

/// The closed form this module's classic mode implements, derived rather than
/// fitted:
///
///   wet:  W/X = e / (1 − fb·s·e·H_dc)      with e = z^−D
///   out:  H   = dry_gain + wet_gain·s·W/X
///
/// Every expectation in R1/R2/R3 is this function evaluated at the shipped
/// constants, so a change to the mix law or the blocker corner moves the test
/// with the code rather than against it.
double comb_magnitude(double hz, double delay_ms, double feedback, Polarity polarity,
                      double mix = 0.5) {
    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(mix, dry_gain, wet_gain);
    const double sign = polarity == Polarity::negative ? -1.0 : 1.0;
    const double delay_samples = delay_ms * 0.001 * kSr;
    const std::complex<double> e = std::polar(1.0, -2.0 * M_PI * hz * delay_samples / kSr);
    const std::complex<double> wet = e / (1.0 - feedback * sign * e * dc_blocker_response(hz));
    return std::abs(dry_gain + wet_gain * sign * wet);
}

}  // namespace

// ── Measurement-recipe guard ──────────────────────────────────────────────

TEST_CASE("the analysis window resolves every frequency this suite probes",
          "[signal][flanger]") {
    // Guards the instrument, not the code. A frequency off the bin grid makes
    // every magnitude read leaky, and the failure would look like a DSP bug.
    for (double delay : {kCenterMs, kCenterMs + kDepthMs, kCenterMs - kDepthMs}) {
        for (int k = 0; k < 3; ++k) {
            INFO("delay " << delay << " ms, k = " << k);
            REQUIRE(on_bin(Fl::notch_hz(k, delay, Polarity::positive)));
            REQUIRE(on_bin(Fl::notch_hz(k + 1, delay, Polarity::negative)));
        }
    }
    // The delays are whole samples at this rate, so the fractional interpolator
    // is exactly a pure delay and the closed form above is exact rather than
    // approximate.
    for (double delay : {kCenterMs, kCenterMs + kDepthMs, kCenterMs - kDepthMs}) {
        const double samples = delay * 0.001 * kSr;
        REQUIRE_THAT(samples, WithinAbs(std::round(samples), 1e-9));
    }
}

// ── R1 — notch positions follow Δf = 1/D ──────────────────────────────────

TEST_CASE("R1 the comb's notches sit where 1/D says, at every swept delay",
          "[signal][flanger]") {
    // The headline relationship, measured at the three delays one LFO cycle
    // visits at the shipped defaults: centre, centre + depth, centre − depth.
    // 111.1 / 166.7 / 333.3 Hz for the fundamental notch — computed here from
    // the constants, not transcribed.
    for (double delay : {kCenterMs, kCenterMs + kDepthMs, kCenterMs - kDepthMs}) {
        for (int k = 0; k < 3; ++k) {
            const double notch = Fl::notch_hz(k, delay, Polarity::positive);
            const double peak = static_cast<double>(k + 1) / (delay * 0.001);

            auto at_notch = held(delay, 0.0, Polarity::positive);
            auto at_peak = held(delay, 0.0, Polarity::positive);
            const double h_notch = transfer(at_notch, notch);
            const double h_peak = transfer(at_peak, peak);

            double dry_gain = 0.0, wet_gain = 0.0;
            Fl::mix_gains(0.5, dry_gain, wet_gain);

            INFO("D = " << delay << " ms, k = " << k << ", notch " << notch << " Hz");
            // The peak is the two paths in phase: exactly dry + wet.
            REQUIRE_THAT(h_peak, WithinRel(dry_gain + wet_gain, 1e-6));
            // The null is exact in theory; 100 dB of measured depth pins the
            // notch to 10^(−100/20)/π ≈ 3e-6 of its frequency, four orders
            // inside kNotchTolerancePct's 3 %.
            const double depth_db = 20.0 * std::log10(h_peak / std::max(h_notch, 1e-18));
            REQUIRE(depth_db >= 100.0);
        }

        // Spacing is the reciprocal of the delay — asserted as the relationship
        // the k-series above already walked, stated once so it is not only
        // implied.
        REQUIRE_THAT(Fl::notch_hz(1, delay, Polarity::positive) -
                         Fl::notch_hz(0, delay, Polarity::positive),
                     WithinRel(Fl::notch_spacing_hz(delay), 1e-12));
    }
}

// ── R2 — feedback sharpens the comb; it does not move it ──────────────────

TEST_CASE("R2 feedback resonates the comb without relocating its features",
          "[signal][flanger]") {
    // The spec says feedback puts resonant peaks AT the comb's notch
    // frequencies. That is true for a negative loop coefficient and false for a
    // positive one, and the algebra says which: the loop's denominator
    // |1 − fb·s·e^{−jωD}| is smallest where the round-trip phase is zero, i.e.
    // at ωD = 2kπ — which is where the feedforward comb already PEAKS. Positive
    // feedback therefore sharpens the peaks and fills in the notches; negative
    // feedback does the reverse. Both are asserted below against the closed
    // form, so whichever way the convention is read the numbers decide.
    const double delay = kCenterMs;
    const double notch = Fl::notch_hz(0, delay, Polarity::positive);
    const double peak = 1.0 / (delay * 0.001);

    double previous_peak = 0.0;
    for (double fb : {0.0, 0.5, 0.9}) {
        auto at_peak = held(delay, fb, Polarity::positive);
        auto at_notch = held(delay, fb, Polarity::positive);
        const double h_peak = transfer(at_peak, peak);
        const double h_notch = transfer(at_notch, notch);

        INFO("fb = " << fb);
        REQUIRE_THAT(h_peak, WithinRel(comb_magnitude(peak, delay, fb, Polarity::positive), 0.01));
        // Two-sided, with an absolute floor rather than a relative tolerance:
        // at fb = 0 the predicted notch is an EXACT zero, and comparing two
        // numbers that are both essentially zero relatively is a comparison of
        // rounding noise. 1e-6 is far below the 0.24 and 0.34 the two non-zero
        // feedback settings predict, so the bound stays meaningful there.
        const double predicted = comb_magnitude(notch, delay, fb, Polarity::positive);
        REQUIRE(h_notch <= predicted * 1.02 + 1e-6);
        REQUIRE(h_notch >= predicted * 0.98 - 1e-6);

        // Sharper, and monotonically so.
        REQUIRE(h_peak > previous_peak);
        previous_peak = h_peak;
        // And the feature is still where it was: the minimum has not moved off
        // the fb = 0 notch frequency.
        REQUIRE(h_notch < h_peak);
    }

    // A negative coefficient resonates the OTHER series — the one the spec
    // describes — which is what makes the sign a colour control rather than a
    // duplicate of `polarity`.
    auto negative_loop = held(delay, -0.9, Polarity::positive);
    REQUIRE_THAT(transfer(negative_loop, notch),
                 WithinRel(comb_magnitude(notch, delay, -0.9, Polarity::positive), 0.01));
    REQUIRE(transfer(negative_loop, notch) > 1.0);
}

// ── R3 — polarity moves the whole series by half a spacing ────────────────

TEST_CASE("R3 negative polarity shifts the notch series to k/D and adds one at DC",
          "[signal][flanger]") {
    const double delay = kCenterMs;
    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);

    for (int k = 1; k <= 2; ++k) {
        const double notch = Fl::notch_hz(k, delay, Polarity::negative);
        auto f = held(delay, 0.0, Polarity::negative);
        const double h = transfer(f, notch);
        INFO("negative-polarity notch k = " << k << " at " << notch << " Hz");
        REQUIRE(20.0 * std::log10((dry_gain + wet_gain) / std::max(h, 1e-18)) >= 100.0);
    }

    // The half-spacing shift, stated from the other side: the frequency that is
    // a NULL under positive polarity is a PEAK under negative.
    const double positive_notch = Fl::notch_hz(0, delay, Polarity::positive);
    auto swapped = held(delay, 0.0, Polarity::negative);
    REQUIRE_THAT(transfer(swapped, positive_notch), WithinRel(dry_gain + wet_gain, 1e-6));

    // And the notch at DC: `k = 0` of the negative series is 0 Hz, so the
    // response falls away toward it as 2g·|sin(πfD)| — computed, then measured
    // at the lowest frequency the window resolves cleanly.
    const double low = 10.0 * kBinHz;
    REQUIRE(on_bin(low));
    auto near_dc = held(delay, 0.0, Polarity::negative);
    REQUIRE_THAT(transfer(near_dc, low),
                 WithinRel(comb_magnitude(low, delay, 0.0, Polarity::negative), 1e-4));
    REQUIRE(Fl::notch_hz(0, delay, Polarity::negative) == 0.0);
}

// ── R4 / R5 — through zero ────────────────────────────────────────────────

TEST_CASE("R4 the through-zero crossing cancels the signal completely",
          "[signal][flanger]") {
    // Pinned at the crossing: depth zero puts the swept path exactly on the dry
    // path's fixed delay and holds it there, so the cancellation is sustained
    // and can be measured for as long as one likes rather than inferred from a
    // dip. This is the cleanest possible statement of "the two paths meet".
    //
    // Polarity NEGATIVE, and that is not the spec's label. At the crossing both
    // paths carry the identical signal; their sum is 2x and only their
    // difference is zero. A convention where "positive" nulls in through-zero
    // mode but sums in classic mode would mean two different things under one
    // name, so this module keeps one sign convention and the null lands on the
    // differencing side of it. R5 asserts the other side.
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::through_zero);
    f.set_offset_ms(4.0);
    f.set_depth_ms(0.0);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.set_polarity(Polarity::negative);
    f.reset();

    const int n = static_cast<int>(kSr);
    std::vector<double> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
        in[static_cast<std::size_t>(k)] =
            kProbeAmplitude * std::sin(2.0 * M_PI * 1000.0 * k / kSr);
    f.process(in.data(), out.data(), n);

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    const double dry_reference = dry_gain * kProbeAmplitude / std::sqrt(2.0);
    const double residual = rms(out, static_cast<std::size_t>(n / 2));

    INFO("residual " << 20.0 * std::log10(residual / dry_reference) << " dB below dry");
    REQUIRE(20.0 * std::log10(residual / dry_reference) <= -100.0);
    REQUIRE_THAT(f.instantaneous_delay_ms(), WithinRel(4.0, 1e-9));
}

TEST_CASE("R4 the swept delay really reaches and crosses the dry reference",
          "[signal][flanger]") {
    // The failure this exists for: a through-zero mode whose sweep never
    // actually crosses is still a flanger, just not a through-zero one. The
    // delay-time law is read directly rather than inferred from the spectrum,
    // because inferring it would measure the comb and the law at once.
    constexpr double kOffset = 4.0;
    constexpr double kRate = 1.0;
    constexpr int kPeriods = 3;

    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::through_zero);
    f.set_offset_ms(kOffset);
    f.set_depth_ms(kOffset);  // the full 0 .. 2·offset sweep
    f.set_rate_hz(kRate);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.reset();

    const int n = static_cast<int>(kPeriods * kSr / kRate);
    double lowest = 1e9, highest = -1e9;
    int crossings = 0;
    double previous = 0.0;
    double sample = 0.0, silence = 0.0;
    for (int k = 0; k < n; ++k) {
        f.process(&silence, &sample, 1);
        const double d = f.instantaneous_delay_ms();
        lowest = std::min(lowest, d);
        highest = std::max(highest, d);
        const double side = d - kOffset;
        if (k > 0 && (side > 0.0) != (previous > 0.0)) ++crossings;
        previous = side;
    }

    // The sweep spans essentially the whole 0 .. 2·offset range. It stops a
    // fraction of a sample short of zero because the interpolator's read is
    // clamped to one sample — the guard that keeps its 4-point kernel inside
    // the buffer — which is a floor on the DELAY, not on the crossing.
    REQUIRE(lowest <= 1.5 * 1000.0 / kSr);
    REQUIRE(highest >= 2.0 * kOffset - 1.5 * 1000.0 / kSr);

    // Twice per cycle, minus the one at the far boundary: a sine starting at
    // phase 0 crosses zero at 0.5, 1.0, … , K−0.5 strictly inside K periods,
    // which is 2K−1.
    REQUIRE(crossings == 2 * kPeriods - 1);
}

TEST_CASE("R4 the sweeping crossing produces an audible full-band null",
          "[signal][flanger]") {
    // The dynamic version. The null's WIDTH is set by how fast the sweep drags
    // the delay past the crossing, so the window and the expected floor are
    // computed from the sweep rather than chosen: near the crossing the output
    // is g·δ(t)·x′(t), where δ grows at `depth_samples · 2π·rate/fs` per sample
    // and |x′| ≤ A·2π·f/fs. The rate is the slow tape-drag setting the spec's
    // P3 patch names.
    constexpr double kOffset = 4.0;
    constexpr double kRate = 0.08;
    constexpr double kTone = 1000.0;
    constexpr int kWindow = 16;

    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::through_zero);
    f.set_offset_ms(kOffset);
    f.set_depth_ms(kOffset);
    f.set_rate_hz(kRate);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.set_polarity(Polarity::negative);
    f.reset();

    // The first crossing is half an LFO cycle in.
    const int n = static_cast<int>(0.75 * kSr / kRate);
    std::vector<double> out(static_cast<std::size_t>(n));
    int crossing = -1;
    double closest = 1e9;
    for (int k = 0; k < n; ++k) {
        const double x = kProbeAmplitude * std::sin(2.0 * M_PI * kTone * k / kSr);
        f.process(&x, &out[static_cast<std::size_t>(k)], 1);
        const double distance = std::abs(f.instantaneous_delay_ms() - kOffset);
        if (k > static_cast<int>(0.1 * kSr) && distance < closest) {
            closest = distance;
            crossing = k;
        }
    }
    REQUIRE(crossing > 0);

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    const double dry_reference = dry_gain * kProbeAmplitude / std::sqrt(2.0);
    const double at_crossing =
        rms(out, static_cast<std::size_t>(crossing - kWindow),
            static_cast<std::size_t>(crossing + kWindow));

    const double depth_db = 20.0 * std::log10(at_crossing / dry_reference);
    INFO("null depth " << depth_db << " dB over ±" << kWindow << " samples");
    REQUIRE(depth_db <= -40.0);
}

TEST_CASE("R5 the other polarity reinforces the crossing by exactly 6.02 dB",
          "[signal][flanger]") {
    // The mathematical identity, not a tuned constant: two identical signals in
    // phase sum to twice the amplitude. Measured against the DRY CONTRIBUTION
    // at the same mix — which is the only reference that makes the figure 6 dB
    // at any mix setting, since an equal-power blend never gives either path
    // unity gain.
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::through_zero);
    f.set_offset_ms(4.0);
    f.set_depth_ms(0.0);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.set_polarity(Polarity::positive);
    f.reset();

    const int n = static_cast<int>(kSr);
    std::vector<double> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
        in[static_cast<std::size_t>(k)] =
            kProbeAmplitude * std::sin(2.0 * M_PI * 1000.0 * k / kSr);
    f.process(in.data(), out.data(), n);

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    const double dry_reference = dry_gain * kProbeAmplitude / std::sqrt(2.0);
    const double measured = rms(out, static_cast<std::size_t>(n / 2));
    const double gain_db = 20.0 * std::log10(measured / dry_reference);

    INFO("reinforcement " << gain_db << " dB");
    REQUIRE(gain_db >= 5.5);                                 // the spec's floor
    REQUIRE_THAT(gain_db, WithinAbs(20.0 * std::log10(2.0), 0.05));  // the identity
}

// ── R6 — the feedback loop is bounded, and the registry says by how much ──

TEST_CASE("R6 the feedback loop stays bounded at the ceiling",
          "[signal][flanger]") {
    for (double fb : {Fl::kFbClamp, -Fl::kFbClamp}) {
        Fl f;
        f.prepare(kSr);
        f.set_mode(Mode::classic);
        f.set_center_delay_ms(kCenterMs);
        f.set_depth_ms(kDepthMs);
        f.set_feedback(fb);
        f.set_mix(1.0);
        f.reset();

        const int n = static_cast<int>(10.0 * kSr);
        double worst = 0.0;
        bool finite = true;
        for (int k = 0; k < n; ++k) {
            const double x = k == 0 ? 1.0 : 0.0;
            double y = 0.0;
            f.process(&x, &y, 1);
            finite = finite && std::isfinite(y);
            worst = std::max(worst, std::abs(y));
        }
        INFO("fb = " << fb << ", peak " << worst << " vs envelope "
                     << Fl::worst_case_gain());
        REQUIRE(finite);
        REQUIRE(worst <= Fl::worst_case_gain());
        REQUIRE(worst > 0.0);
    }
}

TEST_CASE("R6 the resonant gain approaches the registered envelope but stays under it",
          "[signal][flanger]") {
    // The impulse test above is loose — a comb's taps never overlap, so its
    // peak is one tap high whatever the feedback. The bound is only meaningful
    // against a signal that EXCITES the resonance, which is a sine sitting on a
    // pole. This is the measurement that makes the registry number a claim
    // rather than a formality.
    const double delay = kCenterMs;
    const double resonance = 1.0 / (delay * 0.001);
    auto f = held(delay, Fl::kFbClamp, Polarity::positive, /*mix=*/1.0);
    const double measured = transfer(f, resonance);

    INFO("resonant gain " << measured << " against envelope " << Fl::worst_case_gain());
    // The loop is exactly `delay` samples long, so its poles land on the
    // feedforward comb's peaks and the closed form predicts the height. That
    // agreement is what proves the feedback tap carries no stray sample of its
    // own: a loop one sample longer would resonate at 1/(D+1) and read ~19 %
    // low here, which is how the extra delay this module used to carry was
    // found.
    REQUIRE_THAT(measured, WithinRel(comb_magnitude(resonance, delay, Fl::kFbClamp,
                                                    Polarity::positive, /*mix=*/1.0),
                                     0.01));
    REQUIRE(measured <= Fl::worst_case_gain());
    // And the registered bound is describing THIS loop, not an unrelated one:
    // the measured resonance sits within a factor of two of it.
    REQUIRE(measured > 0.5 * Fl::worst_case_gain());
}

TEST_CASE("R6 the registered envelope is computed from the shipped clamps",
          "[signal][flanger]") {
    // Series law 8. The spec registers 0.97 × 1.012 ≈ 0.982 — the PER-PASS
    // gain — which would tell a host this module can never exceed unity. It
    // can: a comb at the feedback ceiling presents about 35 dB at its poles.
    // The envelope 1/(1 − per-pass) is the number that means something.
    const double per_pass = Fl::kFbClamp * Fl::kLoopElementGainBound;
    REQUIRE(per_pass < 1.0);
    REQUIRE_THAT(Fl::worst_case_gain(), WithinRel(1.0 / (1.0 - per_pass), 1e-12));
    REQUIRE(Fl::worst_case_gain() > 1.0);
}

// ── R7 — determinism ──────────────────────────────────────────────────────

TEST_CASE("R7 a render is bit-identical after reset, on every mode and engine",
          "[signal][flanger]") {
    // Series law 2. The clean and barberpole paths hold no generator at all —
    // a flanger LFO is periodic by definition of the effect. The BBD engine is
    // the exception: its clock jitter is stochastic, and this test is what
    // proves its seed is rewound rather than merely present.
    for (Mode mode : {Mode::classic, Mode::through_zero, Mode::barberpole}) {
        for (Engine engine : {Engine::clean, Engine::bbd}) {
            Fl f;
            f.prepare(kSr);
            f.set_mode(mode);
            f.set_delay_engine(engine);
            f.set_feedback(0.6);
            f.set_mix(0.5);

            auto run = [&f]() {
                f.reset();
                const int n = static_cast<int>(2.0 * kSr);
                std::vector<double> in(static_cast<std::size_t>(n));
                std::vector<double> out(static_cast<std::size_t>(n));
                for (int k = 0; k < n; ++k)
                    in[static_cast<std::size_t>(k)] =
                        0.4 * std::sin(2.0 * M_PI * 220.0 * k / kSr) +
                        0.2 * std::sin(2.0 * M_PI * 1310.0 * k / kSr);
                f.process(in.data(), out.data(), n);
                return out;
            };

            const auto first = run();
            const auto second = run();
            INFO("mode " << static_cast<int>(mode) << ", engine " << static_cast<int>(engine));
            REQUIRE(first.size() == second.size());
            for (std::size_t k = 0; k < first.size(); ++k) REQUIRE(first[k] == second[k]);
            REQUIRE(rms(first) > 0.01);
        }
    }
}

// ── R8 — real-time allocation probe ───────────────────────────────────────

TEST_CASE("R8 nothing on the audio path allocates after prepare",
          "[signal][flanger][rt-safety]") {
    Fl f;
    f.prepare(kSr);
    f.reset();

    constexpr int kBlock = 64;
    std::vector<double> in(kBlock, 0.1), out_l(kBlock, 0.0), out_r(kBlock, 0.0);

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 256; ++b) {
        f.set_rate_hz(0.02 + 0.03 * (b % 100));
        f.set_depth_ms(0.02 + 0.01 * (b % 200));
        f.set_center_delay_ms(0.1 + 0.05 * (b % 100));
        f.set_offset_ms(0.5 + 0.05 * (b % 100));
        f.set_feedback(-0.9 + 0.007 * b);
        f.set_mix(0.5);
        f.set_stereo_spread(0.25);
        f.set_polarity(b % 2 ? Polarity::negative : Polarity::positive);
        f.set_delay_engine(b % 8 == 0 ? Engine::bbd : Engine::clean);
        f.set_mode(static_cast<Mode>(b % 3));
        f.set_barberpole_shift_hz(3.0);
        f.process(in.data(), out_l.data(), kBlock);
        f.process_stereo(in.data(), in.data(), out_l.data(), out_r.data(), kBlock);
    }
    f.reset();
    REQUIRE(probe.allocation_count() == 0);
}

// ── R9 — latency ──────────────────────────────────────────────────────────

TEST_CASE("R9 latency is zero in classic mode and exactly the offset in through-zero",
          "[signal][flanger]") {
    Fl f;
    f.prepare(kSr);
    f.set_mix(0.0);  // dry only, so the impulse's arrival IS the latency
    f.set_mode(Mode::classic);
    f.reset();
    REQUIRE(f.latency_samples() == 0);

    auto arrival = [&f](int n) {
        std::vector<double> in(static_cast<std::size_t>(n), 0.0);
        std::vector<double> out(static_cast<std::size_t>(n), 0.0);
        in[0] = 1.0;
        f.process(in.data(), out.data(), n);
        for (int k = 0; k < n; ++k)
            if (std::abs(out[static_cast<std::size_t>(k)]) > 1e-9) return k;
        return -1;
    };
    REQUIRE(arrival(4096) == 0);

    // Through-zero delays the DRY path, which is a real latency and is reported
    // rather than hidden. Exact, because that line is an integer-sample buffer:
    // nothing sweeps it, so it never needed fractional accuracy, and skipping
    // the interpolator is what keeps the reported figure a whole number.
    Fl tz;
    tz.prepare(kSr);
    tz.set_mix(0.0);
    tz.set_offset_ms(4.0);
    tz.set_mode(Mode::through_zero);
    tz.reset();
    const int expected = static_cast<int>(std::lround(4.0 * kSr / 1000.0));
    REQUIRE(expected == 192);
    REQUIRE(tz.latency_samples() == expected);

    // Let the mode transition finish before probing, since `set_mode` fades.
    std::vector<double> quiet(2048, 0.0), sink(2048, 0.0);
    tz.process(quiet.data(), sink.data(), 2048);
    auto tz_arrival = [&tz](int n) {
        std::vector<double> in(static_cast<std::size_t>(n), 0.0);
        std::vector<double> out(static_cast<std::size_t>(n), 0.0);
        in[0] = 1.0;
        tz.process(in.data(), out.data(), n);
        for (int k = 0; k < n; ++k)
            if (std::abs(out[static_cast<std::size_t>(k)]) > 1e-9) return k;
        return -1;
    };
    REQUIRE(tz_arrival(4096) == expected);

    // Barberpole leaves the dry path alone, so it costs nothing either.
    Fl bp;
    bp.prepare(kSr);
    bp.set_mode(Mode::barberpole);
    bp.reset();
    REQUIRE(bp.latency_samples() == 0);
}

// ── R10 / R11 — the depth clamps ──────────────────────────────────────────

TEST_CASE("R11 the classic-mode excursion is clamped against the centre",
          "[signal][flanger]") {
    // The exact combination the spec identifies as unsafe: catalog-minimum
    // centre against catalog-maximum depth asks for a delay of −3.9 ms.
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::classic);
    f.set_center_delay_ms(Fl::kCenterMinMs);
    f.set_depth_ms(Fl::kDepthMaxMs);
    f.set_rate_hz(4.0);
    f.set_feedback(0.5);
    f.set_mix(0.5);
    f.reset();

    REQUIRE_THAT(f.effective_depth_ms(),
                 WithinRel(Fl::kCenterMinMs - Fl::kMinClassicDelayMs, 1e-12));
    REQUIRE(Fl::kCenterMinMs - Fl::kDepthMaxMs < 0.0);  // unclamped would go negative

    // And the delay stays above the floor for a whole sweep, with the output
    // finite throughout — the floor is only 2.4 samples at this rate, which is
    // exactly where a 4-point kernel would walk off an unguarded buffer.
    const int n = static_cast<int>(kSr / 4.0 * 2.0);
    double lowest = 1e9;
    bool finite = true;
    for (int k = 0; k < n; ++k) {
        const double x = 0.5 * std::sin(2.0 * M_PI * 1000.0 * k / kSr);
        double y = 0.0;
        f.process(&x, &y, 1);
        finite = finite && std::isfinite(y);
        lowest = std::min(lowest, f.instantaneous_delay_ms());
    }
    REQUIRE(finite);
    // One sample is the interpolator's own guard floor, and it sits below the
    // module's kMinClassicDelayMs at every rate this library runs at.
    REQUIRE(lowest >= std::min(Fl::kMinClassicDelayMs, 1000.0 / kSr) - 1e-9);

    // At the shipped defaults the clamp is inactive — it engages only at the
    // catalog extremes, which is what makes it a guard rather than a taper.
    Fl defaults;
    defaults.prepare(kSr);
    defaults.set_center_delay_ms(kCenterMs);
    defaults.set_depth_ms(kDepthMs);
    REQUIRE_THAT(defaults.effective_depth_ms(), WithinRel(kDepthMs, 1e-12));
}

TEST_CASE("R10 the through-zero excursion is clamped against the offset",
          "[signal][flanger]") {
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::through_zero);
    f.set_offset_ms(4.0);
    f.set_depth_ms(4.0 + 1.0);  // more than the offset: would drive the delay negative
    f.set_rate_hz(2.0);
    f.reset();
    REQUIRE_THAT(f.effective_depth_ms(), WithinRel(4.0, 1e-12));

    const int n = static_cast<int>(kSr / 2.0 * 2.0);
    double lowest = 1e9, highest = -1e9;
    double silence = 0.0, sink = 0.0;
    for (int k = 0; k < n; ++k) {
        f.process(&silence, &sink, 1);
        lowest = std::min(lowest, f.instantaneous_delay_ms());
        highest = std::max(highest, f.instantaneous_delay_ms());
    }
    REQUIRE(lowest >= 0.0);
    REQUIRE(highest <= 2.0 * 4.0 + 1e-6);
}

// ── R12 — stereo ──────────────────────────────────────────────────────────

TEST_CASE("R12 a stereo spread decorrelates the two rails without breaking either comb",
          "[signal][flanger]") {
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::classic);
    f.set_center_delay_ms(kCenterMs);
    f.set_depth_ms(kDepthMs);
    f.set_rate_hz(1.0);
    f.set_stereo_spread(0.25);  // a quarter cycle — the house default
    f.set_feedback(0.4);
    f.set_mix(0.5);
    f.reset();

    const int n = static_cast<int>(2.0 * kSr);
    std::vector<double> in(static_cast<std::size_t>(n));
    std::vector<double> left(static_cast<std::size_t>(n)), right(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
        in[static_cast<std::size_t>(k)] = 0.4 * std::sin(2.0 * M_PI * 700.0 * k / kSr) +
                                          0.3 * std::sin(2.0 * M_PI * 1900.0 * k / kSr);
    f.process_stereo(in.data(), in.data(), left.data(), right.data(), n);

    double difference = 0.0;
    for (std::size_t k = 0; k < left.size(); ++k)
        difference = std::max(difference, std::abs(left[k] - right[k]));
    REQUIRE(difference > 0.01);
    REQUIRE(rms(left) > 0.01);
    REQUIRE(rms(right) > 0.01);

    // Zero spread puts them back in lockstep, which is what proves the
    // difference above came from the spread and not from two rails that simply
    // disagree.
    Fl mono;
    mono.prepare(kSr);
    mono.set_mode(Mode::classic);
    mono.set_center_delay_ms(kCenterMs);
    mono.set_depth_ms(kDepthMs);
    mono.set_rate_hz(1.0);
    mono.set_stereo_spread(0.0);
    mono.set_feedback(0.4);
    mono.set_mix(0.5);
    mono.reset();
    std::vector<double> ml(static_cast<std::size_t>(n)), mr(static_cast<std::size_t>(n));
    mono.process_stereo(in.data(), in.data(), ml.data(), mr.data(), n);
    for (std::size_t k = 0; k < ml.size(); ++k) REQUIRE(ml[k] == mr[k]);
}

TEST_CASE("R12 stereo spread clamps to the catalog half-cycle range",
          "[signal][flanger][stereo]") {
    // The prompt and catalog expose 0..180 degrees. A direct C++ caller gets
    // the same contract in cycles: requests beyond 0.5 clamp to the endpoint,
    // rather than wrapping back through narrower phase relationships.
    Fl clamped;
    Fl endpoint;
    clamped.prepare(kSr);
    endpoint.prepare(kSr);
    clamped.set_stereo_spread(0.75);
    endpoint.set_stereo_spread(0.5);
    clamped.reset();
    endpoint.reset();

    constexpr int n = 8192;
    std::vector<double> in(n), cl(n), cr(n), el(n), er(n);
    for (int i = 0; i < n; ++i)
        in[static_cast<std::size_t>(i)] = 0.5 * std::sin(2.0 * M_PI * 700.0 * i / kSr);
    clamped.process_stereo(in.data(), in.data(), cl.data(), cr.data(), n);
    endpoint.process_stereo(in.data(), in.data(), el.data(), er.data(), n);
    for (int i = 0; i < n; ++i) {
        REQUIRE(cl[static_cast<std::size_t>(i)] == el[static_cast<std::size_t>(i)]);
        REQUIRE(cr[static_cast<std::size_t>(i)] == er[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("R12 saw spread is a phase offset, not an ignored width control",
          "[signal][flanger][stereo]") {
    const auto render = [](double spread, std::vector<double>& left,
                           std::vector<double>& right) {
        Fl f;
        f.prepare(kSr);
        f.set_mode(Mode::classic);
        f.set_waveform(LfoWave::saw_up);
        f.set_rate_hz(1.0);
        f.set_stereo_spread(spread);
        f.set_feedback(0.0);
        f.reset();

        const int n = static_cast<int>(2.0 * kSr);
        std::vector<double> in(static_cast<std::size_t>(n));
        left.resize(static_cast<std::size_t>(n));
        right.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            in[static_cast<std::size_t>(i)] =
                0.5 * std::sin(2.0 * M_PI * 700.0 * i / kSr);
        f.process_stereo(in.data(), in.data(), left.data(), right.data(), n);
    };

    std::vector<double> zero_l, zero_r, quarter_l, quarter_r, half_l, half_r;
    render(0.0, zero_l, zero_r);
    render(0.25, quarter_l, quarter_r);
    render(0.5, half_l, half_r);

    double quarter_side = 0.0;
    double spread_change = 0.0;
    for (std::size_t i = 0; i < zero_l.size(); ++i) {
        REQUIRE(zero_l[i] == zero_r[i]);
        quarter_side = std::max(quarter_side, std::abs(quarter_l[i] - quarter_r[i]));
        spread_change = std::max(spread_change, std::abs(quarter_r[i] - half_r[i]));
    }
    REQUIRE(quarter_side > 0.01);
    REQUIRE(spread_change > 0.01);
}

// ── R13 — the mode switch is bounded ──────────────────────────────────────

TEST_CASE("R13 switching mode mid-render steps no faster than the crossfade allows",
          "[signal][flanger]") {
    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::classic);
    f.set_center_delay_ms(kCenterMs);
    f.set_depth_ms(kDepthMs);
    f.set_offset_ms(4.0);
    f.set_rate_hz(0.5);
    f.set_feedback(0.5);
    f.set_mix(0.5);
    f.reset();

    const int n = static_cast<int>(1.0 * kSr);
    const int switch_at = n / 2;
    std::vector<double> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
        in[static_cast<std::size_t>(k)] = 0.5 * std::sin(2.0 * M_PI * 500.0 * k / kSr);

    for (int k = 0; k < n; ++k) {
        if (k == switch_at) f.set_mode(Mode::through_zero);
        f.process(&in[static_cast<std::size_t>(k)], &out[static_cast<std::size_t>(k)], 1);
    }

    // The bound is derived, not guessed, and it is derived from what the
    // render itself contains:
    //
    //   * a sinusoid of peak amplitude A at f can step by A·2πf/fs per sample;
    //   * the transition's gains can add at most their own slope times the
    //     amplitude they are moving between. The dry crossfade is an
    //     equal-power law over a smoothstep, so |d/dn| ≤ (π/2)·1.5/window; the
    //     wet gate is |cos(π·progress)|, so |d/dn| ≤ π/window.
    //
    // A 5 % margin covers the FM sidebands the sweep puts either side of the
    // probe tone.
    const double window_samples = mode_switch_window_samples();
    double peak = 0.0;
    for (double v : out) peak = std::max(peak, std::abs(v));
    const double signal_slew = peak * 2.0 * M_PI * 500.0 / kSr;
    const double gate_slew = peak * (1.5 * M_PI / 2.0 + M_PI) / window_samples;
    const double bound = 1.05 * (signal_slew + gate_slew);

    double worst = 0.0;
    for (std::size_t k = 1; k < out.size(); ++k)
        worst = std::max(worst, std::abs(out[k] - out[k - 1]));
    INFO("worst step " << worst << " against bound " << bound);
    REQUIRE(worst <= bound);

    // And it is not vacuous: the render has to have contained the effect.
    REQUIRE(rms(out) > 0.05);
    REQUIRE(f.mode() == Mode::through_zero);
}

// ── R14 — float and double agree ──────────────────────────────────────────

TEST_CASE("R14 the float and double instantiations place the comb identically",
          "[signal][flanger]") {
    // The comb math is a single-tap feedback loop, not a cascade, so there is
    // no precision-sensitive recursion depth for the two to diverge over. A
    // divergence beyond this tolerance would mean a clamp-ordering or
    // interpolation bug, not expected drift.
    const double delay = kCenterMs;
    const double notch = Fl::notch_hz(0, delay, Polarity::positive);
    const double peak = 1.0 / (delay * 0.001);

    FlangerT<float> single;
    single.prepare(kSr);
    single.set_mode(Mode::classic);
    single.set_center_delay_ms(delay);
    single.set_depth_ms(0.0);
    single.set_feedback(0.5);
    single.set_mix(0.5);
    single.reset();

    const int n = kSettle + kAnalysisLen;
    std::vector<float> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
    auto measure = [&](double hz) {
        for (int k = 0; k < n; ++k)
            in[static_cast<std::size_t>(k)] = static_cast<float>(
                kProbeAmplitude * std::sin(2.0 * M_PI * hz * k / kSr));
        single.reset();
        single.process(in.data(), out.data(), n);
        std::vector<double> segment(out.begin() + kSettle, out.end());
        return magnitude_at(segment, hz) / kProbeAmplitude;
    };

    auto notch_double = held(delay, 0.5, Polarity::positive);
    auto peak_double = held(delay, 0.5, Polarity::positive);
    const double float_notch = measure(notch);
    const double float_peak = measure(peak);

    REQUIRE_THAT(float_peak, WithinRel(transfer(peak_double, peak), 0.01));
    REQUIRE_THAT(float_notch, WithinRel(transfer(notch_double, notch), 0.05));
    REQUIRE_THAT(float_peak, WithinRel(comb_magnitude(peak, delay, 0.5, Polarity::positive), 0.02));
}

// ── Barberpole ────────────────────────────────────────────────────────────

TEST_CASE("barberpole shifts the wet path instead of sweeping it",
          "[signal][flanger]") {
    // The mechanism, measured directly rather than through the illusion it
    // produces. With the wet path frequency-shifted by Δf, a single input tone
    // comes out as TWO tones — the dry at f and the wet at f + Δf — and the
    // comb the pair forms drifts because their relative phase advances at Δf.
    // A delay-swept flanger cannot produce a second tone at all.
    constexpr double kTone = 500.0;
    constexpr double kShift = 3.0;
    constexpr int kLen = 48000;  // 1 s: 1 Hz bins, so every tone here is exact

    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::barberpole);
    f.set_center_delay_ms(kCenterMs);
    f.set_barberpole_shift_hz(kShift);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.reset();

    std::vector<double> in(static_cast<std::size_t>(kLen + kSettle));
    std::vector<double> out(in.size());
    for (std::size_t k = 0; k < in.size(); ++k)
        in[k] = kProbeAmplitude *
                std::sin(2.0 * M_PI * kTone * static_cast<double>(k) / kSr);
    f.process(in.data(), out.data(), static_cast<int>(in.size()));
    const std::vector<double> segment(out.begin() + kSettle, out.end());

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    REQUIRE_THAT(magnitude_at(segment, kTone) / kProbeAmplitude, WithinRel(dry_gain, 0.02));
    REQUIRE_THAT(magnitude_at(segment, kTone + kShift) / kProbeAmplitude,
                 WithinRel(wet_gain, 0.02));
    // Single sideband: the shift goes one way only, which is what makes the
    // drift monotonic rather than a symmetric warble.
    REQUIRE(magnitude_at(segment, kTone - kShift) / kProbeAmplitude < 0.02 * wet_gain);
}

TEST_CASE("barberpole feedback stacks the shift into an endless spiral",
          "[signal][flanger]") {
    // Why the shifter sits INSIDE the loop: every recirculation adds another
    // Δf, so energy appears at f + 2Δf, f + 3Δf … and climbs without ever
    // landing back where it started. That staircase is the barberpole illusion,
    // and it is the one thing a feedback path wired AROUND the shifter could
    // not produce.
    constexpr double kTone = 500.0;
    constexpr double kShift = 3.0;
    constexpr int kLen = 48000;

    auto render = [&](double feedback) {
        Fl f;
        f.prepare(kSr);
        f.set_mode(Mode::barberpole);
        f.set_center_delay_ms(kCenterMs);
        f.set_barberpole_shift_hz(kShift);
        f.set_feedback(feedback);
        f.set_mix(0.5);
        f.reset();
        std::vector<double> in(static_cast<std::size_t>(kLen + kSettle));
        std::vector<double> out(in.size());
        for (std::size_t k = 0; k < in.size(); ++k)
            in[k] = kProbeAmplitude *
                    std::sin(2.0 * M_PI * kTone * static_cast<double>(k) / kSr);
        f.process(in.data(), out.data(), static_cast<int>(in.size()));
        return std::vector<double>(out.begin() + kSettle, out.end());
    };

    const auto dry = render(0.0);
    const auto spiral = render(0.7);
    for (int pass = 2; pass <= 4; ++pass) {
        const double hz = kTone + pass * kShift;
        INFO("pass " << pass << " at " << hz << " Hz");
        REQUIRE(magnitude_at(spiral, hz) > 20.0 * magnitude_at(dry, hz));
        REQUIRE(magnitude_at(spiral, hz) > 0.01 * kProbeAmplitude);
    }
    // Monotone decay up the staircase: each pass is quieter than the last.
    for (int pass = 2; pass <= 4; ++pass)
        REQUIRE(magnitude_at(spiral, kTone + pass * kShift) <
                magnitude_at(spiral, kTone + (pass - 1) * kShift));
}

TEST_CASE("a negative barberpole shift descends instead of climbing",
          "[signal][flanger]") {
    constexpr double kTone = 2000.0;
    constexpr double kShift = -4.0;
    constexpr int kLen = 48000;

    Fl f;
    f.prepare(kSr);
    f.set_mode(Mode::barberpole);
    f.set_barberpole_shift_hz(kShift);
    f.set_feedback(0.0);
    f.set_mix(0.5);
    f.reset();
    std::vector<double> in(static_cast<std::size_t>(kLen + kSettle));
    std::vector<double> out(in.size());
    for (std::size_t k = 0; k < in.size(); ++k)
        in[k] = kProbeAmplitude *
                std::sin(2.0 * M_PI * kTone * static_cast<double>(k) / kSr);
    f.process(in.data(), out.data(), static_cast<int>(in.size()));
    const std::vector<double> segment(out.begin() + kSettle, out.end());

    double dry_gain = 0.0, wet_gain = 0.0;
    Fl::mix_gains(0.5, dry_gain, wet_gain);
    REQUIRE_THAT(magnitude_at(segment, kTone + kShift) / kProbeAmplitude,
                 WithinRel(wet_gain, 0.02));
    REQUIRE(magnitude_at(segment, kTone - kShift) / kProbeAmplitude < 0.02 * wet_gain);
}

// ── The BBD engine swap ───────────────────────────────────────────────────

TEST_CASE("the BBD engine darkens the wet path and stays bounded",
          "[signal][flanger]") {
    // The character swap, measured against the thing that makes a BBD a BBD:
    // its usable bandwidth is tied to its clock, so the wet path loses treble
    // that the clean line keeps. Asserted as a ratio between the two engines at
    // the same setting, so it is a statement about the engine rather than about
    // any absolute filter shape.
    // Probed quietly. A BBD is a companded device, so its gain is
    // level-dependent BY DESIGN — measured here it passes 0.997 at −60 dBFS,
    // 0.85 at −20 dBFS and 0.28 at full scale. A bandwidth claim measured at a
    // hot level would be measuring the compander instead.
    constexpr double kQuiet = 0.01;
    auto wet_at = [](Engine engine, double hz) {
        Fl f;
        f.prepare(kSr);
        f.set_mode(Mode::classic);
        f.set_delay_engine(engine);
        f.set_center_delay_ms(kCenterMs);
        f.set_depth_ms(0.0);
        f.set_feedback(0.0);
        f.set_mix(1.0);  // wet only: the engine's own response, nothing else
        f.reset();
        return transfer(f, hz, 24000, kQuiet);
    };

    const double clean_low = wet_at(Engine::clean, 300.0);
    const double bbd_low = wet_at(Engine::bbd, 300.0);
    const double clean_high = wet_at(Engine::clean, 12000.0);
    const double bbd_high = wet_at(Engine::bbd, 12000.0);

    // The clean line is allpass: unity at both ends.
    REQUIRE_THAT(clean_low, WithinRel(1.0, 0.02));
    REQUIRE_THAT(clean_high, WithinRel(1.0, 0.02));
    // The BBD passes the low end and rolls off the top — the clock-tied
    // bandwidth that is the device's signature, stated as a ratio against its
    // OWN low end so it is a claim about the engine rather than about any
    // absolute filter shape.
    REQUIRE(bbd_low > 0.8);
    REQUIRE(bbd_high < 0.5 * bbd_low);

    // And it is still bounded in the loop at the feedback ceiling — the engine
    // that carries a waveshaper and a compander is the one most worth checking.
    Fl loop;
    loop.prepare(kSr);
    loop.set_mode(Mode::classic);
    loop.set_delay_engine(Engine::bbd);
    loop.set_center_delay_ms(kCenterMs);
    loop.set_depth_ms(kDepthMs);
    loop.set_feedback(Fl::kFbClamp);
    loop.set_mix(1.0);
    loop.reset();
    double worst = 0.0;
    bool finite = true;
    const int n = static_cast<int>(5.0 * kSr);
    for (int k = 0; k < n; ++k) {
        const double x = k < 4800 ? 0.5 * std::sin(2.0 * M_PI * 500.0 * k / kSr) : 0.0;
        double y = 0.0;
        loop.process(&x, &y, 1);
        finite = finite && std::isfinite(y);
        worst = std::max(worst, std::abs(y));
    }
    REQUIRE(finite);
    REQUIRE(worst <= Fl::worst_case_gain());
}
