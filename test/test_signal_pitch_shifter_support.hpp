#pragma once

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



// ── A2 — warble rate ──────────────────────────────────────────────────────






// ── A3 — latency ──────────────────────────────────────────────────────────



// ── A4 — crossfade continuity ─────────────────────────────────────────────



// ── A5 — determinism ──────────────────────────────────────────────────────



// ── A6 — RT allocation ────────────────────────────────────────────────────


// ── A7 — gain ceiling ─────────────────────────────────────────────────────



// ── A8 — the pedal law ────────────────────────────────────────────────────






// ── A9 — the dive floor ───────────────────────────────────────────────────


// ── Series law 4 — the aliasing this topology really does have ────────────


// ── Composition surface (the harmony engine composes this) ────────────────
