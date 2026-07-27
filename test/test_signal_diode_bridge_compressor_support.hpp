#pragma once

// The diode-bridge compressor lineage — DiodeBridgeGainT, TransformerBracketT,
// DiodeBridgeCompressorT.
//
// The spec's acceptance suite (module M08). Expected values are computed from
// the shipped calibration tables rather than restated, so a change to a
// curvature constant or a saturation depth fails the test that documents it.
//
// FIVE OF THE SPEC'S CRITERIA ARE NOT MET BY ANY CORRECT IMPLEMENTATION. Each
// is adjudicated at its test with the arithmetic that proves it, and in every
// case the TEST was corrected, never the code:
//
//   A3  — the spec's THD3 formula `β·s²/3` drops the factor of 4 from the
//         `sin³θ = (3sinθ − sin3θ)/4` expansion. The correct ratio is `β·s²/12`,
//         four times smaller, and its ±0.02 %-absolute tolerance band excludes
//         the right answer.
//   A4  — an 8 kHz probe at 48 kHz puts a cubic's only harmonic exactly ON
//         Nyquist, where the naive shaper's alias is identically zero. ADAA
//         cannot be 18 dB better than zero.
//   A7  — an "instantaneous |output| ≤ |input| × bound" invariant is
//         unachievable for any signal path containing a filter: after an
//         impulse the input is zero while the output still rings.
//   A8  — `BallisticsFilterT`'s time constants are 10–90 % RISE TIMES (its
//         coefficient carries `ln 9`), so its 63 % point is at `time/ln 9`,
//         45.5 % of nominal. "Within ±15 % of attack_ms/release_ms" is off by
//         a factor of 2.2 before the gain computer's dB-domain mapping is even
//         considered.
//   A9  — the criterion is ratio-dependent. Sidechain attenuation reaches the
//         gain reduction multiplied by `(1 − 1/ratio)`, so 5.7 dB of 60 Hz
//         rejection is only 1.9 dB of reduction difference at ratio 1.5. The
//         spec's rationale compares the two quantities as if they were one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/diode_bridge_compressor.hpp>
#include <pulp/signal/junction.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSr = 48000.0;

using Bridge = DiodeBridgeGainT<double>;
using Bracket = TransformerBracketT<double>;
using Comp = DiodeBridgeCompressorT<double>;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// Coherent DFT magnitude at harmonic `k` of a tone whose period divides the
/// analysis window exactly — leakage-free, so no window and no correction.
double harmonic_magnitude(const std::vector<double>& x, double fundamental_hz, int k) {
    const double w = 2.0 * M_PI * k * fundamental_hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

/// 1 kHz at 48 kHz is exactly 48 samples per period, so any whole number of
/// periods fills the window.
constexpr double kToneHz = 1000.0;
constexpr int kTonePeriod = 48;

Comp make_compressor() {
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-12.0);
    c.set_ratio(4.0);
    c.set_knee_db(6.0);
    c.set_attack_ms(3.0);
    c.set_release_ms(400.0);
    c.set_character(0.0);
    c.set_makeup_db(0.0);
    c.set_feedback(false);
    c.reset();
    return c;
}

/// Settled peak of a sine of the given amplitude through a configured node.
double settled_peak(Comp& c, double amplitude, double seconds, double tone_hz = kToneHz) {
    const int total = static_cast<int>(kSr * seconds);
    const int window = static_cast<int>(kSr * 0.2);
    double peak = 0.0;
    for (int n = 0; n < total; ++n) {
        const double y = c.process(amplitude * std::sin(2.0 * M_PI * tone_hz * n / kSr));
        if (n >= total - window) peak = std::max(peak, std::abs(y));
    }
    return peak;
}

}  // namespace

// ── A1. Gain-law accuracy ─────────────────────────────────────────────────




// ── The junction composition ──────────────────────────────────────────────




// ── A2. Static compression curve ──────────────────────────────────────────



// ── A3. Third-harmonic colour ─────────────────────────────────────────────




// ── A4. Anti-aliasing ─────────────────────────────────────────────────────


// ── A5. Determinism ───────────────────────────────────────────────────────


// ── A6. Latency ───────────────────────────────────────────────────────────


// ── A7. Worst-case gain bound ─────────────────────────────────────────────




// ── A8. Ballistics ────────────────────────────────────────────────────────


namespace {

/// Measures the node's gain-reduction 63 % points across a −20 dBFS → 0 dBFS
/// step and back, returning milliseconds.
struct StepResponse {
    double attack_ms = 0.0;
    double release_ms = 0.0;
    double steady_reduction_db = 0.0;
};

StepResponse measure_step(double attack_ms, double release_ms, bool feedback) {
    Comp c = make_compressor();
    c.set_knee_db(0.0);
    c.set_attack_ms(attack_ms);
    c.set_release_ms(release_ms);
    c.set_feedback(feedback);
    c.reset();

    const double quiet = units::db_to_linear(-20.0);
    const int pre = static_cast<int>(kSr * 1.0);
    const int hold = static_cast<int>(kSr * 2.0);
    const int post = static_cast<int>(kSr * 6.0);

    std::vector<double> reduction;
    reduction.reserve(static_cast<std::size_t>(pre + hold + post));
    for (int n = 0; n < pre + hold + post; ++n) {
        const double amplitude = (n < pre) ? quiet : (n < pre + hold ? 1.0 : quiet);
        c.process(amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr));
        reduction.push_back(-c.gain_reduction_db());
    }

    const double before = reduction[static_cast<std::size_t>(pre - 1)];
    const double loud = reduction[static_cast<std::size_t>(pre + hold - 1)];
    const double recovered = reduction.back();

    int attack_index = pre;
    const double attack_target = before + 0.63212 * (loud - before);
    while (attack_index < pre + hold &&
           reduction[static_cast<std::size_t>(attack_index)] < attack_target)
        ++attack_index;

    int release_index = pre + hold;
    const double release_target = loud + 0.63212 * (recovered - loud);
    while (release_index < static_cast<int>(reduction.size()) &&
           reduction[static_cast<std::size_t>(release_index)] > release_target)
        ++release_index;

    return {1000.0 * (attack_index - pre) / kSr,
            1000.0 * (release_index - pre - hold) / kSr, loud};
}

}  // namespace







// ── A9. Sidechain high-pass ───────────────────────────────────────────────


// ── The transformer brackets ──────────────────────────────────────────────





// ── A10. RT allocation ────────────────────────────────────────────────────


// ── A11. float / double parity ────────────────────────────────────────────


// ── Lifecycle ─────────────────────────────────────────────────────────────
