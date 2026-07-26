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

TEST_CASE("Nonlin ambience: every tap gain traces the designed envelope exactly",
          "[signal][nonlin-ambience][envelope]") {
    // The envelope IS the tap-gain sequence, so this is the claim in its
    // zero-variance form: no windows, no estimator, no tolerance beyond float
    // round-off. `spec_envelope` is an independent transcription of §4.3.
    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(400.0);
        engine.reset();

        INFO("program = " << program_name(program));
        REQUIRE(engine.tap_count(0) > 0);

        const double window = engine.window_samples();
        const double gate_hold = na::kGateHold;
        const double attack = na::kRevRise;

        for (int ch = 0; ch < 2; ++ch) {
            double l1 = 0.0;
            for (int k = 0; k < engine.tap_count(ch); ++k) {
                const auto& tap = engine.tap(ch, k);
                const double tau =
                    (tap.delay - engine.predelay_samples()) / window;
                const double env = spec_envelope(program, tau, gate_hold, attack);

                // The shipped gain law, including the sqrt(Td) density weight
                // that defect D5 forced. Td is the grid spacing at this tap's
                // own time.
                const double grid = kFs / spec_density(tau, na::kDensityRefPct,
                                                       na::kGammaDefault);
                const double expected = env * std::sqrt(grid) * engine.tap_norm(ch);

                INFO("channel " << ch << " tap " << k << " tau " << tau);
                REQUIRE_THAT(std::fabs(static_cast<double>(tap.gain)),
                             Catch::Matchers::WithinRel(expected, 2e-3));
                // Ternary structure: every tap is ±1 times the envelope, never
                // a third magnitude.
                REQUIRE(tap.gain != 0.0f);
                l1 += std::fabs(static_cast<double>(tap.gain));
            }
            // §4.4: the L1 budget is met exactly, which is what makes the peak
            // gain a closed form rather than a measurement.
            INFO("channel " << ch);
            REQUIRE_THAT(l1, Catch::Matchers::WithinRel(na::kL1Budget, 1e-4));
        }
    }
}

TEST_CASE("Nonlin ambience: taps carry both signs and land inside the window",
          "[signal][nonlin-ambience][envelope]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_predelay_ms(25.0);
    engine.reset();

    const int predelay = engine.predelay_samples();
    const int window = engine.window_samples();
    REQUIRE(predelay > 0);

    int positives = 0, negatives = 0, previous = -1;
    for (int k = 0; k < engine.tap_count(0); ++k) {
        const auto& tap = engine.tap(0, k);
        REQUIRE(tap.delay >= predelay);
        REQUIRE(tap.delay < predelay + window);
        REQUIRE(tap.segment >= 0);
        REQUIRE(tap.segment < na::kSegments);
        // Velvet noise is a stratified process: exactly one pulse per grid
        // interval, so positions are strictly increasing and never collide.
        REQUIRE(tap.delay > previous);
        previous = tap.delay;
        (tap.gain > 0.0f ? positives : negatives)++;
    }
    // A ±1 coin over ~750 taps: a 30/70 split would be a 10σ event, so this
    // catches a stuck sign bit without being flaky.
    REQUIRE(positives > engine.tap_count(0) * 3 / 10);
    REQUIRE(negatives > engine.tap_count(0) * 3 / 10);
}

// ── N2 / spec T1 (rendered, naked) ────────────────────────────────────────

TEST_CASE("Nonlin ambience: the rendered envelope is the designed envelope",
          "[signal][nonlin-ambience][envelope]") {
    // The headline claim. Measured on the naked cloud so that what is compared
    // is the envelope and not the diffuser's ring or the segment tilt's
    // colour — both of which are separately asserted (N4, N15).
    //
    // Tolerance: the measured worst-case deviation across all four programs is
    // 0.23 dB; 0.75 dB is that with 3x margin and is still far inside the
    // spec's own ±1.0 dB flatness ask.
    constexpr double kEnvelopeTolDb = 0.75;
    constexpr double kFloorDb = -70.0;

    const int win = static_cast<int>(0.020 * kFs);
    const int hop = static_cast<int>(0.010 * kFs);

    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        configure_naked(engine, program, 400.0);

        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 4000);

        const double pole = spec_segment_pole(0, 0.0, na::kFcBright);
        const double tilt_power = onepole_noise_power(pole);
        const double norm = engine.tap_norm(0);

        int compared = 0;
        double worst = 0.0;
        for (int start = lag; start + win <= static_cast<int>(ir.left.size());
             start += hop) {
            const double tau =
                (start + win * 0.5 - lag) / static_cast<double>(window);
            if (tau > 1.0) break;

            // Expected mean power over the window: the mean of E(τ)² across the
            // window's samples, scaled by the L1 normalisation and the (now
            // uniform) segment filter's noise gain. Every term is computed from
            // shipped constants.
            double sum_e2 = 0.0;
            for (int i = start; i < start + win; ++i) {
                const double t = (i + 0.5 - lag) / static_cast<double>(window);
                const double e = spec_envelope(program, t, na::kGateHold, na::kRevRise);
                sum_e2 += e * e;
            }
            const double expected =
                sum_e2 / win * norm * norm * tilt_power;
            if (to_db(expected) < kFloorDb) continue;

            const double measured = window_power(ir.left, start, win);
            const double deviation = to_db(measured) - to_db(expected);
            worst = std::max(worst, std::fabs(deviation));
            INFO("program " << program_name(program) << " tau " << tau
                            << " measured " << to_db(measured) << " dB expected "
                            << to_db(expected) << " dB");
            REQUIRE(std::fabs(deviation) < kEnvelopeTolDb);
            ++compared;
        }
        INFO("program " << program_name(program) << " worst deviation " << worst);
        REQUIRE(compared > 15);
    }
}

// ── N3 / spec T1 (rendered, shipped defaults) ─────────────────────────────

TEST_CASE("Nonlin ambience: the shipped default render matches the full model",
          "[signal][nonlin-ambience][envelope]") {
    // N2 measures the envelope with the diffuser and the tilt taken out. This
    // one measures the SHIPPED configuration and accounts for both, so that
    // nothing about the default path is left unmodelled.
    //
    // Expected power at sample n is exact in expectation because the tap signs
    // are independent, so all cross terms vanish:
    //     E[h(n)²] = Σ_k g_k² · G²(seg_k) · e_ap(n − d_k)
    // with G² the segment one-pole's white-noise power gain and e_ap the
    // allpass chain's own energy impulse response.
    //
    // Tolerance: measured worst case is 0.64 dB; 1.5 dB is that with margin.
    constexpr double kModelTolDb = 1.5;
    constexpr double kFloorDb = -80.0;

    const int win = static_cast<int>(0.020 * kFs);
    const int hop = static_cast<int>(0.010 * kFs);

    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(400.0);
        engine.reset();

        const int window = engine.window_samples();
        const int length = window + 8000;
        const Stereo ir = render_impulse(engine, length);

        double tilt_power[na::kSegments];
        for (int s = 0; s < na::kSegments; ++s)
            tilt_power[s] = onepole_noise_power(spec_segment_pole(s, 0.0, na::kFcDark));

        const auto energy_ir = spec_allpass_energy_ir(
            na::kDiffusionDefault, engine.allpass_length(0), engine.allpass_length(1),
            12000);

        std::vector<double> expected(static_cast<std::size_t>(length), 0.0);
        for (int k = 0; k < engine.tap_count(0); ++k) {
            const auto& tap = engine.tap(0, k);
            const double energy = static_cast<double>(tap.gain) * tap.gain *
                                  tilt_power[tap.segment];
            const int span = std::min(static_cast<int>(energy_ir.size()),
                                      length - tap.delay);
            for (int i = 0; i < span; ++i)
                expected[static_cast<std::size_t>(tap.delay + i)] +=
                    energy * energy_ir[static_cast<std::size_t>(i)];
        }

        int compared = 0;
        for (int start = 0; start + win <= length; start += hop) {
            double model = 0.0;
            for (int i = start; i < start + win; ++i)
                model += expected[static_cast<std::size_t>(i)];
            model /= win;
            if (to_db(model) < kFloorDb) continue;

            const double measured = window_power(ir.left, start, win);
            INFO("program " << program_name(program) << " window at " << start
                            << " measured " << to_db(measured) << " dB model "
                            << to_db(model) << " dB");
            REQUIRE(std::fabs(to_db(measured) - to_db(model)) < kModelTolDb);
            ++compared;
        }
        REQUIRE(compared > 15);
    }
}

// ── N4 / spec T1 (the four program shapes, qualitatively) ─────────────────

TEST_CASE("Nonlin ambience: each program has the shape a decaying tank cannot make",
          "[signal][nonlin-ambience][envelope]") {
    const int win = static_cast<int>(0.020 * kFs);
    const int hop = static_cast<int>(0.005 * kFs);

    auto envelope_db = [&](NonlinAmbience& engine, const Stereo& ir, int lag,
                           std::vector<double>& taus) {
        std::vector<double> out;
        const double window = engine.window_samples();
        for (int start = lag; start + win <= static_cast<int>(ir.left.size());
             start += hop) {
            out.push_back(to_db(window_power(ir.left, start, win)));
            taus.push_back((start + win * 0.5 - lag) / window);
        }
        return out;
    };

    SECTION("gated: the body is flat and the cut is a cut") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::gated, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        // Body flat within the spec's own ±1.0 dB, once D1's spectral tilt is
        // out of the way. Windows are excluded near the fade-in and near the
        // cut, where a 20 ms window straddles a transition by construction.
        const double body_start = na::kFadeInFrac + 0.05;
        const double body_end = na::kGateHold - 0.05;
        double lo = 1e9, hi = -1e9;
        for (std::size_t i = 0; i < env.size(); ++i)
            if (taus[i] > body_start && taus[i] < body_end) {
                lo = std::min(lo, env[i]);
                hi = std::max(hi, env[i]);
            }
        INFO("gated body spread " << (hi - lo) << " dB");
        REQUIRE(hi - lo < 2.0);  // ±1.0 dB, spec T1

        // And after the cut, silence — exactly, not asymptotically. This is the
        // structural claim; N5 generalises it.
        const int cut = lag + static_cast<int>((na::kGateHold + na::kGateFall) *
                                               engine.window_samples());
        const int flush = segment_flush_samples(na::kFcBright);
        for (int i = cut + flush + last_tap_delay(engine) - engine.window_samples();
             i < static_cast<int>(ir.left.size()); ++i) {
            if (i < cut + flush) continue;
            INFO("sample " << i << " of " << ir.left.size());
            REQUIRE(ir.left[static_cast<std::size_t>(i)] == 0.0f);
            break;  // the exhaustive version is N5
        }
    }

    SECTION("gated at the shipped diffusion: the cut is bounded by the allpass ring") {
        // Defect D2: T1 wants −60 dB within 2w·L = 40 ms. The mandated diffuser
        // rings for ln(10^-3)/ln(g) repetitions of its longest delay. That
        // number, not 2w·L, is the achievable criterion.
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(NonlinProgram::gated);
        engine.set_length_ms(400.0);
        engine.reset();

        const int window = engine.window_samples();
        const Stereo ir = render_impulse(engine, window + 20000);

        double body = 0.0;
        int count = 0;
        for (int start = 0; start + win < static_cast<int>(0.6 * window);
             start += hop, ++count)
            body += window_power(ir.left, start, win);
        body /= count;

        const double repetitions = std::log(1e-3) / std::log(na::kDiffusionDefault);
        const int allpass_60db = static_cast<int>(
            std::ceil(repetitions * std::max(engine.allpass_length(0),
                                             engine.allpass_length(1))));
        const int cut = static_cast<int>((na::kGateHold + na::kGateFall) * window);

        INFO("allpass 60 dB time = " << allpass_60db << " samples ("
                                     << allpass_60db * 1000.0 / kFs << " ms); spec T1 allows "
                                     << 2.0 * na::kGateFall * window << " samples");
        REQUIRE(to_db(window_power(ir.left, cut + allpass_60db, win) / body) < -60.0);
        // ...and it is genuinely still ringing at the point T1 asks for silence,
        // which is the defect, asserted rather than described.
        REQUIRE(to_db(window_power(ir.left,
                                   cut + static_cast<int>(2.0 * na::kGateFall * window),
                                   win) /
                      body) > -60.0);
    }

    SECTION("reverse: the envelope rises, then cuts") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::reverse, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        // Monotone rising up to the plateau. This is the assertion no FDN can
        // pass: a decaying tank's envelope is falling by construction.
        double previous = -1e9;
        int checked = 0;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (taus[i] < 0.10 || taus[i] > na::kRevRise - 0.05) continue;
            INFO("tau " << taus[i] << " level " << env[i] << " previous " << previous);
            REQUIRE(env[i] > previous - 0.5);  // spec T1's ≥ previous − 0.5 dB
            previous = env[i];
            ++checked;
        }
        REQUIRE(checked > 10);

        // Peak is at the plateau, not at the start.
        std::size_t peak = 0;
        for (std::size_t i = 0; i < env.size(); ++i)
            if (taus[i] <= 1.0 && env[i] > env[peak]) peak = i;
        INFO("peak at tau " << taus[peak]);
        REQUIRE(taus[peak] > na::kRevRise - 0.1);
        REQUIRE(taus[peak] <= 1.0);
    }

    SECTION("ambience: monotone falling at the designed rate") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::ambience, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        double previous = 1e9;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (taus[i] < 0.10 || taus[i] > 0.95) continue;
            INFO("tau " << taus[i]);
            REQUIRE(env[i] < previous + 0.5);
            previous = env[i];
        }

        // The total drop is the shipped kAmbDropDb, in amplitude dB. Measured on
        // power, so the criterion is 2x the amplitude figure... which is exactly
        // what `to_db` on a power quantity already reports as amplitude dB.
        double at_low = 0.0, at_high = 0.0;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (std::fabs(taus[i] - na::kFadeInFrac * 2.0) < 0.02) at_low = env[i];
            if (std::fabs(taus[i] - 0.98) < 0.02) at_high = env[i];
        }
        const double expected_drop =
            20.0 * std::log10(spec_envelope(NonlinProgram::ambience, 0.98,
                                            na::kGateHold, na::kRevRise) /
                              spec_envelope(NonlinProgram::ambience,
                                            na::kFadeInFrac * 2.0, na::kGateHold,
                                            na::kRevRise));
        INFO("measured drop " << (at_high - at_low) << " dB, designed "
                              << expected_drop << " dB");
        REQUIRE(std::fabs((at_high - at_low) - expected_drop) < 2.0);
    }

    SECTION("nonlin2: exactly kNlHumps humps, then a gate") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::nonlin2, 400.0);
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, engine.window_samples() + lag + 4000);
        std::vector<double> taus;
        const auto env = envelope_db(engine, ir, lag, taus);

        // Humps are counted by PROMINENCE, not by raw local maxima. Counting
        // raw maxima is the wrong instrument and was measurably so: it reported
        // three, because the broad peak at τ = 0.25 is flat enough that 0.12 dB
        // of estimator noise splits it into two adjacent maxima 0.05 τ apart.
        // Prominence — how far a peak stands above the higher of the two valleys
        // that flank it — is what "a hump" means. The designed ripple is
        // 20·log10(1/(1−kNlDepth)) = 4.44 dB deep, so a threshold at a quarter
        // of that separates a real hump (measured prominence ≈ 4.3 dB) from the
        // spurious one (0.016 dB) by a factor of seventy.
        const double ripple_db = 20.0 * std::log10(1.0 / (1.0 - na::kNlDepth));
        const double min_prominence = 0.25 * ripple_db;

        // Candidates are restricted to the body; the VALLEY SEARCH is not. That
        // distinction is load-bearing: clipping the search at the body edge cut
        // the second hump's right flank off before it descended into the gate
        // and dropped its prominence to 0.82 dB, which read as one hump instead
        // of two. Prominence is a property of the whole curve.
        auto in_body = [&](std::size_t i) {
            return taus[i] >= na::kFadeInFrac * 2.0 && taus[i] <= na::kNlHold - 0.02;
        };
        std::size_t body_count = 0;
        for (std::size_t i = 0; i < env.size(); ++i) body_count += in_body(i) ? 1 : 0;
        REQUIRE(body_count > 20);

        int humps = 0;
        for (std::size_t i = 1; i + 1 < env.size(); ++i) {
            if (!in_body(i)) continue;
            const double level = env[i];
            if (!(level > env[i - 1] && level >= env[i + 1])) continue;
            // Walk out to the first higher point on each side; the deepest
            // valley reached on the way is that side's key col.
            double left_valley = level, right_valley = level;
            for (std::size_t j = i; j-- > 0;) {
                if (env[j] > level) break;
                left_valley = std::min(left_valley, env[j]);
            }
            for (std::size_t j = i + 1; j < env.size(); ++j) {
                if (env[j] > level) break;
                right_valley = std::min(right_valley, env[j]);
            }
            const double prominence = level - std::max(left_valley, right_valley);
            if (prominence >= min_prominence) {
                INFO("hump at tau " << taus[i] << " prominence " << prominence << " dB");
                ++humps;
            }
        }
        INFO("counted " << humps << " humps against a designed " << na::kNlHumps
                        << " (ripple depth " << ripple_db << " dB)");
        REQUIRE(humps == na::kNlHumps);

        // The ripple is genuinely as deep as designed — a flat body would pass
        // a hump count of zero-versus-two only by accident.
        double peak = -1e9, trough = 1e9;
        for (std::size_t i = 0; i < env.size(); ++i) {
            if (!in_body(i)) continue;
            peak = std::max(peak, env[i]);
            trough = std::min(trough, env[i]);
        }
        INFO("measured ripple " << (peak - trough) << " dB, designed " << ripple_db);
        REQUIRE(std::fabs((peak - trough) - ripple_db) < 1.0);
    }
}

// ── N5 — the structural test: no recursion in the wet path ────────────────

TEST_CASE("Nonlin ambience: the wet impulse response is finite",
          "[signal][nonlin-ambience][structure]") {
    // The claim that separates this module from every recursive reverb. With
    // the diffuser bypassed, each allpass degenerates to a pure M-sample delay
    // and the whole wet path is an FIR — so past the last tap (plus the segment
    // filters' own flush, which is computed, not guessed) the output is
    // BIT-EXACTLY zero. No feedback design can produce that sample: a decaying
    // tank's output is asymptotic to zero, never equal to it.
    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        configure_naked(engine, program, 400.0);

        const int lag = bypassed_diffuser_delay(engine);
        const int last = last_tap_delay(engine);
        const int flush = segment_flush_samples(na::kFcBright);
        const int length = lag + last + flush + 8000;
        const Stereo ir = render_impulse(engine, length);

        // Something happened first — otherwise "all zero" would pass trivially.
        double peak = 0.0;
        for (float v : ir.left) peak = std::max(peak, static_cast<double>(std::fabs(v)));
        INFO("program " << program_name(program));
        REQUIRE(peak > 1e-4);

        // Scanned rather than asserted per sample: one assertion carrying the
        // first offending index reads better than eight thousand identical
        // passes hiding one failure.
        const int tail_start = lag + last + flush;
        int offender = -1;
        double offending_value = 0.0;
        for (int n = tail_start; n < length && offender < 0; ++n) {
            const float l = ir.left[static_cast<std::size_t>(n)];
            const float r = ir.right[static_cast<std::size_t>(n)];
            if (l != 0.0f || r != 0.0f) {
                offender = n;
                offending_value = std::fabs(l) > std::fabs(r) ? l : r;
            }
        }
        INFO("first non-zero tail sample " << offender << " (value " << offending_value
                                           << ") after last tap + lag + flush = "
                                           << tail_start << ", of " << length);
        REQUIRE(offender == -1);
    }
}

TEST_CASE("Nonlin ambience: the only recursion is the two bounded allpasses",
          "[signal][nonlin-ambience][structure]") {
    // At the shipped diffusion the response is no longer finite — the diffuser
    // is recursive, which the spec acknowledges. What is asserted is that the
    // ring is bounded by the allpass's own closed form and nothing else: the
    // tail is below −100 dB by the time the coefficient's 100 dB point says it
    // should be, and it does eventually reach exact zero because every
    // recursive state snaps through the denormal threshold.
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(NonlinProgram::gated);
    engine.set_length_ms(200.0);
    engine.reset();

    const double repetitions = std::log(1e-5) / std::log(na::kDiffusionDefault);
    const int allpass_100db = static_cast<int>(std::ceil(
        repetitions * (engine.allpass_length(0) + engine.allpass_length(1))));
    const int length = engine.window_samples() + allpass_100db + 4000;
    const Stereo ir = render_impulse(engine, length);

    double peak = 0.0;
    for (float v : ir.left) peak = std::max(peak, static_cast<double>(std::fabs(v)));

    const int start = engine.window_samples() + allpass_100db;
    double tail = 0.0;
    for (int n = start; n < length; ++n)
        tail = std::max(tail, static_cast<double>(std::fabs(ir.left[static_cast<std::size_t>(n)])));
    INFO("tail " << 20.0 * std::log10(std::max(tail / peak, 1e-30)) << " dB below peak");
    REQUIRE(tail < peak * 1e-5);
}

// ── N6 / spec T2 — echo density ───────────────────────────────────────────

TEST_CASE("Nonlin ambience: echo density follows the shipped density law",
          "[signal][nonlin-ambience][density]") {
    // Defect D3: T2's "NED ≥ 0.9" is unreachable — see N17 for the arithmetic.
    // What T2 protects is that the field starts sparse and becomes dense under
    // the physical growth law and stays flat when that law is switched off.
    // Both are asserted here against the shipped `Nd(u)`.
    auto measure_ned = [](const std::vector<float>& x, int centre, int win) {
        const int start = centre - win / 2;
        double sum = 0.0;
        for (int i = start; i < start + win; ++i) {
            const double v = x[static_cast<std::size_t>(i)];
            sum += v * v;
        }
        const double sigma = std::sqrt(sum / win);
        int above = 0;
        for (int i = start; i < start + win; ++i)
            if (std::fabs(static_cast<double>(x[static_cast<std::size_t>(i)])) > sigma)
                ++above;
        return above / (kErfcHalfRoot2 * win);
    };

    const int win = static_cast<int>(0.020 * kFs);

    SECTION("gamma = 2 grows density in proportion to Nd(u)") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::ambience, 1000.0);
        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 2000);

        // With the tilt neutralised every tap spreads over the same number of
        // samples, so NED is proportional to Nd(u) with one unknown constant.
        // Solve that constant at the first probe and require the rest to follow
        // the law — that is the density claim, with nothing fitted per point.
        const double probes[] = {0.05, 0.2, 0.4, 0.6, 0.8, 0.95};
        double scale = 0.0;
        for (std::size_t i = 0; i < std::size(probes); ++i) {
            const double u = probes[i];
            const double ned = measure_ned(ir.left, lag + static_cast<int>(u * window), win);
            const double law = spec_density(u, na::kDensityRefPct, na::kGammaDefault);
            if (i == 0) {
                scale = ned / law;
                REQUIRE(scale > 0.0);
                continue;
            }
            INFO("u " << u << " NED " << ned << " law " << law << " scale " << scale);
            REQUIRE_THAT(ned, Catch::Matchers::WithinRel(scale * law, 0.20));
        }

        // Sparse early, dense late — Moorer 1979's t² growth, which is what the
        // shipped gamma is.
        const double early = measure_ned(ir.left, lag + static_cast<int>(0.05 * window), win);
        const double late = measure_ned(ir.left, lag + static_cast<int>(0.95 * window), win);
        const double law_ratio = spec_density(0.95, na::kDensityRefPct, na::kGammaDefault) /
                                 spec_density(0.05, na::kDensityRefPct, na::kGammaDefault);
        INFO("early " << early << " late " << late << " measured ratio " << late / early
                      << " law ratio " << law_ratio);
        REQUIRE(late > early * 2.0);
        REQUIRE_THAT(late / early, Catch::Matchers::WithinRel(law_ratio, 0.20));
    }

    SECTION("gamma = 0 holds density flat, which is how gated breaks physics") {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_length_ms(1000.0);
        engine.set_density_growth(0.0);
        engine.set_diffusion(0.0);
        engine.set_hf_damp_hz(na::kFcBright);
        engine.reset();

        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 2000);

        const double early = measure_ned(ir.left, lag + static_cast<int>(0.1 * window), win);
        const double late = measure_ned(ir.left, lag + static_cast<int>(0.9 * window), win);
        INFO("early " << early << " late " << late);
        REQUIRE_THAT(late, Catch::Matchers::WithinRel(early, 0.15));
    }

    SECTION("a gated field's active-sample count falls to exactly zero") {
        NonlinAmbience engine;
        configure_naked(engine, NonlinProgram::gated, 1000.0);
        const int window = engine.window_samples();
        const int lag = bypassed_diffuser_delay(engine);
        const Stereo ir = render_impulse(engine, window + lag + 4000);

        const int probe =
            lag + static_cast<int>((na::kGateHold + na::kGateFall) * window) +
            segment_flush_samples(na::kFcBright);
        int active = 0;
        for (int i = probe; i < probe + win; ++i)
            if (ir.left[static_cast<std::size_t>(i)] != 0.0f) ++active;
        REQUIRE(active == 0);
    }
}

// ── N7 / spec T3 — the tap-count law ──────────────────────────────────────

TEST_CASE("Nonlin ambience: the tap count follows the density integral",
          "[signal][nonlin-ambience][density]") {
    // Expected count is ∫₀^T Nd(t/T) dt = T·(Nd_min + (Nd_max − Nd_min)/(γ+1)),
    // computed from shipped constants. Spec T3 allows ±3 %; measured error is
    // under 0.2 %, so the tolerance is not doing any work — which is the point.
    auto expected_count = [](double length_ms, double density_pct, double gamma) {
        const double seconds = length_ms / 1000.0;
        const double nd_min = na::kNdMin * (density_pct / na::kDensityRefPct);
        return seconds * (nd_min + (na::kNdMax - nd_min) / (gamma + 1.0));
    };

    SECTION("count scales with length") {
        double previous = 0.0;
        for (double length_ms : {175.0, 350.0, 700.0}) {
            NonlinAmbience engine;
            engine.prepare(kFs, na::kMaxLengthMs);
            engine.set_length_ms(length_ms);
            engine.reset();
            const double expected =
                expected_count(length_ms, na::kDensityRefPct, na::kGammaDefault);
            INFO("length " << length_ms << " ms: " << engine.tap_count(0) << " taps, expected "
                           << expected);
            REQUIRE_THAT(static_cast<double>(engine.tap_count(0)),
                         Catch::Matchers::WithinRel(expected, 0.03));
            REQUIRE_THAT(static_cast<double>(engine.tap_count(1)),
                         Catch::Matchers::WithinRel(expected, 0.03));
            // Doubling the window doubles the count: γ acts on normalised u, so
            // the count is linear in absolute time.
            if (previous > 0.0)
                REQUIRE_THAT(engine.tap_count(0) / previous,
                             Catch::Matchers::WithinRel(2.0, 0.03));
            previous = engine.tap_count(0);
        }
    }

    SECTION("density_pct scales Nd_min only") {
        for (double pct : {10.0, 30.0, 60.0, 100.0}) {
            NonlinAmbience engine;
            engine.prepare(kFs, na::kMaxLengthMs);
            engine.set_length_ms(350.0);
            engine.set_density_pct(pct);
            engine.reset();
            INFO("density " << pct << " %");
            REQUIRE_THAT(static_cast<double>(engine.tap_count(0)),
                         Catch::Matchers::WithinRel(
                             expected_count(350.0, pct, na::kGammaDefault), 0.03));
        }
    }

    SECTION("a gated program stores only the taps inside its own gate") {
        // Taps whose envelope is exactly zero are past a hard gate and are
        // dropped rather than stored, which is what makes "the response is zero
        // after the last tap" a statement about the last AUDIBLE tap. So a
        // gated program's count is the density integral truncated at h + w, not
        // the full-window integral.
        NonlinAmbience gated, ambience;
        for (auto* engine : {&gated, &ambience}) {
            engine->prepare(kFs, na::kMaxLengthMs);
            engine->set_length_ms(300.0);
        }
        gated.set_program(NonlinProgram::gated);
        gated.reset();
        ambience.reset();

        const double seconds = 0.3;
        const double cut = na::kGateHold + na::kGateFall;
        const double truncated =
            seconds * (na::kNdMin * cut + (na::kNdMax - na::kNdMin) * std::pow(cut, 3) / 3.0);
        INFO("gated " << gated.tap_count(0) << " taps against a truncated integral of "
                      << truncated);
        REQUIRE_THAT(static_cast<double>(gated.tap_count(0)),
                     Catch::Matchers::WithinRel(truncated, 0.03));
        REQUIRE(gated.tap_count(0) < ambience.tap_count(0));
    }

    SECTION("the pre-sized table cannot be outgrown") {
        // The grid never steps less than fs/Nd_max samples, so the count is at
        // most (length_ms/1000)·Nd_max — analytically 8000 here, independent of
        // sample rate. In floating point the walk can take ONE extra step: at
        // 44.1 kHz the grid is 44100/4000 = 11.025 samples, which is not
        // representable, and 8000 accumulated additions land a hair below the
        // window end. That single tap is precisely what `kTapGuard` is for, and
        // this section is the assertion that the guard is doing real work rather
        // than being decorative.
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
            NonlinAmbience engine;
            engine.prepare(rate, na::kMaxLengthMs);
            engine.set_length_ms(na::kMaxLengthMs);
            engine.set_density_pct(100.0);
            engine.set_density_growth(0.0);
            engine.reset();

            const int analytic =
                static_cast<int>(std::ceil(na::kNdMax * na::kMaxLengthMs / 1000.0));
            const int capacity = analytic + na::kTapGuard;
            for (int ch = 0; ch < 2; ++ch) {
                INFO("rate " << rate << " channel " << ch << ": " << engine.tap_count(ch)
                             << " taps, analytic ceiling " << analytic << ", capacity "
                             << capacity);
                // Within one step of the analytic ceiling — the accumulation
                // slack, and nothing larger hiding behind the guard.
                REQUIRE(engine.tap_count(ch) <= analytic + 1);
                // And strictly inside capacity, which proves the generator's
                // hard stop never fired and silently truncated the field.
                REQUIRE(engine.tap_count(ch) < capacity);
            }
        }
    }
}

// ── N8 / spec T4 — determinism (series law 2) ─────────────────────────────

TEST_CASE("Nonlin ambience: renders are bit-identical for the same parameters",
          "[signal][nonlin-ambience][determinism]") {
    const int length = static_cast<int>(2.0 * kFs);
    const auto stimulus = pink_ish(length, 0xC0FFEEu);

    for (NonlinProgram program : kAllPrograms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(150.0);
        engine.set_converter_amount(0.5);  // exercise the dither stream too
        engine.reset();

        auto run = [&] {
            std::vector<float> l = stimulus, r = stimulus;
            engine.process(l.data(), r.data(), length);
            return std::pair{l, r};
        };

        const auto first = run();
        engine.reset();
        const auto second = run();

        INFO("program " << program_name(program));
        REQUIRE(first.first == second.first);
        REQUIRE(first.second == second.second);

        // A round trip through another program and back rebuilds the tables;
        // the result must be the same tables, not merely similar ones.
        engine.set_program(NonlinProgram::reverse);
        engine.set_program(program);
        engine.reset();
        const auto third = run();
        REQUIRE(first.first == third.first);
        REQUIRE(first.second == third.second);
    }
}

TEST_CASE("Nonlin ambience: the seed selects the realization and nothing else",
          "[signal][nonlin-ambience][determinism]") {
    NonlinAmbience a, b;
    for (auto* engine : {&a, &b}) {
        engine->prepare(kFs, na::kMaxLengthMs);
        engine->set_length_ms(400.0);
    }
    b.set_seed(na::kDefaultSeed ^ 0x1234u);
    a.reset();
    b.reset();

    // Same count and same L1 budget — the seed moves taps, it does not change
    // how many there are or how loud the field is.
    REQUIRE(a.tap_count(0) == b.tap_count(0));
    REQUIRE_THAT(a.tap_norm(0), Catch::Matchers::WithinRel(b.tap_norm(0), 0.05));

    int differing = 0;
    for (int k = 0; k < a.tap_count(0); ++k)
        if (a.tap(0, k).delay != b.tap(0, k).delay) ++differing;
    REQUIRE(differing > a.tap_count(0) / 2);
}

// ── N9 / spec T5 — stereo ─────────────────────────────────────────────────

TEST_CASE("Nonlin ambience: the two channels are independent realizations",
          "[signal][nonlin-ambience][stereo]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_width_pct(100.0);
    engine.reset();

    const Stereo ir = render_impulse(engine, engine.window_samples() + 8000);
    const int start = static_cast<int>(0.1 * engine.window_samples());

    double ll = 0.0, rr = 0.0, lr = 0.0;
    for (std::size_t i = static_cast<std::size_t>(start); i < ir.left.size(); ++i) {
        const double l = ir.left[i], r = ir.right[i];
        ll += l * l;
        rr += r * r;
        lr += l * r;
    }
    const double rho = lr / std::sqrt(ll * rr);
    INFO("inter-channel correlation " << rho);
    // Spec T5's kDecorrThresh. Measured is ~0.01, so this is not a tuned number.
    REQUIRE(std::fabs(rho) <= 0.2);
}

TEST_CASE("Nonlin ambience: width 0 is exactly mono",
          "[signal][nonlin-ambience][stereo]") {
    // The third of the header's shipped-behaviour adjudications: §4.6 describes
    // width as crossfading toward "both channels from seed_L", which would be a
    // rebuild, and §1 classifies width as a continuous parameter that must never
    // trigger one. The mid/side law reaches the same observable — bit-identical
    // channels — without a rebuild.
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_width_pct(0.0);
    engine.reset();

    const Stereo ir = render_impulse(engine, engine.window_samples() + 4000);
    REQUIRE(ir.left == ir.right);

    double peak = 0.0;
    for (float v : ir.left) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    REQUIRE(peak > 1e-4);  // mono, not silent
}

// ── N10 / spec T6 — latency (series law 5) ────────────────────────────────

TEST_CASE("Nonlin ambience: latency is exactly zero and the dry path is a wire",
          "[signal][nonlin-ambience][latency]") {
    REQUIRE(NonlinAmbience::latency_samples() == 0);

    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_mix_pct(0.0);
    engine.reset();

    const Stereo ir = render_impulse(engine, 8000);
    REQUIRE(ir.left[0] == 1.0f);
    REQUIRE(ir.right[0] == 1.0f);

    // Not merely "small": exactly zero. At mix = 0 the dry path is a wire and
    // no wet energy leaks past it at all.
    double leak = 0.0;
    for (std::size_t n = 1; n < ir.left.size(); ++n)
        leak = std::max({leak, std::fabs(static_cast<double>(ir.left[n])),
                         std::fabs(static_cast<double>(ir.right[n]))});
    INFO("largest wet leak at mix = 0: " << leak);
    REQUIRE(leak == 0.0);
}

// ── N11 / spec T7 — the worst-case gain is a tested bound (series law 8) ──

TEST_CASE("Nonlin ambience: the rendered L1 gain stays under the shipped bound",
          "[signal][nonlin-ambience][gain]") {
    // The registry value is a closed form of the shipped constants, recomputed
    // here rather than restated, and then asserted against the actual response.
    const double bound = na::worst_case_gain();
    REQUIRE_THAT(bound,
                 Catch::Matchers::WithinRel(
                     std::pow(1.0 + 2.0 * na::kDiffusionDefault, na::kNumAllpass) *
                         na::kL1Budget,
                     1e-12));
    REQUIRE_THAT(bound, Catch::Matchers::WithinAbs(23.04, 1e-9));

    // Render length: the window plus the diffuser's own 100 dB ring time, so
    // the sum captures every sample that can contribute to it. Computed from
    // the shipped coefficient rather than picked.
    auto ring_tail = [](const NonlinAmbience& engine) {
        const double repetitions = std::log(1e-5) / std::log(na::kDiffusionDefault);
        return static_cast<int>(std::ceil(
            repetitions * (engine.allpass_length(0) + engine.allpass_length(1))));
    };

    auto check = [&](NonlinProgram program, double length_ms) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_program(program);
        engine.set_length_ms(length_ms);
        engine.reset();
        REQUIRE_THAT(engine.worst_case_gain(), Catch::Matchers::WithinRel(bound, 1e-12));

        const Stereo ir =
            render_impulse(engine, engine.window_samples() + ring_tail(engine));
        double l1_left = 0.0, l1_right = 0.0;
        for (std::size_t n = 0; n < ir.left.size(); ++n) {
            l1_left += std::fabs(static_cast<double>(ir.left[n]));
            l1_right += std::fabs(static_cast<double>(ir.right[n]));
        }
        INFO("program " << program_name(program) << " length " << length_ms
                        << " ms: L1 = " << l1_left << " / " << l1_right << ", bound "
                        << bound);
        REQUIRE(l1_left <= bound);
        REQUIRE(l1_right <= bound);
        // The bound is not vacuous either — a bound ten times the truth would
        // be no bound at all.
        REQUIRE(std::max(l1_left, l1_right) > 0.35 * bound);
    };

    for (NonlinProgram program : kAllPrograms)
        for (double length_ms : {50.0, 400.0}) check(program, length_ms);

    // One pass at the parameter maximum, where the tap count is highest. The
    // bound is length-independent by construction (the L1 budget is normalised),
    // so this guards against a length-dependent regression without paying for
    // four full-length renders; N7 exercises the maximum length on every
    // channel and sample rate.
    check(NonlinProgram::ambience, na::kMaxLengthMs);
}

TEST_CASE("Nonlin ambience: the bound tracks diffusion and the converter stage",
          "[signal][nonlin-ambience][gain]") {
    // Defect D4: the DC blocker's L1 gain is exactly 2, so a chain containing
    // one has twice the bound. That is why the default path does not contain
    // one, and why the bound reported with the converter engaged is doubled.
    REQUIRE_THAT(na::worst_case_gain(0.0), Catch::Matchers::WithinRel(na::kL1Budget, 1e-12));
    REQUIRE_THAT(na::worst_case_gain(na::kDiffusionDefault, true),
                 Catch::Matchers::WithinRel(2.0 * na::worst_case_gain(), 1e-12));

    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.set_diffusion(0.0);
    engine.reset();
    REQUIRE_THAT(engine.worst_case_gain(),
                 Catch::Matchers::WithinRel(na::kL1Budget, 1e-12));

    // With the diffuser bypassed the FIR's L1 gain is exactly the budget, by
    // construction — the taps are the whole response, so the bound is tight.
    const Stereo ir = render_impulse(engine, engine.window_samples() + 8000);
    double l1 = 0.0;
    for (float v : ir.left) l1 += std::fabs(static_cast<double>(v));
    INFO("L1 with diffuser bypassed = " << l1 << ", budget " << na::kL1Budget);
    REQUIRE(l1 <= na::kL1Budget);
    REQUIRE(l1 > 0.75 * na::kL1Budget);
}

// ── N12 / spec T8 — RT allocation probe ───────────────────────────────────

TEST_CASE("Nonlin ambience: process and the rebuild-swap path never allocate",
          "[signal][nonlin-ambience][rt]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(200.0);
    engine.reset();

    constexpr int kBlock = 256;
    std::vector<float> left(kBlock), right(kBlock);
    const auto stimulus = pink_ish(kBlock, 0xBEEFu);

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            left = stimulus;
            right = stimulus;
            for (int n = 0; n < kBlock; ++n) {
                // Per-sample automation of every continuous parameter, per T8.
                const double u = static_cast<double>(n) / kBlock;
                engine.set_mix_pct(100.0 * u);
                engine.set_tone(2.0 * u - 1.0);
                engine.set_width_pct(100.0 * (1.0 - u));
                engine.set_output_gain_db(6.0 * u - 3.0);
                engine.process_sample(left[static_cast<std::size_t>(n)],
                                      right[static_cast<std::size_t>(n)]);
            }
            // ...and a topology swap partway through, which is the path that
            // regenerates ~1500 taps into the pre-sized back bank.
            if (block == 100) engine.set_program(NonlinProgram::gated);
            if (block == 150) engine.set_length_ms(180.0);
        }
        REQUIRE(probe.allocation_count() == 0);
    }

    // Every topology setter, once each, still inside a probe.
    {
        pulp::test::RtAllocationProbe probe;
        engine.set_program(NonlinProgram::reverse);
        engine.set_length_ms(120.0);
        engine.set_predelay_ms(15.0);
        engine.set_density_pct(85.0);
        engine.set_density_growth(1.0);
        engine.set_gate_hold_pct(50.0);
        engine.set_attack_pct(60.0);
        engine.set_seed(0x1234u);
        engine.set_diffusion(0.4);
        engine.set_converter_amount(0.7);
        engine.reset();
        engine.process(left.data(), right.data(), kBlock);
        REQUIRE(probe.allocation_count() == 0);
    }
}

// ── N13 / spec T9 — denormal safety ───────────────────────────────────────

TEST_CASE("Nonlin ambience: silence after a loud transient flushes to zero",
          "[signal][nonlin-ambience][rt]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(400.0);
    engine.reset();

    const int length = static_cast<int>(5.0 * kFs);
    std::vector<float> left(static_cast<std::size_t>(length), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(length), 0.0f);
    left[0] = 4.0f;
    right[0] = 4.0f;
    engine.process(left.data(), right.data(), length);

    int denormals = 0, first_denormal = -1;
    for (int n = 0; n < length; ++n) {
        const bool bad = pulp::signal::is_denormal(left[static_cast<std::size_t>(n)]) ||
                         pulp::signal::is_denormal(right[static_cast<std::size_t>(n)]);
        if (bad) {
            ++denormals;
            if (first_denormal < 0) first_denormal = n;
        }
    }
    INFO(denormals << " denormal samples, first at " << first_denormal);
    REQUIRE(denormals == 0);
    // Every recursive state — both allpasses and all sixteen segment one-poles —
    // snaps through the denormal threshold, so the tail reaches exact zero
    // rather than grinding on subnormals forever.
    REQUIRE(left[static_cast<std::size_t>(length - 1)] == 0.0f);
    REQUIRE(right[static_cast<std::size_t>(length - 1)] == 0.0f);
}

// ── N14 — the program swap ────────────────────────────────────────────────

TEST_CASE("Nonlin ambience: a program change crossfades instead of clicking",
          "[signal][nonlin-ambience][swap]") {
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_length_ms(300.0);
    engine.set_program(NonlinProgram::gated);
    engine.reset();

    const int length = static_cast<int>(1.0 * kFs);
    auto stimulus = pink_ish(length, 0x5EEDu);
    std::vector<float> left = stimulus, right = stimulus;

    const int swap_at = length / 2;
    engine.process(left.data(), right.data(), swap_at);
    REQUIRE_FALSE(engine.swap_in_progress());
    engine.set_program(NonlinProgram::reverse);
    REQUIRE(engine.swap_in_progress());
    engine.process(left.data() + swap_at, right.data() + swap_at, length - swap_at);
    REQUIRE_FALSE(engine.swap_in_progress());

    // No sample-to-sample step larger than the largest one the same signal
    // produces while NOT swapping. A hard bank switch shows up here as a
    // discontinuity of the order of the signal itself.
    auto largest_step = [&](int from, int to) {
        double worst = 0.0;
        for (int n = from + 1; n < to; ++n)
            worst = std::max(worst, std::fabs(static_cast<double>(
                                        left[static_cast<std::size_t>(n)] -
                                        left[static_cast<std::size_t>(n - 1)])));
        return worst;
    };
    const double quiescent = largest_step(1000, swap_at - 1000);
    const int fade = static_cast<int>(na::kSwapFadeMs * kFs / 1000.0);
    const double during = largest_step(swap_at - 4, swap_at + fade + 4);
    INFO("largest step while quiescent " << quiescent << ", across the swap " << during);
    REQUIRE(during < quiescent * 1.5);
}

// ── N15 — the segment tilt ────────────────────────────────────────────────

TEST_CASE("Nonlin ambience: the field darkens over time, and tone steers it",
          "[signal][nonlin-ambience][tilt]") {
    // §4.5's spectral evolution. Measured as the high-frequency energy fraction
    // early versus late in the response, which is what "loses highs over time"
    // means without needing a full spectrum.
    auto hf_fraction = [](const std::vector<float>& x, int start, int count) {
        // One-zero difference as the HF probe, one-pole sum as the LF probe.
        double hf = 0.0, total = 0.0;
        for (int n = start + 1; n < start + count; ++n) {
            const double d = static_cast<double>(x[static_cast<std::size_t>(n)]) -
                             static_cast<double>(x[static_cast<std::size_t>(n - 1)]);
            hf += d * d;
            total += static_cast<double>(x[static_cast<std::size_t>(n)]) *
                     static_cast<double>(x[static_cast<std::size_t>(n)]);
        }
        return hf / std::max(total, 1e-30);
    };

    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(NonlinProgram::gated);  // flat envelope isolates the colour
    engine.set_length_ms(800.0);
    engine.set_diffusion(0.0);
    engine.reset();

    const int window = engine.window_samples();
    const int lag = bypassed_diffuser_delay(engine);
    const int span = static_cast<int>(0.05 * kFs);
    const Stereo ir = render_impulse(engine, window + lag + 4000);

    const double early = hf_fraction(ir.left, lag + static_cast<int>(0.05 * window), span);
    const double late = hf_fraction(ir.left, lag + static_cast<int>(0.60 * window), span);
    INFO("HF fraction early " << early << " late " << late);
    REQUIRE(late < early);

    // tone > 0 keeps highs later; the late field must brighten relative to the
    // neutral setting, and the neutral setting relative to tone < 0.
    auto late_hf_at_tone = [&](double tone) {
        NonlinAmbience e;
        e.prepare(kFs, na::kMaxLengthMs);
        e.set_program(NonlinProgram::gated);
        e.set_length_ms(800.0);
        e.set_diffusion(0.0);
        e.set_tone(tone);
        e.reset();
        const Stereo r = render_impulse(e, e.window_samples() + lag + 4000);
        return hf_fraction(r.left, lag + static_cast<int>(0.60 * e.window_samples()), span);
    };
    const double dark = late_hf_at_tone(-1.0);
    const double bright = late_hf_at_tone(1.0);
    INFO("late HF fraction: dark " << dark << " neutral " << late << " bright " << bright);
    REQUIRE(bright > late);
    REQUIRE(late > dark);
}

TEST_CASE("Nonlin ambience: reverse brightens into its swell",
          "[signal][nonlin-ambience][tilt]") {
    // §4.5's program-specific flag: the Reverse program maps segments backwards
    // so the field gets brighter as the swell approaches its peak, which is the
    // opposite of every other program and of every real room.
    NonlinAmbience engine;
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.set_program(NonlinProgram::reverse);
    engine.set_length_ms(800.0);
    engine.reset();

    // The reversal is visible directly in the tap table: early taps carry high
    // segment indices (the dark end) and late taps carry low ones.
    const int first = engine.tap(0, 0).segment;
    const int last = engine.tap(0, engine.tap_count(0) - 1).segment;
    INFO("first tap segment " << first << ", last tap segment " << last);
    REQUIRE(first == na::kSegments - 1);
    REQUIRE(last == 0);

    NonlinAmbience forward;
    forward.prepare(kFs, na::kMaxLengthMs);
    forward.set_program(NonlinProgram::ambience);
    forward.set_length_ms(800.0);
    forward.reset();
    REQUIRE(forward.tap(0, 0).segment == 0);
    REQUIRE(forward.tap(0, forward.tap_count(0) - 1).segment == na::kSegments - 1);
}

// ── N16 — the optional converter character ────────────────────────────────

TEST_CASE("Nonlin ambience: the converter stage is off by default and quantizes when on",
          "[signal][nonlin-ambience][converter]") {
    auto render = [](double amount) {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_length_ms(200.0);
        engine.set_converter_amount(amount);
        engine.reset();
        return render_impulse(engine, engine.window_samples() + 8000);
    };

    const Stereo off = render(0.0);
    const Stereo on = render(1.0);

    // Off is the default, and off means bit-for-bit untouched.
    NonlinAmbience defaulted;
    defaulted.prepare(kFs, na::kMaxLengthMs);
    defaulted.set_length_ms(200.0);
    defaulted.reset();
    const Stereo baseline =
        render_impulse(defaulted, defaulted.window_samples() + 8000);
    REQUIRE(baseline.left == off.left);

    // On is audibly different, and the difference is in the direction the stage
    // says: the DC blocker and the low corner remove sub-`kConverterFcLo`
    // energy, and the quantizer adds a noise floor.
    double difference = 0.0, reference = 0.0;
    for (std::size_t n = 0; n < off.left.size(); ++n) {
        const double d = static_cast<double>(on.left[n]) - off.left[n];
        difference += d * d;
        reference += static_cast<double>(off.left[n]) * off.left[n];
    }
    INFO("converter difference " << 10.0 * std::log10(difference / reference) << " dB");
    REQUIRE(difference > 0.0);

    // The word length follows the shipped formula, so the quantizer's step is a
    // computed quantity rather than an implied one.
    const int bits = na::kConverterBitsMax -
                     static_cast<int>(std::lround(na::kConverterBitSweep * 1.0));
    REQUIRE(bits == 12);
}

// ── N17 — the spec's own arithmetic, asserted rather than described ───────

TEST_CASE("Nonlin ambience: the five re-scoped spec criteria, proven with numbers",
          "[signal][nonlin-ambience][spec-defects]") {
    // Each block below re-derives, from the shipped constants, why the criterion
    // as written cannot hold. If a shipped constant changes so that it CAN hold,
    // these assertions fail and the re-scoping above must be revisited — which
    // is the point of asserting the arithmetic instead of writing it in a
    // comment.

    SECTION("D1: the segment tilt alone moves a broadband RMS envelope by 3.19 dB") {
        const double bright = onepole_noise_power(spec_segment_pole(0, 0.0, na::kFcDark));
        const double dark = onepole_noise_power(
            spec_segment_pole(na::kSegments - 1, 0.0, na::kFcDark));
        const double tilt_db = 10.0 * std::log10(bright / dark);
        INFO("tilt across the window = " << tilt_db << " dB");
        REQUIRE(tilt_db > 3.0);
        // ...which is more than T1's entire ±1.0 dB Gated flatness budget, and
        // the Gated body covers most of it.
        const double body_fraction = na::kGateHold;
        REQUIRE(tilt_db * body_fraction > 2.0);
    }

    SECTION("D2: the mandated diffuser rings far longer than 2w allows") {
        NonlinAmbience engine;
        engine.prepare(kFs, na::kMaxLengthMs);
        engine.set_length_ms(400.0);
        engine.reset();

        const double repetitions = std::log(1e-3) / std::log(na::kDiffusionDefault);
        const int ring_60db = static_cast<int>(std::ceil(
            repetitions * std::max(engine.allpass_length(0), engine.allpass_length(1))));
        const int allowed =
            static_cast<int>(2.0 * na::kGateFall * engine.window_samples());
        INFO("allpass 60 dB time " << ring_60db << " samples, T1 allows " << allowed);
        REQUIRE(ring_60db > allowed);
        // The window length at which T1's criterion would become achievable —
        // reported so the re-scoping is a bounded statement, not a shrug.
        const double achievable_ms =
            ring_60db * 1000.0 / kFs / (2.0 * na::kGateFall);
        INFO("T1's criterion needs length_ms >= " << achievable_ms);
        REQUIRE(achievable_ms > 400.0);
    }

    SECTION("D3: normalized echo density cannot reach 0.9 at any shipped density") {
        // NED counts samples above the local RMS. A sparse cloud's only such
        // samples are its pulses, so NED <= (Nd/fs)/erfc(1/sqrt2).
        const double ceiling = na::kNdMax / kFs / kErfcHalfRoot2;
        INFO("NED ceiling at kNdMax = " << ceiling << "; T2 demands >= 0.9");
        REQUIRE(ceiling < 0.9);
        // The density that WOULD reach 1, for the record: 0.3173·fs.
        const double required = kErfcHalfRoot2 * kFs;
        INFO("NED = 1 needs " << required << " pulses/s, " << required / na::kNdMax
                              << "x the shipped maximum");
        REQUIRE(required > 3.0 * na::kNdMax);
    }

    SECTION("D4: a DC blocker in the path would double the shipped bound") {
        REQUIRE_THAT(na::kDcBlockerL1Gain, Catch::Matchers::WithinAbs(2.0, 1e-12));
        REQUIRE_THAT(na::worst_case_gain(na::kDiffusionDefault, true),
                     Catch::Matchers::WithinAbs(46.08, 1e-9));
    }

    SECTION("D5: the spec's gain law would tilt the envelope by 7 dB") {
        // Without the sqrt(Td) weight, a window's RMS is E(τ)·sqrt(Nd(τ)), so
        // the measured envelope carries half the density ratio in dB.
        const double early = spec_density(0.0, na::kDensityRefPct, na::kGammaDefault);
        const double late = spec_density(1.0, na::kDensityRefPct, na::kGammaDefault);
        const double tilt_db = 10.0 * std::log10(late / early);
        INFO("uncompensated envelope tilt across the window = " << tilt_db << " dB");
        REQUIRE(tilt_db > 6.0);
        // And across the Gated body specifically, which T1 wants flat to ±1 dB.
        const double body =
            10.0 * std::log10(spec_density(na::kGateHold, na::kDensityRefPct,
                                           na::kGammaDefault) /
                              early);
        INFO("across the gated body = " << body << " dB against a ±1.0 dB budget");
        REQUIRE(body > 4.0);
    }
}

// ── Housekeeping ──────────────────────────────────────────────────────────

TEST_CASE("Nonlin ambience: prepare and reset leave a usable engine",
          "[signal][nonlin-ambience][lifecycle]") {
    NonlinAmbience engine;

    // Parameter calls before prepare must not crash or leave state that a later
    // prepare cannot repair.
    engine.set_program(NonlinProgram::gated);
    engine.set_length_ms(500.0);
    engine.prepare(kFs, na::kMaxLengthMs);
    engine.reset();
    REQUIRE(engine.tap_count(0) > 0);
    REQUIRE(engine.program() == NonlinProgram::gated);
    REQUIRE_THAT(engine.length_ms(), Catch::Matchers::WithinAbs(500.0, 1e-9));

    // Re-preparing at a different rate rebuilds coherently.
    engine.prepare(96000.0, na::kMaxLengthMs);
    engine.reset();
    REQUIRE_THAT(static_cast<double>(engine.window_samples()),
                 Catch::Matchers::WithinRel(0.5 * 96000.0, 1e-3));
    REQUIRE(engine.tap_count(0) > 0);

    // Parameters are clamped to their documented ranges rather than trusted.
    engine.set_length_ms(1e9);
    REQUIRE(engine.length_ms() <= na::kMaxLengthMs);
    engine.set_tone(50.0);
    REQUIRE_THAT(engine.tone(), Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("Nonlin ambience: the engine works at every supported sample rate",
          "[signal][nonlin-ambience][lifecycle]") {
    for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        NonlinAmbience engine;
        engine.prepare(rate, na::kMaxLengthMs);
        engine.set_program(NonlinProgram::ambience);
        engine.set_length_ms(300.0);
        engine.reset();

        INFO("sample rate " << rate);
        // The envelope is scale invariant (series law 7): the tap COUNT depends
        // only on time and density, never on the sample rate. Ambience rather
        // than a gated program, so the expectation is the plain density
        // integral with no truncation term — the gated truncation is asserted
        // on its own in N7.
        REQUIRE_THAT(static_cast<double>(engine.tap_count(0)),
                     Catch::Matchers::WithinRel(
                         0.3 * (na::kNdMin + (na::kNdMax - na::kNdMin) /
                                                 (na::kGammaDefault + 1.0)),
                         0.02));

        const int length = engine.window_samples() + 8000;
        std::vector<float> left(static_cast<std::size_t>(length), 0.0f);
        std::vector<float> right(static_cast<std::size_t>(length), 0.0f);
        left[0] = 1.0f;
        right[0] = 1.0f;
        engine.process(left.data(), right.data(), length);
        for (float v : left) REQUIRE(std::isfinite(v));

        // The allpass delays land on primes at every rate, which is what keeps
        // the two combs from aligning.
        for (int i = 0; i < na::kNumAllpass; ++i) {
            const int m = engine.allpass_length(i);
            for (int d = 2; d * d <= m; ++d) REQUIRE(m % d != 0);
        }
    }
}
