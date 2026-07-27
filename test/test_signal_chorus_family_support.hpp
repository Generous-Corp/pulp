#pragma once

// ChorusEnsembleT — the chorus family's acceptance suite.
//
// The spec's tests 1–11 (chorus-pulp-module-prompt.md, "Acceptance tests").
// Every expected value is computed from the shipped calibration table at test
// time; nothing is restated as a bare literal, so retuning a constant moves the
// test that documents it instead of silently disagreeing with it.
//
// ── How this suite measures, and why ──────────────────────────────────────
//
// The load-bearing measurements are all about a signal the module does not
// output: the instantaneous delay of each tap. Two instruments are used, and
// the order they appear in matters.
//
//   1. **The click train** (test 1). A unit impulse every 50 ms, mix = 1, and
//      the arrival of each impulse's delayed copy located to sub-sample
//      precision. This measures the delay actually applied to AUDIO, through
//      the real interpolator, and it is the only instrument that can.
//
//   2. **`current_delay_ms()`** (tests 2–5). Exact, cheap, and sampled per
//      sample rather than per click. On its own it would be an accessor
//      agreeing with itself, which is why test 1 spends its budget proving the
//      accessor and the click train agree before anything else relies on it.
//
// The click train alone cannot carry tests 2–5: it samples the delay trace at
// 20 Hz, and taking a min or max of a 20 Hz-sampled TRIANGLE under-reads its
// corner by up to `4 · (1 / (2 · samples_per_cycle))` of the depth — 12 % at
// the CE-2's 1.2 Hz, 5.1 % at the Juno's 0.513 Hz. Both dwarf the ±2 % the
// delay-range criterion allows. So test 1 does not take a min or max of the
// raw trace either: it fits the trace against the independently written
// reference LFO by least squares, which uses every sample and is unbiased.
// The residual of that fit is itself asserted, so a wrong SHAPE fails the test
// rather than hiding inside a two-parameter fit.
//
// The same reasoning kills naive peak-picking everywhere else in this file:
// frequency-response points are coherent DFTs over a whole number of periods,
// never the largest sample of a rendered sine.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/chorus_family.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Chorus = ChorusEnsembleT<double>;
using Voicing = Chorus::Voicing;
using JunoMode = Chorus::JunoMode;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// ── Reference modulation, written out independently of the header ─────────
//
// `LfoT` advances before it reads, so the i-th processed sample (i counted from
// 1) sees phase `i · rate / fs + offset`. Both shapes are transcribed from the
// LFO contract, not called through it: a reference that called the shipped LFO
// would agree with the module by construction.

double ref_wrap(double cycles) {
    cycles -= std::floor(cycles);
    return cycles < 1.0 ? cycles : 0.0;
}


double ref_triangle(double phi) {
    phi = ref_wrap(phi);
    if (phi < 0.25) return 4.0 * phi;
    if (phi < 0.75) return 2.0 - 4.0 * phi;
    return 4.0 * phi - 4.0;
}

double ref_sine(double phi) { return std::sin(2.0 * kPi * ref_wrap(phi)); }

/// The modulation the spec says voice `k` of a voicing should carry at sample
/// index `i` (1-based), in `[-1, +1]`.
double reference_modulation(Voicing v, JunoMode mode, int k, long long i, double rate_hz) {
    const auto cal = Chorus::calibration(v);
    const double offset = static_cast<double>(k) / static_cast<double>(cal.voices);
    const double n = static_cast<double>(i);

    if (v == Voicing::juno_ensemble) {
        const auto spec = Chorus::juno_spec(mode);
        const double a = ref_triangle(n * spec.rate_a_hz / kSr + offset);
        if (!spec.dual) return a;
        const double b = ref_triangle(n * spec.rate_b_hz / kSr + offset);
        return std::clamp(0.5 * (a + b), -1.0, 1.0);
    }
    if (v == Voicing::dimension_d)
        return std::clamp(Chorus::kTrapK * ref_triangle(n * rate_hz / kSr + offset), -1.0, 1.0);
    if (cal.wave == LfoWave::sine) return ref_sine(n * rate_hz / kSr + offset);
    return ref_triangle(n * rate_hz / kSr + offset);
}

/// Centre delay and full-depth excursion the shipped tables give a voicing.
struct Window {
    double center_ms;
    double depth_ms;
    double rate_hz;
};

Window shipped_window(Voicing v, JunoMode mode) {
    const auto cal = Chorus::calibration(v);
    if (v == Voicing::juno_ensemble) {
        const auto spec = Chorus::juno_spec(mode);
        return {spec.center_ms, spec.depth_ms, spec.rate_a_hz};
    }
    return {cal.center_ms, cal.depth_ms, cal.rate_hz};
}

// ── Engine construction ───────────────────────────────────────────────────

struct Config {
    Voicing voicing = Voicing::ce2;
    JunoMode mode = JunoMode::mode_I;
    double depth = 1.0;
    double mix = 1.0;
    double width = 0.0;
    bool bbd = false;
};

/// Order matters: `set_voicing` adopts the voicing's shipped rate, so every
/// other setter runs after it.
void configure(Chorus& c, const Config& cfg) {
    c.set_voicing(cfg.voicing);
    c.set_juno_mode(cfg.mode);
    c.set_depth(cfg.depth);
    c.set_mix(cfg.mix);
    c.set_stereo_width(cfg.width);
    c.set_bbd_color(cfg.bbd);
    c.reset();
}

// ── Signal helpers ────────────────────────────────────────────────────────

/// Coherent DFT magnitude and phase at exactly `hz`, over a whole number of
/// periods. The only frequency-domain instrument in this file: the peak sample
/// of a rendered sine under-reads whenever no sample lands on the crest (six
/// samples per cycle at 8 kHz / 48 kHz, none of them on a crest — a −1.25 dB
/// error that looks exactly like a filter that is not flat).
std::complex<double> coherent_bin(const std::vector<double>& x, std::size_t begin,
                                  std::size_t count, double hz) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t n = 0; n < count; ++n) {
        const double theta = -2.0 * kPi * hz * static_cast<double>(n) / kSr;
        acc += x[begin + n] * std::complex<double>(std::cos(theta), std::sin(theta));
    }
    return acc * (2.0 / static_cast<double>(count));
}

std::vector<double> sine(std::size_t n, double hz, double amplitude) {
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr);
    return out;
}

std::vector<double> seeded_noise(std::size_t n, double amplitude, std::uint32_t seed) {
    Xorshift32 rng{seed};
    std::vector<double> out(n);
    for (auto& v : out) v = amplitude * rng.next_bipolar<double>();
    return out;
}

struct Stereo {
    std::vector<double> left;
    std::vector<double> right;
};

Stereo render(const Config& cfg, const std::vector<double>& in_l, const std::vector<double>& in_r) {
    Chorus c;
    c.prepare(kSr);
    configure(c, cfg);
    Stereo out{in_l, in_r};
    c.process(out.left.data(), out.right.data(), static_cast<int>(out.left.size()));
    return out;
}

/// Response of the WET path alone, extracted linearly. Every voicing's mix
/// matrix carries its own dry term, so `mix = 1` is matrix and `mix = 0` is the
/// bare input; for the mono-source voicings the matrix's dry term IS the input
/// when the input is mono, and the difference is the tap and nothing else.
std::vector<double> wet_only(Config cfg, const std::vector<double>& mono) {
    cfg.mix = 1.0;
    const auto wet = render(cfg, mono, mono);
    cfg.mix = 0.0;
    const auto dry = render(cfg, mono, mono);
    std::vector<double> out(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i) out[i] = wet.left[i] - dry.left[i];
    return out;
}

// ── Instrument 1: click-train delay tracking ──────────────────────────────

struct TrackedDelay {
    std::vector<long long> arrival_index;  ///< sample index the echo peaked at
    std::vector<double> delay_samples;
};

/// Locates each impulse's delayed copy in `channel` and returns the delay it
/// arrived with. The peak is refined by a parabolic fit over the three samples
/// around the largest one — the Lagrange kernel's crest sits between samples
/// whenever the fractional delay does.
TrackedDelay track_clicks(const std::vector<double>& channel, long long click_period,
                          double search_lo, double search_hi) {
    TrackedDelay out;
    const auto lo = static_cast<long long>(std::floor(search_lo));
    const auto hi = static_cast<long long>(std::ceil(search_hi));
    for (long long p = 0; p + hi + 2 < static_cast<long long>(channel.size()); p += click_period) {
        long long best = p + lo;
        double best_mag = -1.0;
        for (long long q = p + lo; q <= p + hi; ++q) {
            const double mag = std::abs(channel[static_cast<std::size_t>(q)]);
            if (mag > best_mag) {
                best_mag = mag;
                best = q;
            }
        }
        if (best <= 0 || best + 1 >= static_cast<long long>(channel.size())) continue;
        const double y0 = std::abs(channel[static_cast<std::size_t>(best - 1)]);
        const double y1 = std::abs(channel[static_cast<std::size_t>(best)]);
        const double y2 = std::abs(channel[static_cast<std::size_t>(best + 1)]);
        const double denom = y0 - 2.0 * y1 + y2;
        const double shift = std::abs(denom) > 1e-12 ? 0.5 * (y0 - y2) / denom : 0.0;
        out.arrival_index.push_back(best);
        out.delay_samples.push_back(static_cast<double>(best - p) + std::clamp(shift, -1.0, 1.0));
    }
    return out;
}

/// Two-parameter least squares of `y ≈ center + depth · m`, plus the RMS
/// residual. Fitting rather than min/max-picking is what makes a 20 Hz trace
/// able to resolve a ±2 % criterion on a triangle's corner.
struct Fit {
    double center;
    double depth;
    double residual_rms;
};

Fit fit_modulation(const std::vector<double>& y, const std::vector<double>& m) {
    const auto n = static_cast<double>(y.size());
    const double mean_y = std::accumulate(y.begin(), y.end(), 0.0) / n;
    const double mean_m = std::accumulate(m.begin(), m.end(), 0.0) / n;
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        sxy += (m[i] - mean_m) * (y[i] - mean_y);
        sxx += (m[i] - mean_m) * (m[i] - mean_m);
    }
    const double depth = sxx > 0.0 ? sxy / sxx : 0.0;
    const double center = mean_y - depth * mean_m;
    double residual = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double e = y[i] - (center + depth * m[i]);
        residual += e * e;
    }
    return {center, depth, std::sqrt(residual / n)};
}

/// Collects `current_delay_ms(voice)` per sample over a silent render.
std::vector<double> delay_trace(const Config& cfg, int voice, std::size_t samples) {
    Chorus c;
    c.prepare(kSr);
    configure(c, cfg);
    std::vector<double> trace(samples);
    double l = 0.0;
    double r = 0.0;
    for (std::size_t i = 0; i < samples; ++i) {
        l = 0.0;
        r = 0.0;
        c.process(&l, &r, 1);
        trace[i] = c.current_delay_ms(voice);
    }
    return trace;
}

std::string voicing_name(Voicing v) {
    switch (v) {
        case Voicing::ce2: return "ce2";
        case Voicing::juno_ensemble: return "juno_ensemble";
        case Voicing::dimension_d: return "dimension_d";
        case Voicing::tri_chorus: return "tri_chorus";
    }
    return "?";
}

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// 1. Delay-range accuracy — and the calibration of the accessor
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, documented rather than papered over: the criterion's second
// clause asks the Dimension D and TriChorus delays to "exactly match the
// shipped constants, since there is no external reference to fall short of".
// A MEASUREMENT cannot match anything exactly — the click tracker's own
// resolution is a fraction of a sample — so all four voicings are held to the
// same ±2 % band. Series law 6: acceptance criteria must be physically
// achievable.



// ─────────────────────────────────────────────────────────────────────────
// 2. LFO rate accuracy
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, fixed with a documented reason: the recipe says "zero-crossing
// COUNT over 100 s ... within ±0.01 %". A count is an integer. The CE-2's
// 1.2 Hz gives 240 half-cycles in 100 s, so one count is 0.42 % — forty times
// coarser than the criterion, which no implementation could ever pass. What
// resolves ±0.01 % is crossing TIMING: the first and last upward crossings,
// each linearly interpolated to sub-sample precision, divided by the number of
// whole cycles between them. That instrument resolves ~2·10⁻⁷ here, and it
// still measures exactly what the criterion is about.


// ─────────────────────────────────────────────────────────────────────────
// 3. Phase relationships — what actually distinguishes the voicings
// ─────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────
// 4. Juno I+II combination law
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, fixed with a documented reason: the criterion asks for 1000
// sample points across the 2.857 s beat period — one point every 2.857 ms —
// while the recipe's own click train resolves the delay trace at 20 Hz, one
// point every 50 ms. Fifty-seven points is all that instrument can deliver over
// a beat period, and raising the click rate to 350 Hz would space the clicks
// 2.86 ms apart, closer together than the 3.3–3.7 ms delay being measured, so
// each echo would land after the next click. The 1000-point check therefore
// runs on `current_delay_ms` (calibrated against the audio in test 1), and the
// click train's coarser trace cross-checks it there.



// ─────────────────────────────────────────────────────────────────────────
// 5. Trapezoid dwell fraction
// ─────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────
// 6. Worst-case gain
// ─────────────────────────────────────────────────────────────────────────
//
// Two spec defects here, both found by measurement and both recorded with
// numbers rather than smoothed over.
//
//   * §1.3's `gain ≤ (1 + N)` is **not an upper bound**. It treats a modulated
//     tap as unity gain, but the tap is a delay line read through the 4-point
//     Lagrange kernel, whose coefficients at a half-sample offset are
//     (−1/16, 9/16, 9/16, −1/16) — absolute sum 1.25. The CE-2 measured 2.078
//     against the spec's ceiling of 2 on a full-scale noise probe, which is how
//     this was found. The correct closed form replaces each tap's 1 with
//     `kTapL1`, and the first assertion below recomputes `kTapL1` from the
//     shipped kernel so the constant cannot drift away from the interpolator.
//   * The criterion also asks for "≥10 % headroom margin" against `(1 + N)`.
//     Unachievable, and for a reason worth stating: `(1 + N)` is *attained
//     exactly* by a DC input, because a delay line is transparent to DC and so
//     the dry term and every feedforward tap sit at +1 together. Zero headroom,
//     asserted below. Series law 6.
//   * §4.3's Dimension D bound of 1.41 + 1 + 1 ≈ 3.41 takes the cross-feed
//     high-pass as "≤ 1 (unity passband)". Passband gain is a steady-state
//     sinusoidal figure and not a peak bound; a first-order high-pass has L1
//     norm 2/(1 + tan(π f_c/f_s)) = 1.974 at the shipped 200 Hz and 48 kHz.



// ─────────────────────────────────────────────────────────────────────────
// 7. Dimension D cross-mix
// ─────────────────────────────────────────────────────────────────────────

// Spec defect, and the reason this test measures the cross-mix TERM instead of
// the criterion's "mono sum" difference. As written the criterion says the mono
// sum changes by < 0.5 dB everywhere below `f_c` and by ≥ 1 dB everywhere above
// `2 f_c`. Both halves are false, and neither because the cross-mix misbehaves:
//
//   * Below `f_c` the specified one-pole high-pass still passes 70.7 % of the
//     cross-feed AT `f_c` itself, so a blanket "< f_c ⇒ inaudible" cannot hold
//     for the topology §4.3 specifies. Measured: −0.94 dB at 40 Hz, and +1.5 to
//     +1.8 dB at 80–120 Hz.
//   * Those positive numbers expose the other problem: the mono sum is a 6 ms
//     COMB (dry plus a 6 ms tap), so subtracting part of the wet term can raise
//     the sum as easily as lower it, and the criterion is really measuring the
//     comb. Above `2 f_c` the same comb gives only −0.67 dB at 400 Hz while
//     giving −5.5 dB at 1 kHz. Series law 6.
//
// What is comb-immune, and is what the criterion is actually about, is the
// difference signal `mono(width = 1) − mono(width = 0)`, which is exactly
// `−hpf(wet)`. Its normalised magnitude is asserted against the first-order
// high-pass law computed from the shipped corner. The criterion's own two
// clauses are then kept in the achievable form the measurement supports.


// ─────────────────────────────────────────────────────────────────────────
// 8. BBD colour composition
// ─────────────────────────────────────────────────────────────────────────

// Spec defect, and the reason this test does not assert a −3 dB point against
// the bandwidth law. The criterion asks the wet path's −3 dB point to "match
// Prompt 2's bandwidth law ... ±15 % (same tolerance Prompt 2 itself uses for
// this law — inherited, not loosened)". That inheritance does not survive: the
// sibling suite (`test/test_character_delay.cpp`, "BBD bandwidth follows the
// clock rate") applies its ±15 % to `bbd_bandwidth_hz()`, the reported COEFFICIENT,
// not to a measured audio −3 dB point. `bandwidth_hz` names the corner of each
// of two cascaded 2-pole sections, and the composite path adds a linear
// interpolation stage and a clock-rate resampler on top of them, so its −3 dB
// point necessarily lands well below the nominal corner. Measured here: 6.39 kHz
// against a 10 kHz law — 0.64×, and no correct implementation of the cited
// topology can be closer. Series law 6.
//
// So the coefficient is asserted exactly (1e-9, tighter than the ±15 %), and
// the audio claim — "the wet path narrows to the composed bandwidth" — is
// asserted as three structural facts computed from that same coefficient:
// flat well inside it, far down at it, and monotone in between.


// ─────────────────────────────────────────────────────────────────────────
// 9. Latency
// ─────────────────────────────────────────────────────────────────────────

// A note on what "zero latency" is allowed to mean, after the first draft of
// this test failed on the Dimension D. That draft asserted the output was
// silent between the impulse and the first wet arrival, which the Dimension D
// broke with 0.00481 at sample 1 — exactly `(A − 1) · h_lp[1]` of its dry
// low-shelf, reproduced by hand to six figures. A causal filter's TAIL is not
// lookahead, so that assertion was testing the shelf, not the latency. What
// zero latency actually claims is causality plus an unshifted wet arrival, and
// that is what is asserted below: nothing at all before the input (exact zero,
// not a threshold), and the wet peak inside the calibration window.



// ─────────────────────────────────────────────────────────────────────────
// 10. Determinism
// ─────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────
// 11. RT allocation
// ─────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────
// Contract checks the acceptance list implies but does not enumerate
// ─────────────────────────────────────────────────────────────────────────
