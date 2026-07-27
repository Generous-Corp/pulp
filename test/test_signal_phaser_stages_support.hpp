#pragma once

// PhaserStagesT — the cascaded-allpass phaser acceptance suite.
//
// This is module M12's suite (spec: phaser-pulp-module-prompt.md, acceptance
// tests 1–13). Every expected value is COMPUTED from a shipped constant or a
// shipped closed form — `PhaserStagesT::notch_frequency_hz`,
// `notch_frequency_analog_hz`, `worst_case_gain`, `kFeedbackMax`,
// `kSweepRangeRatio` — never restated as a literal, so moving a constant fails
// the test that documents it instead of quietly disagreeing with it.
//
// ## Measurement recipe, and why it is built this way
//
// Freezing the LFO (`set_rate_hz(0)`, `set_stereo_spread(0)`) makes the whole
// module exactly LINEAR TIME-INVARIANT: constant coefficients, a linear
// feedback sum, a linear mix. So its impulse response is a complete
// description, and every magnitude/phase question is answered from one render
// instead of one render per frequency. Two instruments read that response, and
// they check each other:
//
//   * `Spectrum` — a small radix-2 FFT of the impulse response. Whole-band, all
//     bins at once. Used for broad scans: counting notches, finding the peak of
//     a resonant loop, bounding gain over a parameter grid.
//   * `response_at` — a direct DTFT of the same impulse response at ONE exact
//     frequency, no bin grid and no window. Used wherever the answer has to be
//     sub-bin exact: locating a notch minimum, reading the phase at it.
//
// Neither is trusted on its own. `Instruments agree` cross-checks the FFT
// against the DTFT, and `Coherent DFT of a real rendered sine` cross-checks the
// DTFT against an actual sine pushed through `process()` and measured by a
// coherent single-bin DFT — the slow, unimpeachable measurement. If the fast
// instrument ever drifts from the slow one, those two tests fail before any
// physics test can be misread.
//
// PEAK-SAMPLE MEASUREMENT IS NEVER USED for an amplitude, and one test
// (`Peak-sample amplitude under-reads`) exists purely to show why: a discrete
// 8 kHz sine at 48 kHz has six samples per cycle and none of them lands on the
// crest, so its peak sample reads 1.25 dB low — indistinguishable from a filter
// that is not flat. Every amplitude here comes from a coherent DFT.
//
// Acceptance-class constants (FFT sizes, render lengths, grid densities, ±
// bounds) are stated at their use site with the reason they are big/small
// enough; per the series contract's precise reading they are neither cited
// values nor design parameters.
//
// ## Spec deviations, each with the number that forced it
//
//   1. Acceptance tests 1 and 2 ask for notch positions within ±2 % of the
//      ANALOG law `f_k = fc·tan((2k−1)π/2N)`. The shipped filter is the
//      bilinear/TPT discretisation of that prototype, and the two diverge
//      beyond ±2 % INSIDE the catalog's own `center_hz` range — at N = 4,
//      fc = 2000 Hz the analog law is +2.69 % off, and at N = 12, fc = 2000 Hz
//      it is +26.6 % off. No correct implementation can meet the criterion as
//      written. `Analog prototype law versus the shipped digital law` proves
//      that with numbers; the position tests assert against the shipped digital
//      law to 0.01 %, which is a far tighter bound than the spec asked for.
//   2. Acceptance test 12 asks for "no observable performance cliff in a
//      release-mode timing probe". Wall-clock assertions are not deterministic
//      on a shared CI runner. The deterministic property the timing probe was
//      standing in for — that recursive state reaches EXACT zero rather than
//      lingering as a subnormal — is asserted directly instead.
//   3. Acceptance test 10 asks for a roster entry in `test_signal_rt_safety.cpp`.
//      The probe runs here, against the same `RtAllocationProbe` harness, so the
//      module's RT contract is covered by the module's own suite.
//   4. Acceptance test 7 asks for the stereo phase relationship to be recovered
//      by cross-correlating notch contours extracted from audio. The swept
//      corner frequency is read directly from `sweep_frequency_hz()` instead:
//      it is the quantity under test, and recovering it from audio would put a
//      pitch-tracker's error bars in front of a relationship that is exact.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/phaser_stages.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Phaser = PhaserStages64;   // the analysis engine: double, so a measured
                                 // null is limited by the physics, not by the
                                 // sample type. The `float` alias gets its own
                                 // parity test.
using Complex = std::complex<double>;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// The spec's reference configuration: Small Stone mode at its documented
/// centre. Every notch-position expectation is a function of these.
constexpr int kSmallStoneStages = 4;
constexpr double kRefCenterHz = 400.0;

/// Impulse-response length for a feedback-free configuration. The stages snap
/// their own integrators to zero through `snap_to_zero`, so these responses are
/// EXACTLY zero well before this — measured tail energy past 2048 samples is
/// identically 0.0 at every stage count. 4096 is that with a doubling of slack.
constexpr int kIrLenOpenLoop = 4096;

/// Impulse-response length for the maximum-feedback configuration. A k = 0.9
/// loop closed around a 12-stage cascade still has 8.9e-10 of its energy past
/// 32768 samples; at 65536 it is 1.9e-16, so truncation cannot move a
/// magnitude by more than ~1e-8 relative — four orders below the 1e-3 the
/// worst-case-gain bound is asserted to.
constexpr int kIrLenClosedLoop = 65536;

// ── Engine configuration ──────────────────────────────────────────────────

struct Config {
    int stages = kSmallStoneStages;
    double center_hz = kRefCenterHz;
    double feedback = 0.0;
    double mix = Phaser::kMixDefault;
    double stagger = Phaser::kStaggerDefault;
    double sample_rate = kSr;
};

/// Configures an engine and FREEZES its LFO, which is what makes the module
/// linear time-invariant and therefore fully described by an impulse response.
/// `rate_hz(0)` holds both LFO phases at 0, where triangle and sine are both
/// exactly 0, so `fc == center_hz` on both channels; `stereo_spread(0)` keeps
/// the right channel's offset from moving it.
template <typename EngineT>
void configure_frozen(EngineT& engine, const Config& cfg) {
    engine.prepare(cfg.sample_rate);
    engine.set_rate_hz(0.0);
    engine.set_stereo_spread(0.0f);
    engine.set_depth(0.0f);
    engine.set_stage_count(cfg.stages);
    engine.set_center_hz(cfg.center_hz);
    engine.set_feedback(static_cast<float>(cfg.feedback));
    engine.set_mix(static_cast<float>(cfg.mix));
    engine.set_stagger_ratio(cfg.stagger);
    engine.reset();
}

/// Renders a block of stereo audio through a configured engine.
template <typename EngineT, typename S>
void render(EngineT& engine, const std::vector<S>& in, std::vector<S>& out_l,
            std::vector<S>& out_r) {
    out_l.assign(in.size(), S{0});
    out_r.assign(in.size(), S{0});
    engine.process(in.data(), in.data(), out_l.data(), out_r.data(),
                   static_cast<int>(in.size()));
}

/// Impulse response of the frozen engine (left channel), as `double`.
template <typename EngineT = Phaser>
std::vector<double> frozen_impulse_response(const Config& cfg, int length) {
    using S = std::conditional_t<std::is_same_v<EngineT, PhaserStages>, float,
                                 double>;
    EngineT engine;
    configure_frozen(engine, cfg);

    std::vector<S> in(static_cast<std::size_t>(length), S{0});
    in[0] = S{1};
    std::vector<S> ol, orr;
    render(engine, in, ol, orr);

    std::vector<double> h(ol.size());
    for (std::size_t i = 0; i < ol.size(); ++i)
        h[i] = static_cast<double>(ol[i]);
    return h;
}

// ── Instrument A: exact DTFT at one frequency ─────────────────────────────

/// `H(f)` of an impulse response, evaluated at an EXACT frequency — no bin
/// grid, no window, no leakage. The phasor is stepped recursively and
/// renormalised every 1024 samples so its magnitude cannot drift.
Complex response_at(const std::vector<double>& h, double f_hz,
                    double sample_rate = kSr) {
    const double w = -2.0 * kPi * f_hz / sample_rate;
    const Complex step(std::cos(w), std::sin(w));
    Complex phasor(1.0, 0.0);
    Complex acc(0.0, 0.0);
    for (std::size_t i = 0; i < h.size(); ++i) {
        acc += h[i] * phasor;
        phasor *= step;
        if ((i & 1023u) == 1023u) phasor /= std::abs(phasor);
    }
    return acc;
}

double magnitude_at(const std::vector<double>& h, double f_hz) {
    return std::abs(response_at(h, f_hz));
}

// ── Instrument B: radix-2 FFT, for whole-band scans ───────────────────────

/// In-place iterative radix-2 decimation-in-time FFT. Thirty lines, no
/// dependency, and cross-checked against `response_at` by its own test.
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

/// Magnitude spectrum of an impulse response over `[0, Nyquist]`.
struct Spectrum {
    std::vector<double> magnitude;   ///< bins 0 .. n/2
    double bin_hz = 0.0;

    double frequency(std::size_t bin) const {
        return static_cast<double>(bin) * bin_hz;
    }
    std::size_t bin_for(double hz) const {
        return static_cast<std::size_t>(hz / bin_hz);
    }
};

Spectrum spectrum_of(const std::vector<double>& h, double sample_rate = kSr) {
    std::size_t n = 1;
    while (n < h.size()) n <<= 1;
    std::vector<Complex> buf(n, Complex(0.0, 0.0));
    for (std::size_t i = 0; i < h.size(); ++i) buf[i] = Complex(h[i], 0.0);
    fft_in_place(buf);

    Spectrum s;
    s.bin_hz = sample_rate / static_cast<double>(n);
    s.magnitude.resize(n / 2 + 1);
    for (std::size_t i = 0; i < s.magnitude.size(); ++i)
        s.magnitude[i] = std::abs(buf[i]);
    return s;
}

// ── Notch location and counting ───────────────────────────────────────────

/// Refines a notch minimum by ternary search on the exact DTFT magnitude.
/// `|H(f)|` is smooth and unimodal in a bracket that contains exactly one
/// null, so this converges to the true minimum rather than to a bin centre.
double refine_minimum(const std::vector<double>& h, double lo, double hi) {
    for (int i = 0; i < 200 && (hi - lo) > 1e-9 * hi; ++i) {
        const double a = lo + (hi - lo) / 3.0;
        const double b = hi - (hi - lo) / 3.0;
        if (magnitude_at(h, a) < magnitude_at(h, b)) hi = b; else lo = a;
    }
    return 0.5 * (lo + hi);
}

/// Counts distinct notches in a magnitude spectrum: contiguous runs of bins
/// more than `depth_db` below the response's own peak. Between any two notches
/// the cascade's phase passes an EVEN multiple of π, where the mix-0.5 response
/// returns to exactly its peak, so the runs are cleanly separated and a run
/// count is a notch count.
int count_notches(const Spectrum& s, double depth_db) {
    const double peak = *std::max_element(s.magnitude.begin(), s.magnitude.end());
    const double threshold = peak * std::pow(10.0, -depth_db / 20.0);
    int runs = 0;
    bool inside = false;
    for (double m : s.magnitude) {
        if (m < threshold) {
            if (!inside) ++runs;
            inside = true;
        } else {
            inside = false;
        }
    }
    return runs;
}

// ── Instrument C: coherent DFT of a real rendered sine ────────────────────

/// Drives a configured engine with a sine at exactly `cycles · fs / window`
/// and measures the output amplitude with a single-bin DFT over exactly that
/// window. Integer cycles per window means zero leakage: the measurement is
/// exact and needs no window function and no correction factor. This is the
/// slow instrument the fast ones are validated against.
double coherent_sine_amplitude(const Config& cfg, int cycles, int window,
                               int settle) {
    Phaser engine;
    configure_frozen(engine, cfg);

    const double f = static_cast<double>(cycles) * kSr / static_cast<double>(window);
    const double w = 2.0 * kPi * f / kSr;

    std::vector<double> in(static_cast<std::size_t>(settle + window));
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(w * static_cast<double>(i));

    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    Complex acc(0.0, 0.0);
    for (int i = 0; i < window; ++i) {
        const double phase = -2.0 * kPi * static_cast<double>(cycles) *
                             static_cast<double>(i) / static_cast<double>(window);
        acc += ol[static_cast<std::size_t>(settle + i)] *
               Complex(std::cos(phase), std::sin(phase));
    }
    return 2.0 * std::abs(acc) / static_cast<double>(window);
}

// ── Misc ──────────────────────────────────────────────────────────────────

double db(double linear) { return 20.0 * std::log10(linear); }

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// The sweep contour of both channels, one entry per frame, taken through the
/// shipped block API a frame at a time.
struct SweepContours {
    std::vector<double> left, right;
};

SweepContours sweep_contours(int frames, double rate_hz, double spread_cycles,
                             double depth, double center_hz,
                             LfoWave wave = LfoWave::triangle) {
    Phaser engine;
    engine.prepare(kSr);
    engine.set_rate_hz(rate_hz);
    engine.set_stereo_spread(static_cast<float>(spread_cycles));
    engine.set_depth(static_cast<float>(depth));
    engine.set_center_hz(center_hz);
    engine.set_wave(wave);
    engine.reset();

    SweepContours c;
    c.left.reserve(static_cast<std::size_t>(frames));
    c.right.reserve(static_cast<std::size_t>(frames));
    double x = 0.0, ol = 0.0, orr = 0.0;
    for (int i = 0; i < frames; ++i) {
        engine.process(&x, &x, &ol, &orr, 1);
        c.left.push_back(engine.sweep_frequency_hz(0));
        c.right.push_back(engine.sweep_frequency_hz(1));
    }
    return c;
}

/// Normalised zero-mean cross-correlation of two equal-length contours at a
/// sample lag. ±1 means "the same shape, shifted"; 0 means orthogonal.
double correlation_at_lag(const std::vector<double>& a,
                          const std::vector<double>& b, int lag) {
    const int n = static_cast<int>(a.size());
    double mean_a = 0.0, mean_b = 0.0;
    for (double v : a) mean_a += v;
    for (double v : b) mean_b += v;
    mean_a /= n;
    mean_b /= n;

    double num = 0.0, da = 0.0, dbb = 0.0;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const int j = i + lag;
        if (j < 0 || j >= n) continue;
        const double u = a[static_cast<std::size_t>(i)] - mean_a;
        const double v = b[static_cast<std::size_t>(j)] - mean_b;
        num += u * v;
        da += u * u;
        dbb += v * v;
        ++count;
    }
    REQUIRE(count > 0);
    return num / std::sqrt(da * dbb);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Instrument validation — run these first; every physics test below reads a
// number through one of them.
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 1 — notch position, Small Stone mode
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 2 — notch count scales with stage count
// ═══════════════════════════════════════════════════════════════════════════



// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 3 — the mix law
// ═══════════════════════════════════════════════════════════════════════════



// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 4 — the allpass stage is unity gain
// ═══════════════════════════════════════════════════════════════════════════




// ═══════════════════════════════════════════════════════════════════════════
// Acceptance tests 5 and 6 — the feedback bound (series laws 1 and 8)
// ═══════════════════════════════════════════════════════════════════════════





// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 7 — stereo quadrature spread
// ═══════════════════════════════════════════════════════════════════════════





// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 8 — triangle versus sine sweep shape
// ═══════════════════════════════════════════════════════════════════════════



// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 9 — determinism
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 10 — the RT contract
// ═══════════════════════════════════════════════════════════════════════════



// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 11 — latency
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 12 — denormal safety
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 13 — stage-count clamping
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// Anti-aliasing policy (series law 4) — the claim, asserted
// ═══════════════════════════════════════════════════════════════════════════


// ═══════════════════════════════════════════════════════════════════════════
// The stagger extension, and the sample-type parity
// ═══════════════════════════════════════════════════════════════════════════



// ═══════════════════════════════════════════════════════════════════════════
// The shipped defaults are the documented preset
// ═══════════════════════════════════════════════════════════════════════════
