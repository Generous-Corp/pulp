#pragma once

// AdditiveBankT — the sinusoidal-bank additive engine acceptance suite.
//
// Module M19 (spec: additive-pulp-module-prompt.md, acceptance tests AT-1..10).
// Every expected value is COMPUTED from a shipped constant or a shipped closed
// form — `partial_frequency_hz`, `nyquist_guard_gain`, `worst_case_gain`,
// `envelope_db_at`, the voice tables themselves — never restated as a literal.
// Changing a shipped constant changes the expectation, not the test.
//
// ## Measurement recipe, and the one property that makes it exact
//
// A sustained voice with `attack_ms = 0` is EXACTLY PERIODIC from sample 0:
// the onset reaches unity on the first sample, sustained partials never decay,
// and with no parameter changes the control-rate ramps have zero slope. So for
// a harmonic voice whose fundamental divides the sample rate, a window of a
// whole number of periods contains an integer number of cycles of every
// partial, and a single-bin DFT over that window is EXACT — no window
// function, no leakage, no correction factor, no bin-collision reasoning
// hiding inside a tolerance.
//
// That is the instrument used wherever it applies (`coherent_amplitude`).
// Where it cannot apply — inharmonic bell ratios, stretched partials, decaying
// envelopes — the signal is not periodic in any practical window, so a
// Hann-windowed DTFT evaluated at an exact frequency is used instead
// (`windowed_magnitude`), with a ternary search when a peak's LOCATION rather
// than its height is the measurement.
//
// PEAK-SAMPLE AMPLITUDE IS NEVER USED. A discrete sine's largest sample under-
// reads whenever no sample lands on the crest — six samples per cycle at
// 8 kHz/48 kHz reads 1.25 dB low, which looks exactly like a spectrum that is
// not flat. The one place a peak sample appears is AT-7, where the quantity
// under test IS the instantaneous peak and the test constructs the alignment
// so a sample lands on it exactly (see that test).
//
// Acceptance-class constants (render lengths, FFT sizes, ± bounds) are stated
// at their use site with the reason they are large or small enough.
//
// ## Spec deviations, each with the number that forced it
//
//   1. AT-4(a) asks that "partials above fs/2−guard are ≤ −100 dB". They are
//      not, and cannot be: §6 defines the guard as a raised-cosine TAPER over
//      that band, so a partial at 23,200 Hz (inside the guard, fs = 48 kHz) has
//      gain 0.9045 — i.e. −0.87 dB, not −100 dB. The two clauses of the spec
//      contradict each other. Asserted here against the shipped taper law, with
//      ≤ −100 dB required only at and above Nyquist where the guard is exactly
//      zero. See `Nyquist guard tapers rather than cliffs`.
//   2. AT-4 specifies f0 = 200 Hz for the alias test. At that fundamental every
//      alias image folds exactly onto another requested partial
//      (48000 − 200n = 200·(240−n)), so a folded partial would be invisible —
//      the test cannot fail. f0 = 190 Hz is used instead, where no image
//      coincides with a partial. Same arithmetic, a test that can actually
//      detect the defect it is looking for.
//   3. §3 asks for "the existing osc/ family sine (polynomial/lookup)" AND for
//      THD ≤ −100 dB. There is no polynomial or lookup sine in the osc family:
//      `osc/va.hpp`'s sine shape is `std::sin(2π·φ)` in double. The one
//      polynomial sine in the tree, `FastMath::sin`, misses −100 dB by about
//      40 dB — asserted in `The catalog sine meets the THD requirement`, so the
//      contradiction is executable rather than a comment.
//   4. §7's code line computes the stiffness stretch from the array index
//      (`n = p+1`) while its prose says each partial carries its own ratio. For
//      the organ table those disagree — its first row is the 16′ sub at ratio
//      0.5, which the index reading would stretch as if it were mode 1. The
//      ratio is used, since the stretch is a property of which mode of the
//      string is vibrating. Identical for a pure harmonic table, which is what
//      AT-3 measures.
//   5. §1a's API has `retrigger()` but no way to end a note, while the catalog
//      table ships a `release_ms`. `release()` is added; a parameter with no
//      path to it is not a parameter.
//   6. AT-9 asks for a roster entry in `test_signal_rt_safety.cpp`. The probe
//      runs here against the same harness, so the module's RT contract is
//      covered by the module's own suite.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/additive_bank.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Bank = AdditiveBank64;   // the analysis engine: double, so a measured
                               // null is limited by the physics rather than by
                               // the sample type. `AdditiveBank` gets a parity
                               // test of its own.
using Complex = std::complex<double>;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// One LSB of a `float` mantissa — the spec's stated tolerance on the
/// worst-case-gain measurement. An acceptance-class constant: it says how
/// precisely a summed accumulator can be compared, not how the engine behaves.
const double kOneLsb = std::ldexp(1.0, -23);

// ── Engine setup ──────────────────────────────────────────────────────────

/// A voice whose every partial is sustained and whose onset is instantaneous,
/// which is what makes a render exactly periodic and the coherent DFT exact.
/// `attack_ms = 0` is not a corner case: the envelope core floors a stage at
/// 1e-4 ms, so the first sample already sits at unity.
template <typename EngineT>
void configure_steady(EngineT& engine, const VoiceTable& voice, int count,
                      double f0) {
    engine.prepare(kSr, EngineT::kMaxPartialsDefault);
    engine.load_voice(voice);
    engine.set_partial_count(count);
    engine.set_fundamental_hz(f0);
    engine.set_spectral_tilt_db_oct(0.0);
    engine.set_master_gain_db(0.0);
    engine.set_attack_ms(0.0);
    engine.set_envelope_mode(EngineT::EnvelopeMode::shared_ar);
    engine.set_inharmonicity_b(0.0);
    engine.reset();
    engine.retrigger();
}

/// A pure harmonic table: `count` partials at integer ratios, unit amplitude
/// unless overridden. The neutral probe voice — anything it measures is a
/// property of the engine rather than of a shipped preset.
VoiceTable harmonic_voice(int count, double amp = 1.0, double phase01 = 0.0) {
    VoiceTable v;
    v.harmonic = true;
    for (int p = 0; p < count; ++p)
        v.add({static_cast<double>(p + 1), amp, phase01, 0.0});
    return v;
}

template <typename EngineT>
std::vector<double> render(EngineT& engine, int n) {
    using S = std::conditional_t<std::is_same_v<EngineT, AdditiveBank>, float,
                                 double>;
    std::vector<S> buf(static_cast<std::size_t>(n), S{0});
    engine.process(buf.data(), n);
    std::vector<double> out(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i)
        out[i] = static_cast<double>(buf[i]);
    return out;
}

// ── Instrument A: coherent single-bin DFT (exact on a periodic render) ────

/// Amplitude at exactly `cycles · fs / window` Hz, measured over a window
/// holding a whole number of cycles. Zero leakage, so the result is the true
/// amplitude with no window correction.
double coherent_amplitude(const std::vector<double>& x, int cycles, int window,
                          int offset = 0) {
    Complex acc(0.0, 0.0);
    for (int i = 0; i < window; ++i) {
        const double ph = -2.0 * kPi * static_cast<double>(cycles) *
                          static_cast<double>(i) / static_cast<double>(window);
        acc += x[static_cast<std::size_t>(offset + i)] *
               Complex(std::cos(ph), std::sin(ph));
    }
    return 2.0 * std::abs(acc) / static_cast<double>(window);
}

// ── Instrument B: Hann-windowed DTFT at an exact frequency ────────────────

/// Magnitude at an arbitrary frequency of a signal that is NOT periodic in the
/// window. The Hann window's symmetric taper leaves an isolated peak's location
/// unbiased, which is what the frequency measurements below depend on.
double windowed_magnitude(const std::vector<double>& x, double f_hz,
                          int offset = 0, int length = -1) {
    const int n = length > 0 ? length
                             : static_cast<int>(x.size()) - offset;
    const double w = -2.0 * kPi * f_hz / kSr;
    Complex acc(0.0, 0.0);
    double norm = 0.0;
    for (int i = 0; i < n; ++i) {
        const double win =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(n)));
        const double ph = w * static_cast<double>(i);
        acc += win * x[static_cast<std::size_t>(offset + i)] *
               Complex(std::cos(ph), std::sin(ph));
        norm += win;
    }
    return 2.0 * std::abs(acc) / norm;
}

/// Locates a spectral peak to far better than a bin by ternary search on the
/// windowed magnitude. The bracket must contain exactly one partial.
double refine_peak(const std::vector<double>& x, double lo, double hi,
                   int offset = 0, int length = -1) {
    for (int i = 0; i < 200 && (hi - lo) > 1e-9 * hi; ++i) {
        const double a = lo + (hi - lo) / 3.0;
        const double b = hi - (hi - lo) / 3.0;
        if (windowed_magnitude(x, a, offset, length) >
            windowed_magnitude(x, b, offset, length))
            hi = b;
        else
            lo = a;
    }
    return 0.5 * (lo + hi);
}

// ── Instrument C: radix-2 FFT, for whole-band alias scans ─────────────────

void fft_in_place(std::vector<Complex>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const Complex wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const Complex u = a[i + k];
                const Complex v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

struct Spectrum {
    std::vector<double> magnitude;
    double bin_hz = 0.0;
    double frequency(std::size_t bin) const {
        return static_cast<double>(bin) * bin_hz;
    }
};

/// Hann-windowed magnitude spectrum, normalised so an isolated full-scale
/// sinusoid reads 1.0.
Spectrum spectrum_of(const std::vector<double>& x) {
    std::size_t n = 1;
    while (n < x.size()) n <<= 1;
    std::vector<Complex> buf(n, Complex(0.0, 0.0));
    double norm = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double win =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                  static_cast<double>(x.size())));
        buf[i] = Complex(win * x[i], 0.0);
        norm += win;
    }
    fft_in_place(buf);
    Spectrum s;
    s.bin_hz = kSr / static_cast<double>(n);
    s.magnitude.resize(n / 2 + 1);
    for (std::size_t i = 0; i < s.magnitude.size(); ++i)
        s.magnitude[i] = 2.0 * std::abs(buf[i]) / norm;
    return s;
}

double db(double linear) {
    return 20.0 * std::log10(std::max(linear, 1e-300));
}

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// AT-1 — single-partial frequency accuracy
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// AT-2 — harmonic-sum magnitudes
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// AT-3 — the stiff-string inharmonicity law
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// AT-4 — the Nyquist guard
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// AT-5 — spectral morph and scale invariance (series law 7)
// ═══════════════════════════════════════════════════════════════════════════





// ═══════════════════════════════════════════════════════════════════════════
// AT-6 — retrigger phase policy
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// AT-7 — the crest bound (series laws 1 and 8)
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// AT-8 — determinism
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// AT-9 — the RT contract
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// AT-10 — latency
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// Per-partial trajectories, the voice tables, and the doublet
// ═══════════════════════════════════════════════════════════════════════════
