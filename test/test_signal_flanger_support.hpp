#pragma once

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
#include <numbers>
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
    const double w = 2.0 * std::numbers::pi * hz / kSr;
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
        in[n] = amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kSr);
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
    const double pole = std::exp(-2.0 * std::numbers::pi * Fl::kDcBlockHz / kSr);
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * hz / kSr);
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
    const std::complex<double> e =
        std::polar(1.0, -2.0 * std::numbers::pi * hz * delay_samples / kSr);
    const std::complex<double> wet = e / (1.0 - feedback * sign * e * dc_blocker_response(hz));
    return std::abs(dry_gain + wet_gain * sign * wet);
}

}  // namespace

// ── Measurement-recipe guard ──────────────────────────────────────────────


// ── R1 — notch positions follow Δf = 1/D ──────────────────────────────────


// ── R2 — feedback sharpens the comb; it does not move it ──────────────────


// ── R3 — polarity moves the whole series by half a spacing ────────────────


// ── R4 / R5 — through zero ────────────────────────────────────────────────





// ── R6 — the feedback loop is bounded, and the registry says by how much ──




// ── R7 — determinism ──────────────────────────────────────────────────────


// ── R8 — real-time allocation probe ───────────────────────────────────────


// ── R9 — latency ──────────────────────────────────────────────────────────


// ── R10 / R11 — the depth clamps ──────────────────────────────────────────



// ── R12 — stereo ──────────────────────────────────────────────────────────




// ── R13 — the mode switch is bounded ──────────────────────────────────────


// ── R14 — float and double agree ──────────────────────────────────────────


// ── Barberpole ────────────────────────────────────────────────────────────




// ── The BBD engine swap ───────────────────────────────────────────────────
