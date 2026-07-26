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

TEST_CASE("A single partial sits at exactly the requested frequency",
          "[signal][additive][frequency]") {
    // 440 Hz at 48 kHz is 11 cycles per 1200 samples exactly, so a window of
    // 1200·k samples holds a whole number of cycles and the coherent DFT is
    // exact. 40 such blocks is 1 s of analysis.
    constexpr int kWindow = 48000;
    constexpr int kCycles = 440;

    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, 440.0);
    // One sample past the analysis window, so the zero-crossing count below
    // sees the crossing that spans the wrap. Counting sign changes over N
    // samples examines N−1 adjacent pairs, which misses exactly one of the 2·k
    // crossings a k-cycle window contains — an off-by-one in the RULER, not in
    // the signal. The DFT still reads exactly `kWindow` samples.
    const auto x = render(bank, kWindow + 1);

    // Every joule of the signal is at 440 Hz. A far sharper frequency
    // statement than reading a peak: an error of 0.01 % would leave the fitted
    // 440 Hz component unable to account for the energy.
    const double amp = coherent_amplitude(x, kCycles, kWindow);
    double total = 0.0;
    for (int i = 0; i < kWindow; ++i) {
        const double v = x[static_cast<std::size_t>(i)];
        total += v * v;
    }
    const double explained = 0.5 * amp * amp * static_cast<double>(kWindow);
    REQUIRE_THAT(explained / total, WithinAbs(1.0, 1e-9));

    // The spec's corroboration: a zero-crossing count over the same window.
    //
    // Asserted to +/-1, and that is the exact right tolerance rather than
    // slack. A 440-cycle window contains 880 crossings, but its endpoint sits
    // exactly on one of them: 81 of these samples land on a zero of the sine,
    // and at those samples the sign is decided by the last bit of the
    // accumulated phase rather than by the frequency. Choosing a phase offset
    // does not escape it — any rational offset still puts samples on zeros,
    // because the period divides the window by construction. So a crossing
    // count over a finite window is a +/-1 measurement, full stop. The exact
    // frequency statement is the energy test above, which is already tight to
    // 1e-9; this is the independent corroboration the spec asks for.
    int crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        if ((x[i - 1] < 0.0) != (x[i] < 0.0)) ++crossings;
    REQUIRE(std::abs(crossings - 2 * kCycles) <= 1);

    // And no harmonic content: the bank is a sine, not a shape.
    for (int h = 2; h <= 6; ++h)
        REQUIRE(db(coherent_amplitude(x, kCycles * h, kWindow) / amp) < -100.0);
}

TEST_CASE("One partial is bit-identical to a catalog sine oscillator",
          "[signal][additive][frequency]") {
    // The header claims a single partial IS the catalog's sine rather than a
    // lookalike. That is a bit-exactness claim, so it gets a bit-exact test
    // against `osc::VaOscillator` — including the phase CONVENTION, which is
    // the part that is easy to get wrong and invisible in a spectrum: both
    // evaluate at the entry phase and advance afterwards, so sample 0 is the
    // stored phase itself and not one increment past it.
    constexpr double kF0 = 440.0;
    constexpr double kPhase = 0.25;
    constexpr int kN = 4096;

    Bank bank;
    // Unity all the way through: one partial at amplitude 1 leaves the
    // normaliser inert (its sum is exactly 1), and 0 dB master, 0 tilt and a
    // flat envelope leave nothing else to scale it.
    configure_steady(bank, harmonic_voice(1, 1.0, kPhase), 1, kF0);
    const auto x = render(bank, kN);

    osc::VaOscillator reference;
    reference.set_shape(osc::VaShape::sine);
    reference.reset(kPhase);
    const double increment = kF0 / kSr;

    for (int i = 0; i < kN; ++i)
        REQUIRE(x[static_cast<std::size_t>(i)] == reference.next(increment));
}

TEST_CASE("The catalog sine meets the THD requirement and FastMath does not",
          "[signal][additive][frequency][spec-defect]") {
    // SPEC DEFECT, with the number. Section 3 asks for "the existing osc/
    // family sine (polynomial/lookup)" and, in the same paragraph, for a single
    // evaluated sine to hold THD <= -100 dB. Those cannot both be satisfied:
    // there IS no polynomial or lookup sine in the osc family — `osc/va.hpp`'s
    // sine shape is `std::sin(2*pi*phi)` in double, which is what this engine
    // composes — and the tree's one polynomial sine, `FastMath::sin`, is a
    // Bhaskara-style approximation about 40 dB short of the requirement.
    constexpr int kWindow = 48000;
    constexpr int kCycles = 440;

    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, 440.0);
    const auto x = render(bank, kWindow);

    const double fundamental = coherent_amplitude(x, kCycles, kWindow);
    double shipped_thd = 0.0;
    for (int h = 2; h <= 20; ++h) {
        const double a = coherent_amplitude(x, kCycles * h, kWindow);
        shipped_thd += a * a;
    }
    shipped_thd = std::sqrt(shipped_thd) / fundamental;
    REQUIRE(db(shipped_thd) < -100.0);

    // The same measurement on the fast approximation, at the same frequency.
    std::vector<double> fast(static_cast<std::size_t>(kWindow));
    for (int i = 0; i < kWindow; ++i) {
        const double ph = static_cast<double>(kCycles) *
                          static_cast<double>(i) / static_cast<double>(kWindow);
        fast[static_cast<std::size_t>(i)] = static_cast<double>(
            FastMath::sin(static_cast<float>(2.0 * kPi * ph)));
    }
    const double fast_fundamental = coherent_amplitude(fast, kCycles, kWindow);
    double fast_thd = 0.0;
    for (int h = 2; h <= 20; ++h) {
        const double a = coherent_amplitude(fast, kCycles * h, kWindow);
        fast_thd += a * a;
    }
    fast_thd = std::sqrt(fast_thd) / fast_fundamental;

    REQUIRE(db(fast_thd) > -100.0);
    // And it is not marginal — it misses by more than 30 dB, which across a
    // 64-partial sum is exactly the accumulated harmonic error the requirement
    // exists to prevent.
    REQUIRE(db(fast_thd) - db(shipped_thd) > 30.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-2 — harmonic-sum magnitudes
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Partial magnitudes match the voice table",
          "[signal][additive][spectrum]") {
    constexpr int kWindow = 48000;
    constexpr double kF0 = 200.0;   // divides fs, so every harmonic is coherent
    const double amps[4] = {1.0, 0.5, 0.25, 0.125};

    VoiceTable v;
    v.harmonic = true;
    for (int p = 0; p < 4; ++p)
        v.add({static_cast<double>(p + 1), amps[p], 0.0, 0.0});

    Bank bank;
    configure_steady(bank, v, 4, kF0);
    const auto x = render(bank, kWindow);

    // The normaliser scales every partial by one common factor, so the RATIOS
    // are what the table promises regardless of whether it engaged. Asserting
    // ratios rather than absolutes is also what makes this test independent of
    // the crest bound, which has its own test.
    const double first = coherent_amplitude(x, static_cast<int>(kF0), kWindow);
    for (int p = 0; p < 4; ++p) {
        const double a = coherent_amplitude(
            x, static_cast<int>(kF0) * (p + 1), kWindow);
        REQUIRE_THAT(db(a / first), WithinAbs(db(amps[p] / amps[0]), 0.1));
    }

    // And the absolute level is the table amplitude times the shipped
    // normaliser — computed here from the same sum the engine forms, so a
    // change to the normaliser law fails this rather than sliding past it.
    double magnitude_sum = 0.0;
    for (double a : amps) magnitude_sum += a;
    const double expected_norm = 1.0 / std::max(1.0, magnitude_sum);
    REQUIRE_THAT(first, WithinRel(amps[0] * expected_norm, 1e-6));

    // No energy anywhere else. Checked at frequencies that are coherent in this
    // window but are not partials, so a leak would have nowhere to hide.
    for (int hz : {100, 150, 250, 333, 700, 1100, 4000})
        REQUIRE(db(coherent_amplitude(x, hz, kWindow) / first) < -100.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-3 — the stiff-string inharmonicity law
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Partials land where the stiff-string law puts them",
          "[signal][additive][inharmonicity]") {
    // The law is published (Fletcher, Blackham & Stratton 1962):
    // f_n = n*f0*sqrt(1 + B*n^2). The expectation is computed from the shipped
    // form, and the shipped form is separately checked against the spec's own
    // worked number as external ground truth.
    constexpr double kF0 = 220.0;
    const double kB = Bank::kBPianoMid;

    // External ground truth: the spec's section 7 worked check.
    REQUIRE_THAT(Bank::partial_frequency_hz(kF0, 8.0, kB),
                 WithinAbs(1804.5, 0.05));
    // ...and its 2.53 % stretch claim at the eighth partial.
    REQUIRE_THAT(Bank::partial_frequency_hz(kF0, 8.0, kB) / (8.0 * kF0) - 1.0,
                 WithinAbs(0.0253, 5e-5));

    Bank bank;
    configure_steady(bank, harmonic_voice(12), 12, kF0);
    bank.set_inharmonicity_b(kB);
    bank.reset();
    bank.retrigger();

    // 2^17 samples is 2.7 s — long enough that the Hann main lobe is ~0.7 Hz
    // wide, orders finer than the 0.05 % asserted below.
    const auto x = render(bank, 1 << 17);

    for (int n = 1; n <= 12; ++n) {
        const double predicted = Bank::partial_frequency_hz(
            kF0, static_cast<double>(n), kB);
        // A +/-2 % bracket cannot reach a neighbour: consecutive partials are
        // a whole f0 apart, which is at least 12 % of any of these frequencies.
        const double measured =
            refine_peak(x, predicted * 0.98, predicted * 1.02);
        REQUIRE_THAT(measured, WithinRel(predicted, 5e-4));
        // Sharp of the pure harmonic, increasingly so with n — the stretch
        // that makes struck strings shimmer.
        REQUIRE(measured > static_cast<double>(n) * kF0);
    }

    // B = 0 is exactly the harmonic series, not approximately.
    for (int n = 1; n <= 12; ++n)
        REQUIRE_THAT(Bank::partial_frequency_hz(kF0, n, 0.0),
                     WithinRel(static_cast<double>(n) * kF0, 1e-15));

    // The stretch is monotone in B across the whole legal range, including the
    // uncited headroom past the published band.
    double previous = 0.0;
    for (double b : {0.0, Bank::kBPianoTenor, Bank::kBPianoMid,
                     Bank::kBUpperTreble, Bank::kBElectricPianoTine,
                     Bank::kInharmonicityMax}) {
        const double f = Bank::partial_frequency_hz(kF0, 8.0, b);
        REQUIRE(f > previous);
        previous = f;
    }
    REQUIRE(Bank::kBElectricPianoTine == Bank::kInharmonicityCitedMax);
    REQUIRE(Bank::kInharmonicityMax > Bank::kInharmonicityCitedMax);
}

TEST_CASE("The stiffness stretch is keyed to the mode ratio not the row index",
          "[signal][additive][inharmonicity][spec-defect]") {
    // SPEC DEFECT, and a decision this test exists to pin. Section 7's prose
    // says "a general voice table each partial carries its own ratio_p", but
    // the code line beside it computes the stretch from the array index
    // (`n = p+1`). For a pure harmonic table those are the same number, which
    // is why AT-3 cannot tell them apart — and why a wrong choice would ship.
    //
    // They differ for any table whose ratios are not 1, 2, 3, ... The organ's
    // first row is the 16' sub at ratio 0.5: the index reading stretches it as
    // if it were mode 1, which is 0.7 % sharp of where a half-mode actually
    // sits. The ratio is the mode number of the string that is vibrating, so
    // the ratio is what the physics depends on.
    constexpr double kF0 = 400.0;
    const double kB = Bank::kBElectricPianoTine;   // large enough to resolve

    VoiceTable v;
    v.harmonic = true;
    for (double ratio : {0.5, 1.0, 1.5, 2.0}) v.add({ratio, 1.0, 0.0, 0.0});

    Bank bank;
    configure_steady(bank, v, 4, kF0);
    bank.set_inharmonicity_b(kB);
    bank.reset();

    for (int p = 0; p < 4; ++p) {
        const double ratio = v.partials[static_cast<std::size_t>(p)].ratio;
        const double by_ratio = Bank::partial_frequency_hz(kF0, ratio, kB);
        REQUIRE_THAT(bank.partial_frequency(p), WithinRel(by_ratio, 1e-9));
    }

    // The two readings genuinely disagree here, so the assertion above is a
    // choice being tested rather than a tautology.
    const double by_index =
        kF0 * 0.5 * std::sqrt(1.0 + kB * 1.0 * 1.0);   // n = p+1 = 1
    const double by_ratio = Bank::partial_frequency_hz(kF0, 0.5, kB);
    REQUIRE(std::abs(by_index - by_ratio) / by_ratio > 0.005);
    REQUIRE(bank.partial_frequency(0) < by_index);

    // And it is audible in the render, not only in the getter. `reset()` leaves
    // the onset idle, so the retrigger is what makes this a signal rather than
    // 2.7 seconds of silence that any peak search would happily wander through.
    bank.retrigger();
    const auto x = render(bank, 1 << 17);
    double energy = 0.0;
    for (double v : x) energy += v * v;
    REQUIRE(energy > 0.0);
    const double measured =
        refine_peak(x, by_ratio * 0.99, by_ratio * 1.01);
    REQUIRE_THAT(measured, WithinRel(by_ratio, 1e-3));
}

TEST_CASE("Inharmonicity applies to harmonic voices and not to modal ones",
          "[signal][additive][inharmonicity]") {
    // The decision from section 7: a bell's ratios ARE the measurement of a
    // casting's modes, so stretching them with a string's stiffness law would
    // be applying the wrong physics to the wrong object.
    Bank harmonic_bank;
    configure_steady(harmonic_bank, harmonic_voice(8), 8, 220.0);
    const double before = harmonic_bank.partial_frequency(7);
    harmonic_bank.set_inharmonicity_b(Bank::kBUpperTreble);
    harmonic_bank.reset();
    REQUIRE(harmonic_bank.partial_frequency(7) > before);

    Bank modal_bank;
    configure_steady(modal_bank, make_bell_voice(16), 16, 220.0);
    const double bell_before = modal_bank.partial_frequency(7);
    modal_bank.set_inharmonicity_b(Bank::kInharmonicityMax);
    modal_bank.reset();
    REQUIRE_THAT(modal_bank.partial_frequency(7), WithinRel(bell_before, 1e-12));
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-4 — the Nyquist guard
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Nyquist guard tapers rather than cliffs",
          "[signal][additive][nyquist][spec-defect]") {
    // SPEC DEFECT, with the number. AT-4(a) asks that "partials above
    // fs/2 - guard are <= -100 dB". Section 6 defines that same band as a
    // raised-cosine TAPER to zero at Nyquist, so a partial just inside it is
    // barely attenuated: at fs = 48 kHz with the shipped 1000 Hz guard, a
    // partial at 23,200 Hz has gain 0.9045, i.e. -0.87 dB. The two clauses
    // contradict each other, and the taper is the one that matters — a hard
    // cliff is what CLICKS when a glide walks a partial through it, which
    // AT-4(c) separately forbids.
    const double nyquist = 0.5 * kSr;
    const double taper_start = nyquist - Bank::kNyquistGuardBandHz;

    // Unity below the band, exactly zero at and above Nyquist, monotone across.
    REQUIRE_THAT(Bank::nyquist_guard_gain(taper_start, kSr),
                 WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(Bank::nyquist_guard_gain(nyquist, kSr), WithinAbs(0.0, 1e-12));
    REQUIRE(Bank::nyquist_guard_gain(nyquist + 1.0, kSr) == 0.0);
    REQUIRE(Bank::nyquist_guard_gain(nyquist * 4.0, kSr) == 0.0);

    // The midpoint of a raised cosine is exactly one half — the property that
    // distinguishes it from a linear fade or a cliff.
    REQUIRE_THAT(
        Bank::nyquist_guard_gain(taper_start + 0.5 * Bank::kNyquistGuardBandHz,
                                 kSr),
        WithinAbs(0.5, 1e-12));

    double previous = 1.0;
    for (double f = taper_start; f <= nyquist; f += 25.0) {
        const double g = Bank::nyquist_guard_gain(f, kSr);
        REQUIRE(g <= previous + 1e-15);
        previous = g;
    }

    // And this is the spec's own arithmetic, showing the criterion is
    // unmeetable: a partial inside the guard is nowhere near -100 dB.
    REQUIRE(db(Bank::nyquist_guard_gain(23200.0, kSr)) > -1.0);
}

TEST_CASE("Partials past Nyquist are muted rather than folded",
          "[signal][additive][nyquist]") {
    // The spec's f0 = 200 Hz cannot detect a fold: at that fundamental every
    // image lands on another requested partial (48000 - 200n = 200*(240-n)), so
    // a folded partial would be indistinguishable from a legitimate one and the
    // test could not fail. 190 Hz has no such coincidence.
    constexpr double kF0 = 190.0;
    constexpr int kPartials = 128;

    Bank bank;
    configure_steady(bank, harmonic_voice(kPartials), kPartials, kF0);
    const auto x = render(bank, 1 << 17);

    const double nyquist = 0.5 * kSr;
    const double reference = windowed_magnitude(x, kF0);

    int muted = 0, tapered = 0;
    for (int n = 1; n <= kPartials; ++n) {
        const double f = static_cast<double>(n) * kF0;
        const double guard = Bank::nyquist_guard_gain(f, kSr);

        if (guard == 0.0) {
            ++muted;
            // Muted, and its FOLD IMAGE is silent too — which is the actual
            // claim. A partial that was merely quiet at its own frequency but
            // folding at fs-f would pass a "muted" check and still alias.
            const double image = kSr - f;
            if (image > 20.0 && image < nyquist)
                REQUIRE(db(windowed_magnitude(x, image) / reference) < -100.0);
        } else if (guard < 1.0) {
            ++tapered;
        }
    }
    // The stated conditions really do push partials past Nyquist, so the test
    // is exercising the guard rather than describing a band it never reaches.
    REQUIRE(muted > 0);
    REQUIRE(tapered > 0);

    // Whole-band sweep: every peak that rises above the floor is a requested,
    // unmuted partial. This is the general form of "no aliased energy" and
    // needs no bookkeeping about where an image would land.
    const auto s = spectrum_of(x);
    const double floor_db = -80.0;
    for (std::size_t bin = 1; bin + 1 < s.magnitude.size(); ++bin) {
        if (s.magnitude[bin] <= s.magnitude[bin - 1] ||
            s.magnitude[bin] < s.magnitude[bin + 1])
            continue;
        if (db(s.magnitude[bin] / reference) < floor_db) continue;

        const double f = s.frequency(bin);
        const double n = f / kF0;
        const double nearest = std::round(n);
        REQUIRE(std::abs(n - nearest) < 0.02);
        REQUIRE(nearest >= 1.0);
        REQUIRE(nearest <= static_cast<double>(kPartials));
        REQUIRE(Bank::nyquist_guard_gain(nearest * kF0, kSr) > 0.0);
    }
}

TEST_CASE("A partial gliding through the guard does not click",
          "[signal][additive][nyquist]") {
    // AT-4(c). The guard's whole reason for being a taper rather than a switch.
    // Measured as the largest sample-to-sample step, compared against the same
    // voice held still — a mute that stepped would show up as a step far larger
    // than the signal's own slew.
    constexpr int kPartials = 64;
    constexpr int kBlocks = 400;
    constexpr int kBlock = 64;

    const auto max_step = [](const std::vector<double>& x) {
        double m = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i)
            m = std::max(m, std::abs(x[i] - x[i - 1]));
        return m;
    };

    // Held still at the top of the sweep.
    Bank steady;
    configure_steady(steady, harmonic_voice(kPartials), kPartials, 380.0);
    const double still = max_step(render(steady, kBlocks * kBlock));

    // Gliding f0 upward so partials walk through the guard and out past
    // Nyquist one after another.
    Bank sweeping;
    configure_steady(sweeping, harmonic_voice(kPartials), kPartials, 340.0);
    std::vector<double> swept;
    swept.reserve(static_cast<std::size_t>(kBlocks * kBlock));
    for (int b = 0; b < kBlocks; ++b) {
        const double t = static_cast<double>(b) / (kBlocks - 1);
        sweeping.set_fundamental_hz(340.0 + t * 40.0);
        const auto chunk = render(sweeping, kBlock);
        swept.insert(swept.end(), chunk.begin(), chunk.end());
    }

    REQUIRE(max_step(swept) < 1.5 * still);
    for (double v : swept) REQUIRE(std::isfinite(v));
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-5 — spectral morph and scale invariance (series law 7)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Morph is a dB-domain crossfade of the two envelopes",
          "[signal][additive][morph]") {
    constexpr int kWindow = 48000;
    constexpr double kF0 = 200.0;
    constexpr int kPartials = 8;
    constexpr double kSlopeDbOct = -6.0;

    const auto envelope_a = SpectralEnvelope::tilt(0.0, kF0);
    const auto envelope_b = SpectralEnvelope::tilt(kSlopeDbOct, kF0);

    for (double m : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, kF0);
        bank.set_envelope_a(envelope_a);
        bank.set_envelope_b(envelope_b);
        bank.set_morph(static_cast<float>(m));
        bank.reset();
        bank.retrigger();
        const auto x = render(bank, kWindow);

        const double first = coherent_amplitude(x, static_cast<int>(kF0), kWindow);
        for (int n = 1; n <= kPartials; ++n) {
            const double a = coherent_amplitude(
                x, static_cast<int>(kF0) * n, kWindow);
            // The dB crossfade of a flat envelope and a slope is that slope
            // scaled by m, so partial n sits m*slope*log2(n) below the first.
            const double expected_db =
                m * kSlopeDbOct * std::log2(static_cast<double>(n));
            REQUIRE_THAT(db(a / first), WithinAbs(expected_db, 0.05));
        }
    }

    // The morph really is in dB, not in linear amplitude. At the midpoint the
    // two differ by 1.25 dB on a partial 10 dB down — small, but it is the
    // difference between a timbre that travels evenly and one that lurches.
    Bank probe;
    configure_steady(probe, harmonic_voice(kPartials), kPartials, kF0);
    probe.set_envelope_a(envelope_a);
    probe.set_envelope_b(envelope_b);
    probe.set_morph(0.5f);
    const double db_midpoint = 0.5 * kSlopeDbOct * std::log2(4.0);
    const double linear_midpoint =
        db(0.5 * (1.0 + units::db_to_linear(kSlopeDbOct * std::log2(4.0))));
    REQUIRE(std::abs(db_midpoint - linear_midpoint) > 1.0);
    REQUIRE_THAT(probe.envelope_db_at(kF0 * 4.0),
                 WithinAbs(db_midpoint, 1e-9));
}

TEST_CASE("A tilt envelope morphs identically at every fundamental",
          "[signal][additive][morph][scale-invariance]") {
    // Series law 7, in its strict form: the SAME morph at two fundamentals two
    // octaves and a bit apart must produce the same RELATIVE spectrum. A tilt
    // is affine in log2 frequency, which is exactly the class of envelope for
    // which that holds in the absolute-Hz domain.
    constexpr int kWindow = 48000;
    constexpr int kPartials = 8;
    constexpr double kSlopeDbOct = -6.0;

    const auto relative_spectrum = [&](double f0) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, f0);
        bank.set_envelope_a(SpectralEnvelope::tilt(0.0, 100.0));
        bank.set_envelope_b(SpectralEnvelope::tilt(kSlopeDbOct, 100.0));
        bank.set_morph(0.5f);
        bank.reset();
        bank.retrigger();
        const auto x = render(bank, kWindow);

        std::vector<double> rel;
        const double first =
            coherent_amplitude(x, static_cast<int>(f0), kWindow);
        for (int n = 1; n <= kPartials; ++n)
            rel.push_back(db(coherent_amplitude(
                                 x, static_cast<int>(f0) * n, kWindow) /
                             first));
        return rel;
    };

    // 110 and 880 are three octaves apart, and both divide 48 kHz into a whole
    // number of cycles per analysis window.
    const auto low = relative_spectrum(110.0);
    const auto high = relative_spectrum(880.0);
    REQUIRE(low.size() == high.size());
    for (std::size_t i = 0; i < low.size(); ++i)
        REQUIRE_THAT(low[i], WithinAbs(high[i], 0.05));

    // The tilt CONTROL is scale-invariant by construction, being measured
    // against log2(f_p/f0) rather than against absolute frequency.
    for (double f0 : {110.0, 880.0}) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, f0);
        bank.set_spectral_tilt_db_oct(kSlopeDbOct);
        for (int n = 1; n <= kPartials; ++n)
            REQUIRE_THAT(bank.envelope_db_at(f0 * n),
                         WithinAbs(kSlopeDbOct * std::log2(static_cast<double>(n)),
                                   1e-9));
    }
}

TEST_CASE("A formant envelope stays put in Hz and follows pitch on request",
          "[signal][additive][morph][scale-invariance][spec-defect]") {
    // SPEC DEFECT, resolved rather than papered over. Section 8 claims a
    // sampled spectral envelope makes the morph scale-invariant, and section
    // 10c's V6 says the resulting vowel "tracks pitch, unlike a fixed filter
    // bank". Those are contradictory: a FORMANT is a resonance of a fixed body
    // and does NOT move when the singer changes note — that is what makes an
    // "ah" an "ah" at every pitch. An envelope over absolute Hz cannot both
    // stay put and track pitch.
    //
    // The engine answers both by naming the abscissa. `absolute_hz` is the
    // Rodet & Depalle reading and gives real formants; `relative_to_f0` gives
    // the fully scale-invariant gesture. Only the second is pitch-independent
    // for a general envelope, and this test pins both behaviours so neither can
    // drift into the other.
    constexpr int kPartials = 8;

    SpectralEnvelope bump;   // a formant peak at 700 Hz
    bump.add(100.0, -12.0);
    bump.add(700.0, 0.0);
    bump.add(5000.0, -12.0);

    const auto relative_shape = [&](double f0, SpectralDomain domain) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, f0);
        bank.set_spectral_domain(domain);
        bank.set_envelope_a(bump);
        bank.set_morph(0.0f);
        std::vector<double> rel;
        const double first = bank.envelope_db_at(f0);
        for (int n = 1; n <= kPartials; ++n)
            rel.push_back(bank.envelope_db_at(f0 * n) - first);
        return rel;
    };

    // Absolute domain: the formant is anchored in Hz, so the relative spectrum
    // MUST change with pitch. That is the feature, not a failure.
    const auto abs_low = relative_shape(150.0, SpectralDomain::absolute_hz);
    const auto abs_high = relative_shape(600.0, SpectralDomain::absolute_hz);
    double worst = 0.0;
    for (std::size_t i = 0; i < abs_low.size(); ++i)
        worst = std::max(worst, std::abs(abs_low[i] - abs_high[i]));
    REQUIRE(worst > 6.0);

    // Relative domain: the same envelope, now anchored to the fundamental, is
    // pitch-independent for ANY shape rather than only for affine ones.
    const auto rel_low = relative_shape(150.0, SpectralDomain::relative_to_f0);
    const auto rel_high = relative_shape(600.0, SpectralDomain::relative_to_f0);
    for (std::size_t i = 0; i < rel_low.size(); ++i)
        REQUIRE_THAT(rel_low[i], WithinAbs(rel_high[i], 1e-9));
}

TEST_CASE("The spectral envelope interpolates in log frequency and dB",
          "[signal][additive][morph]") {
    SpectralEnvelope e;
    REQUIRE(e.size() == 0);
    // An empty envelope is the identity, so a default-constructed one changes
    // nothing rather than silencing everything.
    REQUIRE_THAT(e.gain_db_at(1000.0), WithinAbs(0.0, 1e-15));

    REQUIRE(e.add(100.0, 0.0));
    REQUIRE(e.add(400.0, -12.0));
    REQUIRE(e.size() == 2);

    // The geometric midpoint of 100 and 400 is 200, and linear-in-log2 puts it
    // at the arithmetic midpoint in dB. Interpolating in linear frequency would
    // put -12 dB's halfway point at 250 Hz and read -8.7 dB here instead.
    REQUIRE_THAT(e.gain_db_at(200.0), WithinAbs(-6.0, 1e-12));
    REQUIRE_THAT(e.gain_db_at(100.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(e.gain_db_at(400.0), WithinAbs(-12.0, 1e-12));

    // Endpoints are HELD outside the span, not extrapolated — extrapolating a
    // slope past the last break-point is how an envelope quietly produces
    // +40 dB at the top of a 128-partial bank.
    REQUIRE_THAT(e.gain_db_at(10.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(e.gain_db_at(20000.0), WithinAbs(-12.0, 1e-12));

    // Ascending order is a precondition of the search, so a violation is
    // rejected and reported rather than silently corrupting it.
    REQUIRE_FALSE(e.add(200.0, 0.0));
    REQUIRE_FALSE(e.add(400.0, 0.0));
    REQUIRE_FALSE(e.add(-5.0, 0.0));
    REQUIRE(e.size() == 2);

    // Capacity is a hard bound, not a hint.
    SpectralEnvelope full;
    for (int i = 0; i < SpectralEnvelope::kMaxBreakpoints; ++i)
        REQUIRE(full.add(20.0 + static_cast<double>(i), 0.0));
    REQUIRE_FALSE(full.add(100000.0, 0.0));
    REQUIRE(full.size() == SpectralEnvelope::kMaxBreakpoints);

    // The shipped tilt helper really is a constant slope per octave.
    const auto tilt = SpectralEnvelope::tilt(-6.0, 100.0);
    REQUIRE_THAT(tilt.gain_db_at(100.0), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(tilt.gain_db_at(200.0), WithinAbs(-6.0, 1e-9));
    REQUIRE_THAT(tilt.gain_db_at(800.0), WithinAbs(-18.0, 1e-9));
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-6 — retrigger phase policy
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Every retrigger policy is deterministic across reset",
          "[signal][additive][retrigger][determinism]") {
    const auto attack_render = [](Bank::RetrigPhase policy, int strikes,
                                  double attack_ms = 0.0) {
        Bank bank;
        configure_steady(bank, make_bell_voice(32), 32, 220.0);
        bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
        bank.set_attack_ms(attack_ms);
        bank.set_retrig_phase(policy);
        bank.reset();
        std::vector<double> out;
        for (int s = 0; s < strikes; ++s) {
            bank.retrigger();
            const auto chunk = render(bank, 2048);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    };

    for (auto policy : {Bank::RetrigPhase::reset_stored,
                        Bank::RetrigPhase::free_run,
                        Bank::RetrigPhase::seeded_random}) {
        // Series law 2: renders separated by a `reset()` are BIT-identical, not
        // merely close. The seeded policy included — its generator is rewound
        // by `reset()`, so the "random" attack is reproducible.
        const auto a = attack_render(policy, 3, 1.0);
        const auto b = attack_render(policy, 3, 1.0);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
    }

    // `reset_stored` additionally makes every STRIKE identical to the first —
    // the property the default exists for. Measured with a zero-length attack,
    // which is what ISOLATES the phase policy: see the envelope-continuation
    // test below for why a non-zero attack would make the second strike differ
    // for a reason that has nothing to do with phase.
    {
        const auto x = attack_render(Bank::RetrigPhase::reset_stored, 3);
        for (std::size_t i = 0; i < 2048; ++i) {
            REQUIRE(x[i] == x[i + 2048]);
            REQUIRE(x[i] == x[i + 4096]);
        }
    }

    // `seeded_random` scatters the phases, so its second strike differs from
    // its first. Without this the policy would be reproducible AND inert.
    {
        const auto x = attack_render(Bank::RetrigPhase::seeded_random, 3);
        bool differs = false;
        for (std::size_t i = 0; i < 2048; ++i)
            if (x[i] != x[i + 2048]) differs = true;
        REQUIRE(differs);
    }

    // `free_run` leaves the phases running, so its second strike also differs —
    // the softer, already-sounding re-entry the policy is for.
    {
        const auto x = attack_render(Bank::RetrigPhase::free_run, 3);
        bool differs = false;
        for (std::size_t i = 0; i < 2048; ++i)
            if (x[i] != x[i + 2048]) differs = true;
        REQUIRE(differs);
    }
}

TEST_CASE("Retriggering mid-note continues the onset rather than restarting it",
          "[signal][additive][retrigger][spec-defect]") {
    // SPEC DEFECT, or at least a conflation worth pinning. Section 9 says
    // `reset_stored` makes "the attack bit-identical every strike". The PHASE
    // policy does exactly that — and the test above proves it. But the rendered
    // attack is the phase policy times the shared onset, and the composed
    // envelope core deliberately RETRIGGERS FROM THE CURRENT LEVEL rather than
    // from zero, because restarting from zero clicks. So a strike that lands
    // while the previous note is still sounding is not bit-identical to a
    // strike from silence, and it should not be.
    //
    // Both halves are asserted here so the distinction survives: identical from
    // silence, deliberately not identical mid-note.
    const auto strike_pair = [](double attack_ms) {
        Bank bank;
        configure_steady(bank, harmonic_voice(8), 8, 220.0);
        bank.set_attack_ms(attack_ms);
        bank.set_retrig_phase(Bank::RetrigPhase::reset_stored);
        bank.reset();
        bank.retrigger();
        const auto first = render(bank, 1024);
        bank.retrigger();   // mid-note: the onset is at unity, not at zero
        const auto second = render(bank, 1024);
        return std::pair{first, second};
    };

    // A 20 ms attack has not finished in 1024 samples (21 ms), so the first
    // strike is still climbing while the second starts from the level it
    // reached. The two therefore differ.
    {
        const auto [first, second] = strike_pair(20.0);
        bool differs = false;
        for (std::size_t i = 0; i < first.size(); ++i)
            if (first[i] != second[i]) differs = true;
        REQUIRE(differs);
        // And the second strike is LOUDER, which is the anti-click behaviour
        // rather than a phase error: it resumes from the level the first had
        // climbed to. Compared as RMS over the strike, not at sample 0 — the
        // stored phase here is 0, so sample 0 is `sin(0)` in both renders and
        // carries no level information at all.
        const auto rms = [](const std::vector<double>& v) {
            double sq = 0.0;
            for (double s : v) sq += s * s;
            return std::sqrt(sq / static_cast<double>(v.size()));
        };
        REQUIRE(rms(second) > rms(first));
    }

    // With the onset already at unity in both cases — a zero-length attack —
    // the two strikes are bit-identical, confirming the difference above is the
    // envelope and not the phases.
    {
        const auto [first, second] = strike_pair(0.0);
        for (std::size_t i = 0; i < first.size(); ++i)
            REQUIRE(first[i] == second[i]);
    }
}

TEST_CASE("Coherent phases give a higher crest factor than scattered ones",
          "[signal][additive][retrigger]") {
    // Phase is a timbre control on the attack, not only a determinism detail:
    // aligned partials produce a percussive click, scattered ones a softer
    // entry at IDENTICAL partial amplitudes. Measured as peak-to-RMS, which is
    // the definition of crest factor — one of the two places in this file a
    // peak sample is the actual quantity under test.
    const auto crest = [](Bank::RetrigPhase policy, double stored_phase) {
        VoiceTable v = harmonic_voice(32, 1.0, stored_phase);
        Bank bank;
        configure_steady(bank, v, 32, 200.0);
        bank.set_retrig_phase(policy);
        bank.set_seed(0x1234u);
        bank.reset();
        bank.retrigger();
        const auto x = render(bank, 4800);

        double peak = 0.0, sq = 0.0;
        for (double s : x) {
            peak = std::max(peak, std::abs(s));
            sq += s * s;
        }
        return peak / std::sqrt(sq / static_cast<double>(x.size()));
    };

    const double aligned = crest(Bank::RetrigPhase::reset_stored, 0.25);
    const double scattered = crest(Bank::RetrigPhase::seeded_random, 0.25);
    REQUIRE(aligned > scattered);
    // A cosine-aligned bank of N equal partials has crest sqrt(2N) — every
    // partial peaks together while the RMS is the incoherent sum. For 32
    // partials that is 8.0.
    REQUIRE_THAT(aligned, WithinRel(std::sqrt(2.0 * 32.0), 0.02));
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-7 — the crest bound (series laws 1 and 8)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The coherent-sum crest bound equals the registry worst-case gain",
          "[signal][additive][gain]") {
    // The registry's number, and it is a proof rather than an estimate: the
    // normaliser divides by the sum of magnitudes, so |y| <= 1 by the triangle
    // inequality, and `master_gain_db` at its legal ceiling scales that.
    REQUIRE_THAT(Bank::worst_case_gain(),
                 WithinRel(units::db_to_linear(Bank::kMasterGainMaxDb), 1e-15));
    REQUIRE_THAT(db(Bank::worst_case_gain()),
                 WithinAbs(Bank::kMasterGainMaxDb, 1e-12));

    // The worst case is CONSTRUCTIBLE, not statistical: store phase 0.25 for
    // every partial so all of them sit at `sin = +1` together. f0 = 200 Hz
    // makes the organ's lowest ratio (0.5) repeat every 480 samples exactly, so
    // the realignment lands ON a sample and the peak is genuinely observable
    // rather than passing between two of them.
    VoiceTable organ = make_organ_voice(64);
    for (int p = 0; p < organ.count; ++p)
        organ.partials[static_cast<std::size_t>(p)].phase01 = 0.25;

    Bank bank;
    configure_steady(bank, organ, 64, 200.0);
    bank.set_master_gain_db(Bank::kMasterGainMaxDb);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 4800);

    double peak = 0.0;
    for (double v : x) peak = std::max(peak, std::abs(v));
    REQUIRE(peak <= Bank::worst_case_gain() + kOneLsb);
    // Attained — so the bound is tight, and a future change that quietly
    // over-attenuates fails here instead of passing a one-sided inequality.
    REQUIRE(peak > Bank::worst_case_gain() - kOneLsb);
}

TEST_CASE("The crest bound holds across the whole legal parameter range",
          "[signal][additive][gain]") {
    // Series law 8 wants a bound the module's own tests assert across the
    // range, not at one flattering point. Every combination of voice, partial
    // count, morph, tilt, detune, inharmonicity and envelope mode below is
    // rendered with phases aligned and the master trim at its ceiling.
    const auto envelope_b = SpectralEnvelope::tilt(6.0, 200.0);

    for (bool bell : {false, true}) {
        for (int count : {1, 16, 64, 128}) {
            for (double morph : {0.0, 1.0}) {
                for (double tilt : {Bank::kSpectralTiltMinDbOct,
                                    Bank::kSpectralTiltMaxDbOct}) {
                    for (double detune : {0.0, Bank::kDetuneMaxCents}) {
                        VoiceTable v = bell ? make_bell_voice(count)
                                            : make_organ_voice(count);
                        for (int p = 0; p < v.count; ++p)
                            v.partials[static_cast<std::size_t>(p)].phase01 = 0.25;

                        Bank bank;
                        configure_steady(bank, v, count, 200.0);
                        bank.set_envelope_a(SpectralEnvelope::tilt(0.0, 200.0));
                        bank.set_envelope_b(envelope_b);
                        bank.set_morph(static_cast<float>(morph));
                        bank.set_spectral_tilt_db_oct(tilt);
                        bank.set_detune_cents(detune);
                        bank.set_inharmonicity_b(Bank::kInharmonicityMax);
                        bank.set_master_gain_db(Bank::kMasterGainMaxDb);
                        bank.set_envelope_mode(
                            Bank::EnvelopeMode::per_partial_decay);
                        bank.reset();
                        bank.retrigger();

                        const auto x = render(bank, 2400);
                        for (double s : x) {
                            REQUIRE(std::isfinite(s));
                            REQUIRE(std::abs(s) <=
                                    Bank::worst_case_gain() + kOneLsb);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("The bound survives a parameter ramp",
          "[signal][additive][gain]") {
    // The normaliser is applied to the PER-PARTIAL gains rather than to the
    // summed output, which is what keeps the bound valid mid-crossfade: a
    // convex blend of two gain sets that each sum to <= 1 also sums to <= 1.
    // A post-sum normaliser would be a different number on each side of a
    // control-block edge and could overshoot between them. Swept hard enough
    // that a block-edge discontinuity would show.
    VoiceTable organ = make_organ_voice(64);
    for (int p = 0; p < organ.count; ++p)
        organ.partials[static_cast<std::size_t>(p)].phase01 = 0.25;

    Bank bank;
    configure_steady(bank, organ, 64, 200.0);
    bank.set_envelope_a(SpectralEnvelope::tilt(0.0, 200.0));
    bank.set_envelope_b(SpectralEnvelope::tilt(6.0, 200.0));
    bank.set_master_gain_db(Bank::kMasterGainMaxDb);
    bank.reset();
    bank.retrigger();

    constexpr int kBlocks = 600;
    constexpr int kBlock = 16;   // finer than the 32-sample control cadence, so
                                 // parameter changes land mid-block too
    for (int b = 0; b < kBlocks; ++b) {
        const double t = static_cast<double>(b) / (kBlocks - 1);
        bank.set_morph(static_cast<float>(t));
        bank.set_spectral_tilt_db_oct(Bank::kSpectralTiltMinDbOct +
                                      t * (Bank::kSpectralTiltMaxDbOct -
                                           Bank::kSpectralTiltMinDbOct));
        bank.set_fundamental_hz(200.0 + t * 300.0);
        for (double s : render(bank, kBlock)) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::abs(s) <= Bank::worst_case_gain() + kOneLsb);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-8 — determinism
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("A five-second bell render is bit-identical across reset",
          "[signal][additive][determinism]") {
    const auto run = [] {
        Bank bank;
        configure_steady(bank, make_bell_voice(64), 64, 220.0);
        bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
        bank.set_detune_cents(3.0);
        bank.set_retrig_phase(Bank::RetrigPhase::seeded_random);
        bank.set_attack_ms(2.0);
        bank.reset();

        std::vector<double> out;
        // A fixed trigger sequence, so the RNG advance and the envelope state
        // both have history by the end rather than being freshly reset.
        for (int strike = 0; strike < 4; ++strike) {
            bank.retrigger();
            const auto a = render(bank, 48000);
            out.insert(out.end(), a.begin(), a.end());
            bank.release();
            const auto b = render(bank, 12000);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    };

    const auto a = run();
    const auto b = run();
    REQUIRE(a.size() == static_cast<std::size_t>(4 * 60000));
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("Reset rewinds a live instance rather than a fresh one",
          "[signal][additive][determinism]") {
    // AT-6 and AT-8 in their literal form: "render -> reset -> render is
    // bit-identical". The distinction from the tests above is the whole point —
    // those build a NEW bank each time, so the generator starts at its seed
    // whether or not `reset()` rewinds it. Only re-rendering the SAME instance
    // after a reset can catch a `reset()` that forgets the RNG, and every
    // seeded quantity here (initial phases under `seeded_random`, the doublet
    // detune jitter) depends on it.
    Bank bank;
    configure_steady(bank, make_bell_voice(48), 48, 261.0);
    bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
    bank.set_retrig_phase(Bank::RetrigPhase::seeded_random);
    bank.set_detune_cents(9.0);
    bank.set_attack_ms(4.0);
    bank.set_seed(0x51EEDu);

    const auto pass = [&] {
        bank.reset();
        std::vector<double> out;
        for (int strike = 0; strike < 3; ++strike) {
            bank.retrigger();
            const auto a = render(bank, 6000);
            out.insert(out.end(), a.begin(), a.end());
            bank.release();
            const auto b = render(bank, 2000);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    };

    const auto first = pass();
    const auto second = pass();   // same object, only `reset()` between them
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);

    // Not vacuous: the render has to actually contain the seeded content. If
    // the phases were not being scattered at all, the test above would pass on
    // a bank with no randomness in it.
    double energy = 0.0;
    for (double v : first) energy += v * v;
    REQUIRE(energy > 0.0);

    // And re-seeding really does change the render, so the seed is reaching the
    // signal rather than being stored and ignored.
    bank.set_seed(0xD1FFu);
    const auto reseeded = pass();
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i)
        if (first[i] != reseeded[i]) differs = true;
    REQUIRE(differs);
}

TEST_CASE("The float and double instantiations agree on the physics",
          "[signal][additive][parity]") {
    // The accumulators, gains and sum are `double` in both instantiations; only
    // the output cast differs. So the two must agree to float rounding, and
    // each must be internally deterministic.
    constexpr int kWindow = 48000;
    constexpr double kF0 = 200.0;

    AdditiveBank f32;
    Bank f64;
    configure_steady(f32, harmonic_voice(16), 16, kF0);
    configure_steady(f64, harmonic_voice(16), 16, kF0);

    const auto a = render(f32, kWindow);
    const auto b = render(f64, kWindow);
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE_THAT(a[i], WithinAbs(b[i], 1e-6));

    for (int n = 1; n <= 16; ++n) {
        const double x = coherent_amplitude(a, static_cast<int>(kF0) * n, kWindow);
        const double y = coherent_amplitude(b, static_cast<int>(kF0) * n, kWindow);
        REQUIRE_THAT(x, WithinRel(y, 1e-5));
    }

    AdditiveBank again;
    configure_steady(again, harmonic_voice(16), 16, kF0);
    const auto c = render(again, kWindow);
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == c[i]);

    REQUIRE_THAT(AdditiveBank::worst_case_gain(),
                 WithinRel(Bank::worst_case_gain(), 1e-15));
    REQUIRE(AdditiveBank::latency_samples() == Bank::latency_samples());
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-9 — the RT contract
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Nothing allocates after prepare",
          "[signal][additive][rt]") {
    auto bank = std::make_unique<Bank>();
    bank->prepare(kSr, Bank::kMaxPartialsDefault);

    const auto organ = make_organ_voice(128);
    const auto bell = make_bell_voice(128);
    const auto envelope = SpectralEnvelope::tilt(-6.0, 200.0);
    std::vector<double> out(512, 0.0);

    require_allocates_no_memory([&] {
        bank->load_voice(organ);
        bank->load_voice(bell);
        bank->set_partial_count(97);
        bank->set_fundamental_hz(311.0);
        bank->set_inharmonicity_b(Bank::kBUpperTreble);
        bank->set_spectral_tilt_db_oct(-9.0);
        bank->set_master_gain_db(-3.0);
        bank->set_envelope_a(envelope);
        bank->set_envelope_b(envelope);
        bank->set_morph(0.5f);
        bank->set_spectral_domain(SpectralDomain::relative_to_f0);
        bank->set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
        bank->set_attack_ms(3.0);
        bank->set_release_ms(500.0);
        bank->set_detune_cents(7.0);
        bank->set_pitch_glide(-120.0, 40.0);
        bank->set_retrig_phase(Bank::RetrigPhase::seeded_random);
        bank->set_seed(99u);
        bank->set_partial(3, 3.5, 0.4, 0.1, 250.0);
        bank->reset();
        bank->retrigger();
        bank->process(out.data(), 512);
        (void) bank->next();
        bank->release();
        bank->process(out.data(), 512);
        (void) bank->partial_frequency(3);
        (void) bank->envelope_db_at(1000.0);
    });

    // A silent tail flushes to exact zero rather than drizzling subnormals into
    // the accumulator for minutes after a long decay.
    bank->reset();
    const auto tail = render(*bank, 4096);
    for (double v : tail) {
        REQUIRE(std::fpclassify(v) != FP_SUBNORMAL);
        REQUIRE(v == 0.0);   // never retriggered, so the onset is idle
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// AT-10 — latency
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Latency is zero and output begins at sample zero",
          "[signal][additive][latency]") {
    REQUIRE(Bank::latency_samples() == 0);
    REQUIRE(AdditiveBank::latency_samples() == 0);

    // Phase 0.25 puts the partial at its crest on the very first sample, so a
    // group delay of even one sample would show as a zero at index 0.
    Bank bank;
    configure_steady(bank, harmonic_voice(1, 1.0, 0.25), 1, 440.0);
    const auto x = render(bank, 16);
    REQUIRE(x[0] != 0.0);
    REQUIRE_THAT(x[0], WithinRel(1.0, 1e-9));

    // The same with a full 128-partial bank: no buffering appears at scale.
    Bank big;
    configure_steady(big, harmonic_voice(128, 1.0, 0.25), 128, 100.0);
    const auto y = render(big, 16);
    REQUIRE(y[0] != 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Per-partial trajectories, the voice tables, and the doublet
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Per-partial decay follows each partial's own time constant",
          "[signal][additive][envelope]") {
    // The whole reason `per_partial_decay` exists: a struck body's top dies
    // while its bottom rings. Measured by fitting the decay of two partials
    // with a 4:1 ratio in their table time constants.
    constexpr double kF0 = 200.0;
    constexpr double kSlowMs = 4000.0;
    constexpr double kFastMs = 1000.0;

    VoiceTable v;
    v.harmonic = true;
    v.add({1.0, 1.0, 0.0, kSlowMs});
    v.add({4.0, 1.0, 0.0, kFastMs});

    Bank bank;
    configure_steady(bank, v, 2, kF0);
    bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 96000);   // 2 s

    const auto tau_ms_of = [&](int cycles) {
        // Two coherent windows a known distance apart; the ratio of amplitudes
        // gives the time constant directly, with no curve fitting.
        constexpr int kWin = 4800;        // 0.1 s, whole cycles of every partial
        const int gap = 48000;            // 1.0 s between window starts
        const double a0 = coherent_amplitude(x, cycles / 10, kWin, 0);
        const double a1 = coherent_amplitude(x, cycles / 10, kWin, gap);
        return 1000.0 * (static_cast<double>(gap) / kSr) / std::log(a0 / a1);
    };

    REQUIRE_THAT(tau_ms_of(static_cast<int>(kF0)), WithinRel(kSlowMs, 0.02));
    REQUIRE_THAT(tau_ms_of(static_cast<int>(kF0) * 4), WithinRel(kFastMs, 0.02));

    // `shared_ar` ignores the table's time constants outright — both partials
    // hold. If the mode leaked, the 4:1 spread above would still be visible.
    Bank shared;
    configure_steady(shared, v, 2, kF0);
    shared.set_envelope_mode(Bank::EnvelopeMode::shared_ar);
    shared.reset();
    shared.retrigger();
    const auto held = render(shared, 96000);
    for (int cycles : {static_cast<int>(kF0), static_cast<int>(kF0) * 4}) {
        const double a0 = coherent_amplitude(held, cycles / 10, 4800, 0);
        const double a1 = coherent_amplitude(held, cycles / 10, 4800, 48000);
        REQUIRE_THAT(a1, WithinRel(a0, 1e-9));
    }
}

TEST_CASE("The shared onset holds at unity while gated",
          "[signal][additive][envelope]") {
    // A dependency on `envelope.hpp` worth pinning, because getting it wrong is
    // silent and costs exactly 3.1 dB. `ArT` is the right core for an
    // attack-that-holds shape, but with a held gate it parks at its `sustain_`
    // member — which defaults to 0.7, not to the peak its own comment
    // describes. This engine sets sustain to 1 explicitly; the assertion below
    // fails loudly if that ever stops being necessary or stops being done.
    ArT<double> onset;
    onset.prepare(kSr);
    onset.set_attack_ms(1.0);
    onset.set_sustain(1.0);
    onset.gate_on();
    for (int i = 0; i < 4800; ++i) onset.next();
    REQUIRE_THAT(onset.next(), WithinAbs(1.0, 1e-12));

    ArT<double> defaulted;
    defaulted.prepare(kSr);
    defaulted.set_attack_ms(1.0);
    defaulted.gate_on();
    for (int i = 0; i < 4800; ++i) defaulted.next();
    REQUIRE(defaulted.next() < 0.99);

    // Which the bank does not inherit: a sustained partial holds its full
    // table amplitude for as long as the gate is up.
    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, 200.0);
    bank.set_attack_ms(1.0);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 96000);
    REQUIRE_THAT(coherent_amplitude(x, 20, 4800, 48000),
                 WithinRel(coherent_amplitude(x, 20, 4800, 24000), 1e-9));
    REQUIRE_THAT(coherent_amplitude(x, 20, 4800, 48000), WithinRel(1.0, 1e-6));

    // And a release actually falls.
    bank.release();
    const auto tail = render(bank, 96000);
    REQUIRE(std::abs(tail.back()) < 1e-9);
}

TEST_CASE("The doublet splits a partial into a beating pair",
          "[signal][additive][doublet]") {
    // A real bell's casting asymmetry gives each mode two close frequencies.
    // With the detune engaged the bank renders two oscillators per partial, so
    // the test looks for two peaks where one used to be.
    constexpr double kF0 = 440.0;
    constexpr double kCents = 20.0;   // wide enough to resolve in 2.7 s

    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, kF0);
    bank.set_detune_cents(kCents);
    REQUIRE(bank.doublet_active());
    bank.set_seed(0x2468u);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 1 << 17);

    // The pair straddles the nominal frequency, each within half the detune
    // times the seeded jitter — so the bracket is the widest the jitter allows.
    const double widest = 0.5 * kCents * (1.0 + Bank::kDoubletJitterSpread);
    const double lo_edge = kF0 * units::cents_to_ratio(-widest * 1.05);
    const double hi_edge = kF0 * units::cents_to_ratio(widest * 1.05);

    const double lower = refine_peak(x, lo_edge, kF0);
    const double upper = refine_peak(x, kF0, hi_edge);
    REQUIRE(lower < kF0);
    REQUIRE(upper > kF0);

    // Symmetric about the nominal in cents, because the pair is +/- the same
    // jittered offset rather than two independent draws.
    REQUIRE_THAT(units::ratio_to_cents(lower / kF0),
                 WithinAbs(-units::ratio_to_cents(upper / kF0), 1e-3));

    // Zero detune renders ONE oscillator, not two coincident ones at half
    // amplitude — the amplitude at the nominal is the full table value.
    Bank mono;
    configure_steady(mono, harmonic_voice(1), 1, 200.0);
    REQUIRE_FALSE(mono.doublet_active());
    const auto m = render(mono, 48000);
    REQUIRE_THAT(coherent_amplitude(m, 200, 48000), WithinRel(1.0, 1e-9));

    // And the pair conserves level: two half-amplitude oscillators, so the
    // sum's magnitude at the nominal is unchanged when they are coincident in
    // the limit of zero detune.
    Bank pair;
    configure_steady(pair, harmonic_voice(1), 1, 200.0);
    pair.set_detune_cents(1e-6);
    pair.reset();
    pair.retrigger();
    const auto p = render(pair, 48000);
    REQUIRE_THAT(coherent_amplitude(p, 200, 48000), WithinRel(1.0, 1e-4));
}

TEST_CASE("The organ voice is the documented drawbar registration",
          "[signal][additive][voice]") {
    // The footage-to-harmonic mapping is documented behaviour (Hammond, US
    // 1,956,350): 16' -> 0.5, 5 1/3' -> 1.5, 8' -> 1, 4' -> 2, 2 2/3' -> 3,
    // 2' -> 4, 1 3/5' -> 5, 1 1/3' -> 6, 1' -> 8.
    const double expected_ratio[9] = {0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
    const auto organ = make_organ_voice(64);

    REQUIRE(organ.harmonic);
    REQUIRE(organ.count == 64);
    for (int p = 0; p < 9; ++p) {
        REQUIRE_THAT(organ.partials[static_cast<std::size_t>(p)].ratio,
                     WithinRel(expected_ratio[p], 1e-15));
        REQUIRE(organ.partials[static_cast<std::size_t>(p)].amp > 0.0);
    }

    // Every partial is sustained — a tonewheel is switched, not struck.
    for (int p = 0; p < organ.count; ++p)
        REQUIRE(organ.partials[static_cast<std::size_t>(p)].decay_ms <= 0.0);

    // Documented tonewheel voices are phase-coherent.
    for (int p = 0; p < organ.count; ++p)
        REQUIRE(organ.partials[static_cast<std::size_t>(p)].phase01 == 0.0);

    // The tail past the ninth drawbar is -12 dB/oct, i.e. amplitude
    // proportional to 1/ratio^2. Checked as a ratio between two partials an
    // octave apart, so the assertion is about the LAW rather than the level.
    //
    // The expectation is COMPUTED from that exponent, not written as "-12":
    // "12 dB per octave" is the trade name for a factor of four, and
    // 20*log10(4) is 12.0412 dB. Asserting the round number to two decimals
    // fails a correct implementation by 0.04 dB.
    const auto amp_at = [&](int p) {
        return organ.partials[static_cast<std::size_t>(p)].amp;
    };
    constexpr double kTailExponent = 2.0;
    const double per_octave_db = -20.0 * std::log10(std::pow(2.0, kTailExponent));
    REQUIRE_THAT(per_octave_db, WithinAbs(-12.0412, 1e-3));
    REQUIRE_THAT(db(amp_at(31) / amp_at(15)), WithinAbs(per_octave_db, 1e-9));
    REQUIRE_THAT(db(amp_at(63) / amp_at(31)), WithinAbs(per_octave_db, 1e-9));
}

TEST_CASE("The bell voice has its named modes and its decay spread",
          "[signal][additive][voice]") {
    // The five tuned partials of the modern English church bell are documented
    // (Perrin, Charnley & de Pont 1983): hum 0.5, prime 1.0, tierce ~1.2,
    // quint 1.5, nominal 2.0.
    const auto bell = make_bell_voice(64);
    REQUIRE_FALSE(bell.harmonic);

    REQUIRE_THAT(bell.partials[0].ratio, WithinAbs(0.50, 1e-12));
    REQUIRE_THAT(bell.partials[1].ratio, WithinAbs(1.00, 1e-12));
    REQUIRE_THAT(bell.partials[2].ratio, WithinAbs(1.20, 0.02));   // tierce
    REQUIRE_THAT(bell.partials[3].ratio, WithinAbs(1.50, 1e-12));
    REQUIRE_THAT(bell.partials[4].ratio, WithinAbs(2.00, 1e-12));

    // Non-integer ratios are the point — a bell is not a harmonic series.
    for (int p = 0; p < 5; ++p) {
        const double r = bell.partials[static_cast<std::size_t>(p)].ratio;
        if (p == 2) REQUIRE(std::abs(r - std::round(r)) > 0.1);
    }

    // The decay spread the voice exists for: the nominal rings 2.5 s while the
    // p=10 cluster mode dies in about 526 ms. The spec's worked check.
    const double nominal_ms = bell.partials[4].decay_ms;
    const double cluster_ms = bell.partials[10].decay_ms;
    REQUIRE_THAT(nominal_ms / cluster_ms, WithinRel(4.75, 0.02));
    REQUIRE_THAT(cluster_ms, WithinAbs(526.0, 2.0));
    REQUIRE_THAT(bell.partials[10].ratio, WithinAbs(5.7, 0.01));

    // Monotone: higher modes die first, all the way up the cluster.
    for (int p = 11; p < bell.count; ++p) {
        REQUIRE(bell.partials[static_cast<std::size_t>(p)].ratio >
                bell.partials[static_cast<std::size_t>(p - 1)].ratio);
        REQUIRE(bell.partials[static_cast<std::size_t>(p)].decay_ms <
                bell.partials[static_cast<std::size_t>(p - 1)].decay_ms);
    }

    // And it is audibly a bell: with the prime at 440 the nominal sits at 880.
    Bank bank;
    configure_steady(bank, bell, 16, 440.0);
    REQUIRE_THAT(bank.partial_frequency(4), WithinRel(880.0, 1e-9));
    REQUIRE_THAT(bank.partial_frequency(0), WithinRel(220.0, 1e-9));
}

TEST_CASE("Partial count and max partials are clamped and reported",
          "[signal][additive][limits]") {
    Bank bank;
    bank.prepare(kSr, 32);
    REQUIRE(bank.max_partials() == 32);
    bank.load_voice(make_organ_voice(128));

    bank.set_partial_count(1000);
    REQUIRE(bank.partial_count() == 32);   // capped by prepare, not by the table
    bank.set_partial_count(-4);
    REQUIRE(bank.partial_count() == 1);
    bank.set_partial_count(17);
    REQUIRE(bank.partial_count() == 17);

    // `prepare` bounds `max_partials` to the array ceiling.
    bank.prepare(kSr, 10000);
    REQUIRE(bank.max_partials() == Bank::kMaxPartialsCeiling);
    bank.prepare(kSr, 0);
    REQUIRE(bank.max_partials() == 1);

    // A table shorter than the requested count caps the count too, so the bank
    // never reads a row that was never written.
    Bank small;
    small.prepare(kSr, 128);
    VoiceTable v = harmonic_voice(5);
    small.load_voice(v);
    small.set_partial_count(64);
    REQUIRE(small.partial_count() == 5);

    // Parameter clamps report what actually took effect.
    Bank p;
    p.prepare(kSr, 64);
    p.set_fundamental_hz(1e9);
    REQUIRE_THAT(p.fundamental_hz(), WithinRel(Bank::kFundamentalMaxHz, 1e-12));
    p.set_fundamental_hz(0.0);
    REQUIRE_THAT(p.fundamental_hz(), WithinRel(Bank::kFundamentalMinHz, 1e-12));
    p.set_inharmonicity_b(5.0);
    REQUIRE_THAT(p.inharmonicity_b(), WithinRel(Bank::kInharmonicityMax, 1e-12));
    p.set_inharmonicity_b(-1.0);
    REQUIRE_THAT(p.inharmonicity_b(), WithinAbs(0.0, 1e-15));
    p.set_master_gain_db(100.0);
    REQUIRE_THAT(p.master_gain_db(), WithinRel(Bank::kMasterGainMaxDb, 1e-12));
    p.set_detune_cents(1000.0);
    REQUIRE_THAT(p.detune_cents(), WithinRel(Bank::kDetuneMaxCents, 1e-12));
    p.set_morph(9.0f);
    REQUIRE_THAT(p.morph(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("A pitch glide scales every partial together",
          "[signal][additive][trajectory]") {
    // The strike chiff: a shared initial glide, so the whole spectrum arrives
    // sharp and settles. Scaling all partials by one ratio is what keeps it a
    // pitch move rather than an inharmonicity move.
    constexpr double kF0 = 400.0;
    constexpr double kStartCents = 200.0;
    constexpr double kGlideMs = 100.0;

    Bank bank;
    configure_steady(bank, harmonic_voice(4), 4, kF0);
    bank.set_pitch_glide(kStartCents, kGlideMs);
    bank.reset();
    bank.retrigger();

    // At the strike every partial is sharp by exactly the same ratio.
    const double expected = units::cents_to_ratio(kStartCents);
    for (int p = 0; p < 4; ++p)
        REQUIRE_THAT(bank.partial_frequency(p),
                     WithinRel(kF0 * static_cast<double>(p + 1) * expected, 1e-9));

    // After the glide time it has settled to the nominal.
    (void) render(bank, static_cast<int>(kSr * kGlideMs / 1000.0) + 256);
    for (int p = 0; p < 4; ++p)
        REQUIRE_THAT(bank.partial_frequency(p),
                     WithinRel(kF0 * static_cast<double>(p + 1), 1e-6));
}
