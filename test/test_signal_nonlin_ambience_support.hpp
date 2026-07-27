#pragma once

// NonlinAmbienceT — the nonlin / gated ambience engine's acceptance suite
// (spec T1–T9), plus the structural test the spec does not have and the module
// most needs.
//
// ── What is load-bearing here ─────────────────────────────────────────────
//
// This module's entire product is that the reverb's amplitude envelope is
// DESIGNED rather than decayed. A test that only checked "there is a tail"
// would pass against an FDN and prove nothing. So the two assertions this file
// exists for are:
//
//   1. The rendered impulse response's envelope IS the requested program shape
//      — flat-then-cut for Gated, rising-then-cut for Reverse, a short
//      exponential for Ambience, `n_b` humps for NonLin2 — measured from the
//      render and compared against an INDEPENDENT transcription of the spec's
//      §4.3 envelope math (this file's `spec_envelope`, which shares no code
//      with the module's `program_envelope`).
//   2. The wet path really has no recursion: with the diffuser bypassed the
//      impulse response is BIT-EXACTLY zero past its last tap. A recursive tank
//      cannot produce that sample, which is what makes the shapes in (1)
//      reachable at all.
//
// Series law 1 (bound the loop gain of any gain-carrying nonlinearity inside a
// feedback path) is **not applicable by construction**: there is no feedback in
// the wet path and no nonlinearity in the default path at all. The worst-case
// gain is therefore not measured-and-hoped but a closed form of the shipped
// constants, `Π_i(1+2·g_i)·G_L1`, and N11 renders the response and asserts the
// actual `Σ|h[n]|` stays under it (series law 8).
//
// ── Measurement recipe ────────────────────────────────────────────────────
//
// **Rectangular windows, not Hann.** The spec's T1 asks for 5 ms Hann. A
// rectangular window makes the short-time power estimate an unweighted sum of
// the tap energies inside it, which is what lets the expected value below be
// written in closed form and the residual be attributed. A Hann window would
// put a data-dependent weight on each tap and turn every tolerance into a
// guess. Same quantity, no window correction hiding inside the tolerance.
//
// **20 ms windows, not 5 ms.** At the shipped `Nd_min` a 5 ms window holds four
// taps. 20 ms holds sixteen at the sparsest point and ~80 at the densest.
//
// **The estimator is far quieter than a Poisson process would be, and that is
// why the tolerances below can be tight.** Velvet noise places exactly one
// pulse per grid interval and jitters it inside that interval — it is a
// stratified (low-discrepancy) process, not a Poisson one, so the number of
// taps in a window is `N/Td ± 1` rather than Poisson-distributed. The measured
// worst-case deviation from the closed-form prediction across all four programs
// is 0.23 dB (naked cloud) and 0.64 dB (full default path); the tolerances used
// below are set from those numbers with margin, not from a hand-wave.
//
// **Two configurations are measured, deliberately.**
//   * The "naked shape" configuration — `set_diffusion(0)` and
//     `set_hf_damp_hz(kFcBright)` — removes the two things that are *not* the
//     envelope: the diffuser's ~118 ms ring and the segment tilt's frequency-
//     dependent power loss. What is left is the designed envelope and nothing
//     else, so N2 compares it to `spec_envelope` directly and tightly.
//   * The shipped default configuration is measured in N3 against the FULL
//     model — tap gains squared, weighted by each segment filter's white-noise
//     power gain `(1−a)/(1+a)`, convolved with the allpass chain's own energy
//     impulse response. That model is exact in expectation because the tap
//     signs are independent, so the cross terms vanish.
//
// ── Five spec defects, each proven with numbers ──────────────────────────
//
// Every one of these was found by computing the criterion's own arithmetic
// before changing any code, and each is asserted as arithmetic in N17 so that a
// future change to a shipped constant re-derives it rather than inheriting this
// paragraph. The MODULE is not bent to fit them; the TESTS are re-scoped, with
// the reason.
//
// **D1 — T1's ±1.0 dB Gated flatness cannot hold at the shipped defaults,
// because §4.5's own segment tilt breaks it.** The tilt gives segment `s` a
// one-pole at `fc(s)`, and a one-pole's white-noise power gain is
// `(1−a)/(1+a)`: −1.08 dB at `kFcBright` and −4.27 dB at `kFcDark`. So a
// broadband short-time RMS envelope falls 3.19 dB across the window for
// spectral reasons alone, and the Gated body (τ ≤ h = 0.70, six of eight
// segments) droops a measured 2.4 dB. The criterion is about the ENVELOPE, so
// N2 asserts it with the tilt neutralized, where the measured flatness is
// 0.23 dB — comfortably inside the spec's ±1.0 dB.
//
// **D2 — T1's "≤ −60 dB for τ > h + 2w" is unreachable, by a factor of 2.6, and
// §3 is what makes it so.** The pre-diffusion allpasses are recursive and ring
// at `g` per `M` samples. Reaching −60 dB takes `ln(10⁻³)/ln(g) = 19.4`
// repetitions of the longer delay: 19.4 × 293 = 5675 samples = 118 ms. T1
// allows `2w·L`, which at its own stated `length_ms = 400` is 1920 samples =
// 40 ms. Measured: −60 dB at 105 ms after the cut. N4 asserts the achievable
// criterion — below −60 dB within the allpass chain's own computed 60 dB time —
// and N5 asserts the exact-zero version with the diffuser bypassed.
//
// **D3 — T2's "NED ≥ 0.9" is unreachable by construction.** Normalized echo
// density counts samples exceeding the local RMS, normalised by `erfc(1/√2) =
// 0.3173`. A sparse cloud's only samples above its own RMS are its pulses, so
// `NED ≤ (Nd/fs)/0.3173`. At the shipped `kNdMax = 4000` and 48 kHz that ceiling
// is **0.2626** — NED reaches 1 only when the response has Gaussian sample
// statistics, which needs `0.3173·fs = 15230` pulses/s, 3.8× the shipped
// maximum. Being sparse is the entire point of velvet noise, so the fix is not
// to raise the density. N6 instead asserts what T2 actually protects: that echo
// density GROWS under `γ = 2`, is FLAT under `γ = 0`, and tracks the shipped
// `Nd(u)` law.
//
// **D4 — T7's `worst_case_gain` omits the output DC blocker the spec's own §2
// diagram mandates.** A DC blocker has a time-domain L1 gain of exactly 2 for
// any pole. Placing one in the default output path would double the shipped
// bound from 23.04 to 46.08 AND destroy the finite-IR property this module is
// built around. The module ships it inside the converter stage instead (see the
// header). N11 asserts the bound in both configurations.
//
// **D5 — §4.3's tap-gain law and §7 T1's measurement contradict each other, and
// the module follows the measurement.** `g_k = sign·E(τ_k)·norm` with a
// time-varying density makes the short-time RMS `E(τ)·sqrt(Nd(τ))`, not `E(τ)`:
// at the shipped `γ = 2` that is +7.0 dB of error across an Ambience tail and
// +4.7 dB across a Gated body. The module weights each tap by `sqrt(Td(τ_k))`,
// which makes the RMS exactly `E(τ)` and leaves the §4.4 L1 budget — and
// therefore `worst_case_gain` — untouched. N1 asserts the shipped law including
// the weight; N17 asserts the arithmetic that forced it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/nonlin_ambience.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <vector>

using pulp::signal::NonlinAmbience;
using pulp::signal::NonlinProgram;
namespace na = pulp::signal::nonlin_ambience;

namespace {

constexpr double kFs = 48000.0;

/// `erfc(1/√2)` — the fraction of a Gaussian outside ±1σ, and the normalising
/// constant in the echo-density measure. Written out rather than called so the
/// test does not depend on a libm edge case.
constexpr double kErfcHalfRoot2 = 0.31731050786291410;

constexpr double kPi = 3.14159265358979323846;

// ── Independent transcription of the spec's math ─────────────────────────
//
// These share NO code with the module. They are the spec's §3 and §4.3 written
// out again from the document, which is the only way an assertion about "the
// envelope is what was asked for" means anything.

/// Spec §4.3, transcribed. `gate_hold` is Gated's `h`; `attack` is Reverse's
/// `r` and NonLin2's `h`.
double spec_envelope(NonlinProgram program, double tau, double gate_hold, double attack) {
    if (tau < 0.0 || tau > 1.0) return 0.0;
    const double fade_in = std::min(1.0, tau / na::kFadeInFrac);
    switch (program) {
        case NonlinProgram::ambience: {
            const double alpha = na::kAmbDropDb * std::log(10.0) / 20.0;
            return fade_in * std::exp(-alpha * tau);
        }
        case NonlinProgram::gated:
            if (tau <= gate_hold) return 1.0;
            if (tau <= gate_hold + na::kGateFall)
                return 1.0 - (tau - gate_hold) / na::kGateFall;
            return 0.0;
        case NonlinProgram::reverse:
            if (tau <= attack) return std::pow(tau / attack, na::kRevPow);
            return 1.0;
        case NonlinProgram::nonlin2: {
            if (tau > attack) return 0.0;
            const double ripple = 0.5 - 0.5 * std::cos(2.0 * kPi * na::kNlHumps * tau);
            return fade_in * ((1.0 - na::kNlDepth) + na::kNlDepth * ripple);
        }
    }
    return 0.0;
}

/// Spec §4.2, transcribed: `Nd(u) = Nd_min + (Nd_max − Nd_min)·u^γ`, with
/// `Nd_min` scaled by `density_pct` about its reference.
double spec_density(double u, double density_pct, double gamma) {
    const double nd_min = na::kNdMin * (density_pct / na::kDensityRefPct);
    return nd_min + (na::kNdMax - nd_min) * std::pow(std::clamp(u, 0.0, 1.0), gamma);
}

/// Spec §3's single-multiplier allpass difference equation, transcribed, run as
/// a two-stage chain and returned as its ENERGY impulse response `h[n]²`. Used
/// as the smearing kernel of the expectation model in N3.
std::vector<double> spec_allpass_energy_ir(double g, int m0, int m1, int length) {
    std::vector<double> b0(static_cast<std::size_t>(m0), 0.0);
    std::vector<double> b1(static_cast<std::size_t>(m1), 0.0);
    std::vector<double> out(static_cast<std::size_t>(length), 0.0);
    int w0 = 0, w1 = 0;
    for (int n = 0; n < length; ++n) {
        double x = n == 0 ? 1.0 : 0.0;
        double d = b0[static_cast<std::size_t>(w0)];
        double w = x + g * d;
        double y = -g * w + d;
        b0[static_cast<std::size_t>(w0)] = w;
        w0 = (w0 + 1) % m0;

        d = b1[static_cast<std::size_t>(w1)];
        w = y + g * d;
        y = -g * w + d;
        b1[static_cast<std::size_t>(w1)] = w;
        w1 = (w1 + 1) % m1;

        out[static_cast<std::size_t>(n)] = y * y;
    }
    return out;
}

/// The white-noise power gain of the one-pole `y = (1−a)x + a·y[n−1]`, which is
/// `Σh² = (1−a)²/(1−a²) = (1−a)/(1+a)`. This is the term the spec's T1 forgets
/// (defect D1).
double onepole_noise_power(double pole) { return (1.0 - pole) / (1.0 + pole); }

/// Spec §4.5's segment pole, transcribed.
double spec_segment_pole(int segment, double tone, double hf_damp_hz) {
    const double q = std::clamp(
        static_cast<double>(segment) / static_cast<double>(na::kSegments - 1) - 0.5 * tone,
        0.0, 1.0);
    const double fc = na::kFcBright * std::pow(hf_damp_hz / na::kFcBright, q);
    return std::exp(-2.0 * kPi * fc / kFs);
}

// ── Rendering helpers ────────────────────────────────────────────────────

struct Stereo {
    std::vector<float> left, right;
};

Stereo render_impulse(NonlinAmbience& engine, int length) {
    Stereo out{std::vector<float>(static_cast<std::size_t>(length), 0.0f),
               std::vector<float>(static_cast<std::size_t>(length), 0.0f)};
    out.left[0] = 1.0f;
    out.right[0] = 1.0f;
    engine.process(out.left.data(), out.right.data(), length);
    return out;
}

/// Mean power over a rectangular window.
double window_power(const std::vector<float>& x, int start, int length) {
    double sum = 0.0;
    for (int i = start; i < start + length; ++i) {
        const double v = x[static_cast<std::size_t>(i)];
        sum += v * v;
    }
    return sum / static_cast<double>(length);
}

double to_db(double power) { return 10.0 * std::log10(std::max(power, 1e-300)); }

/// The constant group delay the two bypassed allpasses contribute at
/// `diffusion = 0`, where each stage degenerates to a pure `M`-sample delay.
int bypassed_diffuser_delay(const NonlinAmbience& engine) {
    return engine.allpass_length(0) + engine.allpass_length(1);
}

int last_tap_delay(const NonlinAmbience& engine) {
    int last = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int k = 0; k < engine.tap_count(ch); ++k)
            last = std::max(last, engine.tap(ch, k).delay);
    return last;
}

/// The samples a segment one-pole needs to fall from unity to the denormal snap
/// threshold, computed from the darkest shipped corner. This is what bounds how
/// long after the last tap the output can still be non-zero.
int segment_flush_samples(double hf_damp_hz) {
    const double pole = std::exp(-2.0 * kPi * hf_damp_hz / kFs);
    return static_cast<int>(std::ceil(std::log(1e-15) / std::log(pole))) + 1;
}

/// A deterministic, reproducible broadband stimulus. Not white — a one-pole
/// tilt, so the segment filters have something to act on at both ends.
std::vector<float> pink_ish(int length, std::uint32_t seed) {
    pulp::signal::Xorshift32 rng(seed);
    std::vector<float> out(static_cast<std::size_t>(length));
    float state = 0.0f;
    for (int n = 0; n < length; ++n) {
        const float white = rng.next_bipolar<float>();
        state = 0.85f * state + 0.15f * white;
        out[static_cast<std::size_t>(n)] = 0.5f * (white + 3.0f * state);
    }
    return out;
}

/// A fresh engine in the "naked shape" configuration: no diffuser ring and no
/// spectral tilt, so a short-time RMS envelope is the designed envelope and
/// nothing else.
void configure_naked(NonlinAmbience& engine, NonlinProgram program, double length_ms) {
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(program);
    engine.set_length_ms(length_ms);
    engine.set_diffusion(0.0);
    engine.set_hf_damp_hz(na::kFcBright);
    engine.reset();  // resolves the program-swap crossfade before measuring
}

const NonlinProgram kAllPrograms[] = {NonlinProgram::ambience, NonlinProgram::gated,
                                      NonlinProgram::reverse, NonlinProgram::nonlin2};

const char* program_name(NonlinProgram p) {
    switch (p) {
        case NonlinProgram::ambience: return "ambience";
        case NonlinProgram::gated: return "gated";
        case NonlinProgram::reverse: return "reverse";
        case NonlinProgram::nonlin2: return "nonlin2";
    }
    return "?";
}

}  // namespace

// ── N1 / spec T1 (exact form) ─────────────────────────────────────────────



// ── N2 / spec T1 (rendered, naked) ────────────────────────────────────────


// ── N3 / spec T1 (rendered, shipped defaults) ─────────────────────────────


// ── N4 / spec T1 (the four program shapes, qualitatively) ─────────────────


// ── N5 — the structural test: no recursion in the wet path ────────────────



// ── N6 / spec T2 — echo density ───────────────────────────────────────────


// ── N7 / spec T3 — the tap-count law ──────────────────────────────────────


// ── N8 / spec T4 — determinism (series law 2) ─────────────────────────────



// ── N9 / spec T5 — stereo ─────────────────────────────────────────────────



// ── N10 / spec T6 — latency (series law 5) ────────────────────────────────


// ── N11 / spec T7 — the worst-case gain is a tested bound (series law 8) ──



// ── N12 / spec T8 — RT allocation probe ───────────────────────────────────


// ── N13 / spec T9 — denormal safety ───────────────────────────────────────


// ── N14 — the program swap ────────────────────────────────────────────────


// ── N15 — the segment tilt ────────────────────────────────────────────────



// ── N16 — the optional converter character ────────────────────────────────


// ── N17 — the spec's own arithmetic, asserted rather than described ───────


// ── Housekeeping ──────────────────────────────────────────────────────────
