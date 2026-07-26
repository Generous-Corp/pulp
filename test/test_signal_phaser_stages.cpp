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

TEST_CASE("Peak-sample amplitude under-reads a discrete sine",
          "[signal][phaser][measurement]") {
    // Not a property of the phaser — a property of the RULER. An 8 kHz sine at
    // 48 kHz has exactly six samples per cycle and none of them lands on the
    // crest, so its largest sample is sin(60 deg) = 0.866 of the true
    // amplitude: 1.25 dB low. Measured through a bypassed phaser, so what is
    // being compared is two readings of the SAME signal.
    Config bypass;
    bypass.mix = 0.0;

    const int window = 6000;         // 8 kHz is exactly 1000 cycles of it
    const int cycles = 1000;
    const int settle = 0;            // mix = 0 is a wire; nothing to settle

    Phaser engine;
    configure_frozen(engine, bypass);
    std::vector<double> in(static_cast<std::size_t>(window));
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(2.0 * kPi * 8000.0 * static_cast<double>(i) / kSr);
    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    double peak = 0.0;
    for (double v : ol) peak = std::max(peak, std::abs(v));

    const double coherent = coherent_sine_amplitude(bypass, cycles, window, settle);

    // The coherent DFT recovers the true unit amplitude exactly.
    REQUIRE_THAT(coherent, WithinRel(1.0, 1e-9));
    // The peak sample is low by exactly the computed amount — 20·log10(sin(pi/3)).
    const double expected_error_db = db(std::sin(kPi / 3.0));
    REQUIRE_THAT(db(peak), WithinAbs(expected_error_db, 1e-6));
    // Which is over a decibel: large enough to be mistaken for a real defect.
    REQUIRE(expected_error_db < -1.0);
}

TEST_CASE("Instruments agree on the same impulse response",
          "[signal][phaser][measurement]") {
    // The FFT is fast and coarse; the DTFT is exact at a point. If they ever
    // disagree, a physics test below is reading a broken ruler. Checked on the
    // hardest configuration — maximum feedback, where the response has the
    // sharpest features.
    Config cfg;
    cfg.stages = 12;
    cfg.feedback = Phaser::kFeedbackMax;
    cfg.mix = 1.0;

    const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
    const auto s = spectrum_of(h);

    for (std::size_t bin : {std::size_t{1}, std::size_t{137}, std::size_t{4001},
                            std::size_t{20000}}) {
        const double from_fft = s.magnitude[bin];
        const double from_dtft = magnitude_at(h, s.frequency(bin));
        REQUIRE_THAT(from_fft, WithinRel(from_dtft, 1e-9));
    }
}

TEST_CASE("Coherent DFT of a real rendered sine matches the impulse response",
          "[signal][phaser][measurement]") {
    // The impulse-response instruments describe an LTI system. This closes the
    // loop on that claim by pushing an actual sine through `process()` and
    // measuring its output amplitude coherently. Agreement to 0.01 dB means
    // the frozen engine really is LTI and its impulse response really does
    // describe it.
    Config cfg;
    cfg.stages = 6;
    cfg.feedback = Phaser::kColorOnFeedback;

    const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);

    const int window = 48000;   // 1 s, so `cycles` is a frequency in whole Hz
    const int settle = 16384;   // past the closed-loop response's useful support

    for (int hz : {110, 400, 963, 2500, 7000}) {
        const double measured = coherent_sine_amplitude(cfg, hz, window, settle);
        const double predicted = magnitude_at(h, static_cast<double>(hz));
        REQUIRE_THAT(db(measured), WithinAbs(db(predicted), 0.01));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 1 — notch position, Small Stone mode
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Notch positions match the shipped digital law in Small Stone mode",
          "[signal][phaser][notch]") {
    Config cfg;   // stages 4, centre 400 Hz, feedback 0, mix 0.5 — the spec's
                  // reference configuration, by way of the shipped defaults.
    const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);

    REQUIRE(Phaser::notch_count(kSmallStoneStages) == 2);

    for (int k = 1; k <= Phaser::notch_count(kSmallStoneStages); ++k) {
        const double predicted = Phaser::notch_frequency_hz(
            k, kSmallStoneStages, kRefCenterHz, kSr);
        // A ±10 % bracket around the prediction contains exactly one null (the
        // neighbouring notches are 5.8x away at N = 4), so the search is
        // unimodal inside it and cannot slide onto the wrong notch.
        const double measured = refine_minimum(h, predicted * 0.9, predicted * 1.1);

        // Two orders tighter than the ±2 % the spec asked for. The law is not
        // an approximation to this filter; it is its closed form.
        REQUIRE_THAT(measured, WithinRel(predicted, 1e-4));

        // ...and it is a real null, not a shallow dip. The spec's floor is
        // 20 dB; exact cancellation at mix = 0.5 delivers far more, and
        // asserting the generous number would let a genuine mix-law regression
        // sneak past. 60 dB is still 40 dB above the criterion.
        const double depth_db = db(1.0 / magnitude_at(h, measured));
        REQUIRE(depth_db > 60.0);
    }
}

TEST_CASE("A notch sits exactly where the cascade phase reaches pi",
          "[signal][phaser][notch]") {
    // Ground truth INDEPENDENT of any notch formula. A notch is a cancellation
    // between the dry path and a unity-magnitude wet path, which can only
    // happen where the wet path's phase is an odd multiple of pi. So: measure
    // the notches from the mix = 0.5 response, then read the phase of the
    // mix = 1.0 (bare cascade) response at those same frequencies. If the
    // implementation, the digital law and this physics ever disagree, this is
    // the test that says which one is wrong.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        for (double fc : {100.0, kRefCenterHz, 2000.0}) {
            Config notched;
            notched.stages = stages;
            notched.center_hz = fc;
            Config wet = notched;
            wet.mix = 1.0;

            const auto h_notched = frozen_impulse_response(notched, kIrLenOpenLoop);
            const auto h_wet = frozen_impulse_response(wet, kIrLenOpenLoop);

            for (int k = 1; k <= Phaser::notch_count(stages); ++k) {
                const double predicted =
                    Phaser::notch_frequency_hz(k, stages, fc, kSr);
                const double measured =
                    refine_minimum(h_notched, predicted * 0.9, predicted * 1.1);

                REQUIRE_THAT(measured, WithinRel(predicted, 1e-4));

                // The bare cascade is unity magnitude everywhere...
                REQUIRE_THAT(std::abs(response_at(h_wet, measured)),
                             WithinAbs(1.0, 1e-6));
                // ...and exactly out of phase with dry at the null.
                REQUIRE_THAT(std::cos(std::arg(response_at(h_wet, measured))),
                             WithinAbs(-1.0, 1e-6));
            }
        }
    }
}

TEST_CASE("Analog prototype law versus the shipped digital law",
          "[signal][phaser][notch][spec-defect]") {
    // SPEC DEFECT, with the arithmetic. Acceptance tests 1 and 2 ask for
    // measured notches within +/-2 % of the CITED ANALOG law
    // `f_k = fc·tan((2k-1)pi/2N)`. The shipped stage is that prototype's
    // bilinear/TPT discretisation, whose notches are
    // `(fs/pi)·arctan(tan(pi·fc/fs)·tan((2k-1)pi/2N))`. Where the prewarp is
    // mild the two agree and the criterion is meetable; where it is not, no
    // correct implementation can pass. Both halves are asserted here so the
    // finding survives as an executable fact rather than a comment.

    // Half one — at the spec's own reference centre the laws agree, and the
    // prototype is a legitimate mental model there.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        for (int k = 1; k <= Phaser::notch_count(stages); ++k) {
            const double digital =
                Phaser::notch_frequency_hz(k, stages, kRefCenterHz, kSr);
            const double analog =
                Phaser::notch_frequency_analog_hz(k, stages, kRefCenterHz);
            REQUIRE(std::abs(analog - digital) / digital < 0.02);
        }
    }

    // Half two — inside the catalog's own `center_hz` range (100..2000 Hz) the
    // prototype breaks the +/-2 % criterion, and it does so at the SMALL STONE
    // stage count, not only at the exotic ones.
    {
        const double digital = Phaser::notch_frequency_hz(2, 4, 2000.0, kSr);
        const double analog = Phaser::notch_frequency_analog_hz(2, 4, 2000.0);
        REQUIRE(std::abs(analog - digital) / digital > 0.02);
    }
    {
        const double digital = Phaser::notch_frequency_hz(6, 12, 2000.0, kSr);
        const double analog = Phaser::notch_frequency_analog_hz(6, 12, 2000.0);
        REQUIRE(std::abs(analog - digital) / digital > 0.25);
    }

    // And the reason it is the prototype that is wrong rather than the code:
    // at `center_hz = 2000` with `depth = 100 %` the sweep reaches fc = 4 kHz,
    // where the prototype predicts a notch ABOVE Nyquist — a notch that cannot
    // exist. The digital law's arctan is bounded by pi/2, so it places every
    // notch strictly below Nyquist at any fc.
    REQUIRE(Phaser::notch_frequency_analog_hz(6, 12, 4000.0) > kSr / 2.0);
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2)
        for (int k = 1; k <= Phaser::notch_count(stages); ++k)
            for (double fc : {20.0, 400.0, 4000.0, 20000.0})
                REQUIRE(Phaser::notch_frequency_hz(k, stages, fc, kSr) < kSr / 2.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 2 — notch count scales with stage count
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Notch count is half the stage count",
          "[signal][phaser][notch]") {
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        Config cfg;
        cfg.stages = stages;
        const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
        const auto s = spectrum_of(h);

        // 0.73 Hz bins. The narrowest null in this configuration — the lowest
        // notch at 12 stages — is about +/-3 Hz wide at its 20 dB point, so the
        // grid resolves every one of them; the widest are hundreds of Hz.
        // 25 dB rather than the spec's 20 dB as the run threshold, purely to
        // stay clear of the 0 dB peaks between notches.
        REQUIRE(count_notches(s, 25.0) == Phaser::notch_count(stages));

        // Each of those notches is where the law says it is.
        for (int k = 1; k <= Phaser::notch_count(stages); ++k) {
            const double predicted =
                Phaser::notch_frequency_hz(k, stages, kRefCenterHz, kSr);
            const double measured =
                refine_minimum(h, predicted * 0.95, predicted * 1.05);
            REQUIRE_THAT(measured, WithinRel(predicted, 1e-4));
        }
    }
}

TEST_CASE("Adding stages interleaves new notches without moving the old ones",
          "[signal][phaser][notch]") {
    // The sonic claim behind multi-stage phasers: more stages reads as DENSER,
    // not merely different. Concretely, the N = 12 notch set contains the
    // N = 4 set — `tan((2k-1)pi/2N)` at (k=2,N=12) and (k=5,N=12) reproduce
    // (k=1,N=4) and (k=2,N=4) exactly, because 3/24 = 1/8 and 9/24 = 3/8.
    for (auto [k4, k12] : {std::pair{1, 2}, std::pair{2, 5}}) {
        REQUIRE_THAT(Phaser::notch_frequency_hz(k4, 4, kRefCenterHz, kSr),
                     WithinRel(Phaser::notch_frequency_hz(k12, 12, kRefCenterHz, kSr),
                               1e-12));
    }

    // And the notch sequence is strictly increasing in k at every stage count,
    // which is what makes "interleaves" the right word.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2)
        for (int k = 2; k <= Phaser::notch_count(stages); ++k)
            REQUIRE(Phaser::notch_frequency_hz(k, stages, kRefCenterHz, kSr) >
                    Phaser::notch_frequency_hz(k - 1, stages, kRefCenterHz, kSr));
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 3 — the mix law
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Notch depth follows the mix cancellation law",
          "[signal][phaser][mix]") {
    // The spec asks only for "deepest at 0.5, less elsewhere". The law is a
    // closed form — depth = -20·log10|1 - 2·mix| — so assert the closed form.
    // A wrong mix law (equal-power, say, which is the tempting default for a
    // dry/wet control) puts sqrt(0.5) on each path at the midpoint and yields
    // NO null at all; a "less than" test would let that through as long as the
    // midpoint still happened to be the deepest point.
    const double f1 =
        Phaser::notch_frequency_hz(1, kSmallStoneStages, kRefCenterHz, kSr);

    double previous_depth = -1.0;
    for (double mix : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        Config cfg;
        cfg.mix = mix;
        const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);

        // The passband reference: the mix-0.5 response peaks at exactly 1.0
        // wherever the cascade phase is an even multiple of pi, and so does
        // every other mix. DC is one such point for any stage count.
        const double passband = magnitude_at(h, 0.0);
        REQUIRE_THAT(passband, WithinAbs(1.0, 1e-9));

        const double depth_db = db(passband / magnitude_at(h, f1));

        if (mix == 0.5) {
            // Exact cancellation: bounded only by arithmetic, not by physics.
            REQUIRE(depth_db > 100.0);
        } else {
            REQUIRE_THAT(depth_db,
                         WithinAbs(-db(std::abs(1.0 - 2.0 * mix)), 1e-6));
        }

        // Monotone rising into the midpoint, monotone falling out of it — the
        // spec's "monotonic falloff on each side", read as one pass.
        if (mix <= 0.5) REQUIRE(depth_db > previous_depth);
        previous_depth = depth_db;
    }

    // Notch POSITION does not depend on mix: the phase geometry is upstream of
    // the mixer. Checked at the two extremes of usable mix.
    for (double mix : {0.25, 0.75}) {
        Config cfg;
        cfg.mix = mix;
        const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);
        REQUIRE_THAT(refine_minimum(h, f1 * 0.9, f1 * 1.1), WithinRel(f1, 1e-4));
    }
}

TEST_CASE("Mix at 1.0 is a flat allpass rather than a deeper phaser",
          "[signal][phaser][mix]") {
    // The corollary of the mix law, and the one users get wrong: fully wet is
    // NOT more phaser. It is the bare cascade — spectrally flat, only its
    // phase moving.
    Config cfg;
    cfg.mix = 1.0;
    const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);
    for (double hz : {20.0, 165.0, 400.0, 965.0, 3000.0, 12000.0, 20000.0})
        REQUIRE_THAT(magnitude_at(h, hz), WithinAbs(1.0, 1e-6));
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 4 — the allpass stage is unity gain
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("A single TPT allpass stage is unity gain on noise",
          "[signal][phaser][allpass]") {
    // The identity the whole feedback-gain proof rests on, checked on the
    // SHIPPED stage — `TptFilterT::process_allpass` is what the cascade runs,
    // so there is no second copy of this filter to drift from.
    TptFilter64 stage;
    stage.prepare(kSr);
    stage.set_cutoff(kRefCenterHz);

    Xorshift32 rng(0x9E3779B9u);
    const int n = 1'000'000;
    const int settle = 4096;   // past the stage's own start-up transient
    double in_sq = 0.0, out_sq = 0.0;
    for (int i = 0; i < n + settle; ++i) {
        const double x = rng.next_bipolar<double>();
        const double y = stage.process_allpass(x);
        if (i >= settle) {
            in_sq += x * x;
            out_sq += y * y;
        }
    }
    const double in_rms = std::sqrt(in_sq / n);
    const double out_rms = std::sqrt(out_sq / n);
    REQUIRE_THAT(db(out_rms / in_rms), WithinAbs(0.0, 0.05));
}

TEST_CASE("The stage reproduces the spec's published worked trace",
          "[signal][phaser][allpass][spec-defect]") {
    // Section 6.1 of the spec publishes a hand trace of one sample through one
    // stage, offered as the thing "implementers can replay by hand ... to
    // confirm their AllpassStageT matches section 3.3 bit-for-bit". So it is
    // external ground truth for the coefficient formula — the one place in
    // this file where a number is restated rather than computed — and it is
    // worth checking exactly.
    //
    //   fc = 400 Hz, fs = 48 kHz, fresh state, x[0] = 1
    //   spec: g = tan(pi·400/48000) = 0.026186    <- correct
    //   spec: G = g/(1+g)           = 0.025519    <- SPEC DEFECT: 0.0255177
    //   spec: y_ap[0] = 2·G − 1     = −0.948962   <- follows from the above
    //   spec: s = y_lp + v = 2·v    =  0.051038   <- follows from the above
    //
    // One arithmetic slip in G, in its sixth decimal place, propagated through
    // the two figures derived from it. Tiny, but the trace's stated purpose is
    // bit-for-bit confirmation, and an implementer who trusts it will go
    // hunting for a 2.6e-6 discrepancy that is not in their code.
    const double g = std::tan(kPi * kRefCenterHz / kSr);
    const double G = g / (1.0 + g);

    // `g` as published is right.
    REQUIRE_THAT(g, WithinAbs(0.026186, 5e-7));

    // `G` as published is not, and this is by how much.
    REQUIRE_THAT(G, WithinAbs(0.0255177, 5e-8));
    REQUIRE(std::abs(G - 0.025519) > 1e-6);

    // The shipped stage agrees with the correct arithmetic to 1.4e-9 — and
    // that residual is itself a finding, not noise. `tpt_filter.hpp` spells its
    // pi as `SampleType{3.14159265358979323846f}`; the `f` suffix makes the
    // literal a FLOAT even in the `double` instantiation, so `TptFilter64`
    // computes its coefficient from a pi that is 2.78e-8 too large. The
    // effective corner frequency carries that same relative error.
    //
    // It is pre-existing, in a shared header this module does not own, and it
    // is 4.7e-7 of a cent of frequency — far below anything audible and four
    // orders below the 1e-4 the notch-position tests assert to. Recorded here
    // rather than worked around silently, and asserted against the exact
    // float-pi prediction so that FIXING the suffix fails this test loudly
    // instead of drifting past it.
    constexpr double kPiAsFloat = 3.14159274101257324219;   // (float) pi, widened
    const double wa_f = 2.0 * kSr * std::tan(2.0 * kPiAsFloat * kRefCenterHz /
                                             (2.0 * kSr));
    const double g_float_pi = wa_f / (2.0 * kSr + wa_f);

    TptFilter64 stage;
    stage.prepare(kSr);
    stage.set_cutoff(kRefCenterHz);
    const double y_ap = stage.process_allpass(1.0);

    REQUIRE_THAT(y_ap, WithinAbs(2.0 * g_float_pi - 1.0, 1e-15));
    REQUIRE_THAT(y_ap, WithinAbs(2.0 * G - 1.0, 2e-9));
    REQUIRE_THAT(kPiAsFloat / kPi - 1.0, WithinAbs(2.78e-8, 1e-10));

    // ...and the stage still differs from the SPEC's published figure by
    // essentially twice the slip in G, which is the signature that says "the
    // document is wrong here, not the code": the spec's error is a thousand
    // times the implementation's.
    REQUIRE_THAT(y_ap - (-0.948962), WithinAbs(2.0 * (G - 0.025519), 1e-8));
    REQUIRE(std::abs(2.0 * (G - 0.025519)) > 1000.0 * std::abs(2.0 * G - 1.0 - y_ap));

    // The trace's step 4: after four such stages the mixer sees
    // `out[0] = 0.5·x[0] + 0.5·y_ap[0]`, the impulse having passed through
    // four fresh stages in series.
    Config cfg;   // the trace's configuration is the shipped default
    const auto h = frozen_impulse_response(cfg, 8);
    REQUIRE_THAT(h[0], WithinAbs(0.5 + 0.5 * std::pow(2.0 * G - 1.0, 4), 1e-8));
}

TEST_CASE("The whole cascade is unity magnitude at every frequency",
          "[signal][phaser][allpass]") {
    // Stronger than the RMS statement above, and it is the version the gain
    // proof actually needs: |A(e^jw)| = 1 at EVERY w, not merely on average.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        Config cfg;
        cfg.stages = stages;
        cfg.mix = 1.0;
        const auto h = frozen_impulse_response(cfg, kIrLenOpenLoop);
        const auto s = spectrum_of(h);
        for (std::size_t bin = 0; bin < s.magnitude.size(); ++bin)
            REQUIRE_THAT(s.magnitude[bin], WithinAbs(1.0, 1e-6));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance tests 5 and 6 — the feedback bound (series laws 1 and 8)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Feedback worst-case gain equals the shipped registry bound",
          "[signal][phaser][feedback][gain]") {
    // The number Forge's registry row cites. `worst_case_gain()` is
    // 1/(1 - kFeedbackMax) = 10.0x = 20.0 dB, and it is an EQUALITY, not a
    // ceiling: |A| = 1 exactly, so the loop peaks at exactly 1/(1-|k|)
    // wherever its phase comes back around. Both halves are asserted, because
    // a one-sided "<=" would still pass if the feedback path were silently
    // broken.
    REQUIRE_THAT(Phaser::worst_case_gain(), WithinRel(10.0, 1e-12));
    REQUIRE_THAT(db(Phaser::worst_case_gain()), WithinAbs(20.0, 1e-9));

    for (double sign : {+1.0, -1.0}) {
        Config cfg;
        cfg.feedback = sign * Phaser::kFeedbackMax;
        cfg.mix = 1.0;   // the worst case over mix: 1/(1-k) > 1, so all-wet
                         // beats any blend with the unity-gain dry path.
        const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
        const auto s = spectrum_of(h);
        const double peak =
            *std::max_element(s.magnitude.begin(), s.magnitude.end());

        REQUIRE(peak <= Phaser::worst_case_gain() + 1e-3);
        // Attained. Bin sampling can only miss a peak, never overshoot it, and
        // the 0.73 Hz grid lands within 0.3 % of this loop's peak.
        REQUIRE(peak > 0.99 * Phaser::worst_case_gain());
    }
}

TEST_CASE("The gain bound holds across the whole parameter range",
          "[signal][phaser][feedback][gain]") {
    // Series law 8 asks for a bound the module's OWN tests assert across the
    // range, not at one flattering operating point. Every combination of the
    // extremes of stage count, feedback sign and magnitude, mix, and the
    // catalog's `center_hz` span.
    for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
         stages += 2) {
        for (double fb : {-Phaser::kFeedbackMax, -Phaser::kColorOnFeedback, 0.0,
                          Phaser::kColorOnFeedback, Phaser::kFeedbackMax}) {
            for (double mix : {0.0, 0.5, 1.0}) {
                for (double fc : {100.0, 2000.0}) {
                    Config cfg;
                    cfg.stages = stages;
                    cfg.feedback = fb;
                    cfg.mix = mix;
                    cfg.center_hz = fc;

                    const auto h =
                        frozen_impulse_response(cfg, kIrLenClosedLoop);
                    const auto s = spectrum_of(h);
                    const double peak =
                        *std::max_element(s.magnitude.begin(), s.magnitude.end());
                    REQUIRE(peak <= Phaser::worst_case_gain() + 1e-3);
                }
            }
        }
    }
}

TEST_CASE("Feedback cannot be pushed past the bound it was proved for",
          "[signal][phaser][feedback][gain]") {
    // The bound is `1/(1 - kFeedbackMax)`, so the clamp IS the proof's
    // hypothesis. If a caller could widen it the registry number would become
    // fiction, and at |k| >= 1 the supremum is infinite.
    Phaser engine;
    engine.prepare(kSr);
    for (float requested : {5.0f, 1.0f, 0.95f, -0.95f, -1.0f, -7.0f}) {
        engine.set_feedback(requested);
        REQUIRE(std::abs(engine.feedback()) <= Phaser::kFeedbackMax);
    }
    engine.set_feedback(static_cast<float>(Phaser::kColorOnFeedback));
    REQUIRE_THAT(engine.feedback(), WithinRel(Phaser::kColorOnFeedback, 1e-6));
}

TEST_CASE("Maximum feedback stays bounded on 30 seconds of full-scale noise",
          "[signal][phaser][feedback][stability]") {
    // The empirical companion to the small-gain argument, run on the modulating
    // engine (not the frozen one) so the time-varying case is covered too.
    Phaser engine;
    engine.prepare(kSr);
    engine.set_stage_count(Phaser::kMaxStages);
    engine.set_feedback(static_cast<float>(Phaser::kFeedbackMax));
    engine.set_mix(1.0f);
    engine.set_rate_hz(2.0);
    engine.set_depth(1.0f);
    engine.set_center_hz(kRefCenterHz);
    engine.set_stereo_spread(0.25f);
    engine.reset();

    Xorshift32 rng(0x1234567u);
    const int total = static_cast<int>(30.0 * kSr);
    const int block = 512;
    std::vector<double> in(block), ol, orr;

    double in_sq = 0.0, out_sq = 0.0;
    double peak_out = 0.0;
    for (int done = 0; done < total; done += block) {
        for (auto& v : in) v = rng.next_bipolar<double>();
        render(engine, in, ol, orr);
        for (int i = 0; i < block; ++i) {
            REQUIRE(std::isfinite(ol[static_cast<std::size_t>(i)]));
            REQUIRE(std::isfinite(orr[static_cast<std::size_t>(i)]));
            in_sq += in[static_cast<std::size_t>(i)] * in[static_cast<std::size_t>(i)];
            out_sq += ol[static_cast<std::size_t>(i)] * ol[static_cast<std::size_t>(i)];
            peak_out = std::max(peak_out, std::abs(ol[static_cast<std::size_t>(i)]));
        }
    }
    const double in_rms = std::sqrt(in_sq / total);
    const double out_rms = std::sqrt(out_sq / total);

    // RMS is bounded by the worst-case gain applied to the input RMS — a
    // necessary condition of the frequency-domain bound, checked in the time
    // domain on a signal that excites every frequency at once.
    REQUIRE(out_rms <= Phaser::worst_case_gain() * in_rms);
    REQUIRE(std::isfinite(peak_out));
    // And it does not merely stay finite: it resonates, which is the whole
    // point of the control.
    REQUIRE(out_rms > in_rms);
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 7 — stereo quadrature spread
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Stereo spread offsets the right channel sweep by a quarter cycle",
          "[signal][phaser][stereo]") {
    const double rate_hz = 1.0;
    const double depth = 0.9;   // short of 100 % so the sweep never reaches the
                                // `kSweepFloorHz` clamp, which would flatten
                                // the contour and hide a real phase error
    const int quarter = static_cast<int>(0.25 / rate_hz * kSr);
    const int frames = static_cast<int>(3.0 / rate_hz * kSr);

    const auto c = sweep_contours(frames, rate_hz, 0.25, depth, kRefCenterHz);

    // The right channel LEADS by a quarter cycle: its LFO phase is
    // `phase + 0.25`, so `fc_R(t) = fc_L(t + 0.25/rate)`.
    REQUIRE_THAT(correlation_at_lag(c.left, c.right, -quarter),
                 WithinAbs(1.0, 1e-3));
    // Quadrature: orthogonal at zero lag. Exact for any odd-symmetric shape —
    // a triangle's harmonics are all odd, and every one of them picks up a
    // quarter-turn whose cosine is zero.
    REQUIRE_THAT(correlation_at_lag(c.left, c.right, 0), WithinAbs(0.0, 1e-3));
}

TEST_CASE("Stereo spread at half a cycle inverts the sweep exactly",
          "[signal][phaser][stereo]") {
    // The sharpest available statement of the phase relationship, and it needs
    // no correlation: a triangle is odd-symmetric about its half cycle, so a
    // 0.5-cycle offset is an exact inversion and the two channels' corner
    // frequencies must sum to twice the centre, sample for sample.
    const auto c = sweep_contours(static_cast<int>(2.0 * kSr), 1.0, 0.5, 0.9,
                                  kRefCenterHz);
    for (std::size_t i = 0; i < c.left.size(); ++i)
        REQUIRE_THAT(c.left[i] + c.right[i], WithinAbs(2.0 * kRefCenterHz, 1e-9));
}

TEST_CASE("Zero stereo spread makes the two channels identical",
          "[signal][phaser][stereo]") {
    Phaser engine;
    engine.prepare(kSr);
    engine.set_stereo_spread(0.0f);
    engine.set_rate_hz(1.5);
    engine.set_feedback(static_cast<float>(Phaser::kColorOnFeedback));
    engine.reset();

    Xorshift32 rng(0xABCDEF01u);
    std::vector<double> in(8192);
    for (auto& v : in) v = rng.next_bipolar<double>();
    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    for (std::size_t i = 0; i < ol.size(); ++i) REQUIRE(ol[i] == orr[i]);
}

TEST_CASE("Saw stereo spread remains a literal phase offset",
          "[signal][phaser][stereo]") {
    constexpr double center = kRefCenterHz;
    const int frames = static_cast<int>(2.0 * kSr);
    const auto zero = sweep_contours(frames, 1.0, 0.0, 0.9, center, LfoWave::saw_up);
    const auto quarter = sweep_contours(frames, 1.0, 0.25, 0.9, center, LfoWave::saw_up);
    const auto half = sweep_contours(frames, 1.0, 0.5, 0.9, center, LfoWave::saw_up);

    double quarter_difference = 0.0;
    double half_inversion_error = 0.0;
    double spread_change = 0.0;
    for (std::size_t i = 0; i < zero.left.size(); ++i) {
        REQUIRE(zero.left[i] == zero.right[i]);
        quarter_difference =
            std::max(quarter_difference, std::abs(quarter.left[i] - quarter.right[i]));
        half_inversion_error =
            std::max(half_inversion_error, std::abs(half.left[i] + half.right[i] - 2.0 * center));
        spread_change =
            std::max(spread_change, std::abs(quarter.right[i] - half.right[i]));
    }
    REQUIRE(quarter_difference > 0.1 * center);
    REQUIRE(half_inversion_error > 0.1 * center);
    REQUIRE(spread_change > 0.1 * center);
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 8 — triangle versus sine sweep shape
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Triangle sweeps at a constant rate and sine lingers at its extremes",
          "[signal][phaser][lfo]") {
    const double rate_hz = 1.0;
    const double depth = 0.9;   // clear of the sweep floor, as above
    const int frames = static_cast<int>(2.0 / rate_hz * kSr);

    // Triangle: |d(fc)/dt| is one value, computed from the shipped mapping —
    // a triangle traverses 4 units of its bipolar range per cycle, and
    // `fc = center·(1 + depth·kSweepRangeRatio·lfo)`.
    {
        const auto c = sweep_contours(frames, rate_hz, 0.0, depth, kRefCenterHz,
                                      LfoWave::triangle);
        const double expected_step = 4.0 * rate_hz / kSr * depth *
                                     Phaser::kSweepRangeRatio * kRefCenterHz;

        int reversals = 0;
        for (std::size_t i = 2; i < c.left.size(); ++i) {
            const double step = std::abs(c.left[i] - c.left[i - 1]);
            if (std::abs(step - expected_step) > 1e-6 * expected_step) {
                // Only the frames straddling the two extrema may differ, where
                // the ramp reverses inside one sample.
                ++reversals;
                continue;
            }
            REQUIRE_THAT(step, WithinRel(expected_step, 1e-6));
        }
        // Two turning points per cycle, two cycles rendered.
        REQUIRE(reversals <= 4);
    }

    // Sine: stationary at the extremes, fastest through the middle.
    {
        const auto c = sweep_contours(frames, rate_hz, 0.0, depth, kRefCenterHz,
                                      LfoWave::sine);
        std::vector<double> step(c.left.size(), 0.0);
        for (std::size_t i = 1; i < c.left.size(); ++i)
            step[i] = std::abs(c.left[i] - c.left[i - 1]);

        const double max_step = *std::max_element(step.begin() + 1, step.end());

        const auto top = static_cast<std::size_t>(
            std::max_element(c.left.begin(), c.left.end()) - c.left.begin());
        const auto bottom = static_cast<std::size_t>(
            std::min_element(c.left.begin(), c.left.end()) - c.left.begin());

        // At the turning points the sweep is essentially stopped...
        REQUIRE(step[top] < 0.01 * max_step);
        REQUIRE(step[bottom] < 0.01 * max_step);
        // ...and it is moving fastest where it crosses the centre.
        const auto fastest = static_cast<std::size_t>(
            std::max_element(step.begin() + 1, step.end()) - step.begin());
        REQUIRE_THAT(c.left[fastest], WithinRel(kRefCenterHz, 0.01));

        // Which is the audible difference the doc block claims: a sine spends
        // longer near its extremes than a triangle does. Measured as the
        // fraction of frames within the top 10 % of the excursion.
        const auto dwell = [](const std::vector<double>& contour) {
            const double lo = *std::min_element(contour.begin(), contour.end());
            const double hi = *std::max_element(contour.begin(), contour.end());
            const double edge = hi - 0.1 * (hi - lo);
            return static_cast<double>(
                       std::count_if(contour.begin(), contour.end(),
                                     [edge](double v) { return v >= edge; })) /
                   static_cast<double>(contour.size());
        };
        const auto tri = sweep_contours(frames, rate_hz, 0.0, depth,
                                        kRefCenterHz, LfoWave::triangle);
        REQUIRE(dwell(c.left) > dwell(tri.left));
    }
}

TEST_CASE("The sweep mapping is linear in Hz and clamped at both ends",
          "[signal][phaser][lfo]") {
    // The documented mapping: `fc = center·(1 + depth·kSweepRangeRatio·lfo)`,
    // clamped into `[kSweepFloorHz, kSweepCeilingRatio·fs]`. Linear in Hz
    // rather than logarithmic because the OTA topology it models sweeps a
    // control CURRENT, which moves the corner linearly.
    {
        // Half depth, no clamping: the excursion is exactly proportional to
        // depth and symmetric about the centre.
        const auto c = sweep_contours(static_cast<int>(2.0 * kSr), 1.0, 0.0, 0.5,
                                      kRefCenterHz);
        const double hi = *std::max_element(c.left.begin(), c.left.end());
        const double lo = *std::min_element(c.left.begin(), c.left.end());
        REQUIRE_THAT(hi, WithinRel(kRefCenterHz * 1.5, 1e-6));
        REQUIRE_THAT(lo, WithinRel(kRefCenterHz * 0.5, 1e-6));
        REQUIRE_THAT(0.5 * (hi + lo), WithinRel(kRefCenterHz, 1e-9));
    }
    {
        // Full depth: the mapping reaches 0 Hz, where the stage would degenerate
        // to a sign flip, so the floor takes over. Documented behaviour, not an
        // accident — and it is the reason the shape tests above run at 90 %.
        const auto c = sweep_contours(static_cast<int>(2.0 * kSr), 1.0, 0.0, 1.0,
                                      kRefCenterHz);
        const double lo = *std::min_element(c.left.begin(), c.left.end());
        REQUIRE_THAT(lo, WithinAbs(Phaser::kSweepFloorHz, 1e-9));
        REQUIRE_THAT(*std::max_element(c.left.begin(), c.left.end()),
                     WithinRel(2.0 * kRefCenterHz, 1e-6));
    }
    {
        // And the ceiling: a high centre at full depth is held below Nyquist
        // with margin, so the bilinear prewarp never diverges.
        Phaser engine;
        engine.prepare(kSr);
        engine.set_center_hz(kSr);   // far past the ceiling on its own
        REQUIRE_THAT(engine.center_hz(),
                     WithinRel(Phaser::kSweepCeilingRatio * kSr, 1e-12));

        const auto c = sweep_contours(static_cast<int>(1.0 * kSr), 1.0, 0.0, 1.0,
                                      20000.0);
        REQUIRE(*std::max_element(c.left.begin(), c.left.end()) <=
                Phaser::kSweepCeilingRatio * kSr + 1e-9);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 9 — determinism
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Renders are bit-identical for the same parameters and input",
          "[signal][phaser][determinism]") {
    // No RNG is involved anywhere in this module — even the LFO's stochastic
    // shapes are seeded and rewound by `reset()` — so equality here is BIT
    // equality, not an epsilon.
    const auto run = [](LfoWave wave) {
        Phaser engine;
        engine.prepare(kSr);
        engine.set_stage_count(8);
        engine.set_rate_hz(3.0);
        engine.set_depth(0.8f);
        engine.set_feedback(static_cast<float>(Phaser::kColorOnFeedback));
        engine.set_stereo_spread(0.25f);
        engine.set_wave(wave);
        engine.reset();

        Xorshift32 rng(0x5EED0001u);
        std::vector<double> in(4096);
        for (auto& v : in) v = rng.next_bipolar<double>();
        std::vector<double> ol, orr;
        render(engine, in, ol, orr);
        ol.insert(ol.end(), orr.begin(), orr.end());
        return ol;
    };

    for (LfoWave wave : {LfoWave::triangle, LfoWave::sine,
                         LfoWave::sample_hold, LfoWave::smooth_random}) {
        const auto a = run(wave);
        const auto b = run(wave);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 10 — the RT contract
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Nothing allocates after construction",
          "[signal][phaser][rt]") {
    // Stronger than the spec's "allocation-free post-prepare()": every buffer
    // in this class is a fixed-size member, so `prepare()` does not allocate
    // either.
    auto engine = std::make_unique<Phaser>();
    std::vector<double> in(512, 0.25), ol(512), orr(512);

    require_allocates_no_memory([&] {
        engine->prepare(kSr);
        engine->reset();
        engine->set_stage_count(11);
        engine->set_rate_hz(4.0);
        engine->set_depth(0.7f);
        engine->set_center_hz(900.0);
        engine->set_feedback(0.8f);
        engine->set_mix(0.5f);
        engine->set_stereo_spread(0.25f);
        engine->set_wave(LfoWave::sine);
        engine->set_stagger_ratio(1.08);
        engine->set_seed(12345u);
        engine->process(in.data(), in.data(), ol.data(), orr.data(), 512);
        engine->process_mono(in.data(), ol.data(), 512);
        (void) engine->sweep_frequency_hz(1);
        (void) engine->stage_count();
    });
}

TEST_CASE("Process is safe when the output aliases the input",
          "[signal][phaser][rt]") {
    // Hosts pass the same buffer for in and out constantly. Each frame's input
    // is read into a local before its output is written, so in-place is exact —
    // asserted against a rendered-to-separate-buffers reference rather than
    // merely "not silent", because the classic symptom of getting this wrong is
    // a plausible-looking but different signal.
    Xorshift32 rng(0xFACEB00Cu);
    std::vector<double> in(2048);
    for (auto& v : in) v = rng.next_bipolar<double>();

    Phaser reference;
    reference.prepare(kSr);
    reference.set_feedback(0.8f);
    reference.set_stereo_spread(0.25f);
    reference.set_rate_hz(2.0);
    reference.reset();
    std::vector<double> ref_l, ref_r;
    render(reference, in, ref_l, ref_r);

    Phaser in_place;
    in_place.prepare(kSr);
    in_place.set_feedback(0.8f);
    in_place.set_stereo_spread(0.25f);
    in_place.set_rate_hz(2.0);
    in_place.reset();
    std::vector<double> a = in, b = in;
    in_place.process(a.data(), b.data(), a.data(), b.data(),
                     static_cast<int>(in.size()));

    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(a[i] == ref_l[i]);
        REQUIRE(b[i] == ref_r[i]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 11 — latency
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Latency is zero and an impulse produces output at sample zero",
          "[signal][phaser][latency]") {
    REQUIRE(Phaser::latency_samples() == 0);
    REQUIRE(PhaserStages::latency_samples() == 0);

    for (double mix : {0.5, 1.0}) {
        for (int stages = Phaser::kMinStages; stages <= Phaser::kMaxStages;
             stages += 2) {
            Config cfg;
            cfg.mix = mix;
            cfg.stages = stages;
            cfg.feedback = Phaser::kColorOnFeedback;
            const auto h = frozen_impulse_response(cfg, 64);
            REQUIRE(h[0] != 0.0);
        }
    }

    // The feedback tap's one-sample memory is INSIDE the loop and is not
    // reportable latency: with the loop open or closed, sample 0 is unaffected
    // by it, because at sample 0 there is no previous output to feed back.
    Config open_loop;
    Config closed_loop;
    closed_loop.feedback = Phaser::kFeedbackMax;
    REQUIRE(frozen_impulse_response(open_loop, 64)[0] ==
            frozen_impulse_response(closed_loop, 64)[0]);
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 12 — denormal safety
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("A decaying tail reaches exact zero rather than lingering subnormal",
          "[signal][phaser][denormal]") {
    // The slowest-decaying configuration the module offers: maximum feedback,
    // maximum stage count. `snap_to_zero` in each stage's integrator and on the
    // feedback memory is what turns the tail into exact zeros instead of a
    // subnormal drizzle that stalls the CPU on FTZ-less targets.
    //
    // The spec also asks for a release-mode TIMING probe here. A wall-clock
    // assertion is not deterministic on a shared CI runner, so the property the
    // timing probe stands in for — state reaching EXACT zero — is asserted
    // directly instead.
    Phaser engine;
    Config cfg;
    cfg.stages = Phaser::kMaxStages;
    cfg.feedback = Phaser::kFeedbackMax;
    cfg.mix = 1.0;
    configure_frozen(engine, cfg);

    const int total = static_cast<int>(5.0 * kSr);
    std::vector<double> in(static_cast<std::size_t>(total), 0.0);
    in[0] = 1.0;
    std::vector<double> ol, orr;
    render(engine, in, ol, orr);

    for (double v : ol) REQUIRE(std::fpclassify(v) != FP_SUBNORMAL);

    // Exact zero, with room to spare. The flush point is a physical quantity,
    // not a free parameter: this configuration takes 2.56 s to decay from a
    // unit impulse through `snap_to_zero`'s 1e-15 threshold, so the spec's 5 s
    // window leaves 2.4 s of margin. Asserting 1 s of margin keeps the test
    // meaningful without sitting on the edge of the measurement.
    const auto last_nonzero = static_cast<std::size_t>(
        std::find_if(ol.rbegin(), ol.rend(), [](double v) { return v != 0.0; })
            .base() -
        ol.begin());
    REQUIRE(ol.size() - last_nonzero > static_cast<std::size_t>(kSr));
    for (std::size_t i = last_nonzero; i < ol.size(); ++i) REQUIRE(ol[i] == 0.0);

    // And the flush point moves the way the loop says it should — longer with
    // more stages (more group delay per trip round the loop) and longer with
    // more feedback (less decay per trip). A guard that lost its threshold
    // would flush instantly and break this ordering, not just the margin above.
    const auto flush_sample = [](int stages, double feedback) {
        Config c;
        c.stages = stages;
        c.feedback = feedback;
        c.mix = 1.0;
        const auto h = frozen_impulse_response(c, static_cast<int>(5.0 * kSr));
        return static_cast<std::size_t>(
            std::find_if(h.rbegin(), h.rend(), [](double v) { return v != 0.0; })
                .base() -
            h.begin());
    };
    REQUIRE(flush_sample(Phaser::kMaxStages, Phaser::kFeedbackMax) >
            flush_sample(Phaser::kMinStages, Phaser::kFeedbackMax));
    REQUIRE(flush_sample(Phaser::kMaxStages, Phaser::kFeedbackMax) >
            flush_sample(Phaser::kMaxStages, Phaser::kColorOnFeedback));

    // Feeding silence into an already-silent instance stays exactly silent —
    // the feedback memory has been snapped too, not just the stage integrators.
    std::vector<double> more(1024, 0.0), m_l, m_r;
    render(engine, more, m_l, m_r);
    for (double v : m_l) REQUIRE(v == 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Acceptance test 13 — stage-count clamping
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Stage count clamps into range and rounds down to even",
          "[signal][phaser][stages]") {
    // Documented behaviour, not a silent no-op: out-of-range and odd requests
    // resolve to a specific value that `stage_count()` reports back.
    Phaser engine;
    engine.prepare(kSr);

    const std::pair<int, int> cases[] = {
        {-5, 4}, {0, 4},  {3, 4},  {4, 4},   {5, 4},   {6, 6},
        {7, 6},  {9, 8},  {11, 10}, {12, 12}, {13, 12}, {100, 12},
    };
    for (auto [requested, expected] : cases) {
        engine.set_stage_count(requested);
        REQUIRE(engine.stage_count() == expected);
        REQUIRE(engine.stage_count() % 2 == 0);
        REQUIRE(engine.stage_count() >= Phaser::kMinStages);
        REQUIRE(engine.stage_count() <= Phaser::kMaxStages);
    }

    // And the clamped value is what actually runs: an odd request produces the
    // notch count of the EVEN value it resolved to, not of the value asked for.
    for (auto [requested, expected] : {std::pair{5, 4}, std::pair{11, 10}}) {
        Config cfg;
        cfg.stages = requested;
        const auto s = spectrum_of(frozen_impulse_response(cfg, kIrLenClosedLoop));
        REQUIRE(count_notches(s, 25.0) == Phaser::notch_count(expected));
    }

    REQUIRE(Phaser::kStageCountDefault == kSmallStoneStages);
    REQUIRE(Phaser().stage_count() == Phaser::kStageCountDefault);
}

// ═══════════════════════════════════════════════════════════════════════════
// Anti-aliasing policy (series law 4) — the claim, asserted
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("The module is exactly linear so there is nothing to alias",
          "[signal][phaser][aliasing]") {
    // Series law 4 asks for an oversampling policy wherever a nonlinearity
    // aliases. The policy here is "none needed", and that is a claim about the
    // code, so it gets asserted rather than asserted-in-prose.
    //
    // Part one: a pure tone produces no harmonics. Measured on the frozen
    // engine at MAXIMUM feedback, because a feedback loop is exactly where a
    // stray nonlinearity would hide.
    Config cfg;
    cfg.stages = 8;
    cfg.feedback = Phaser::kFeedbackMax;
    cfg.mix = Phaser::kMixDefault;

    const int window = 48000;
    const int settle = 16384;
    const int fundamental = 997;   // prime, so no harmonic lands exactly on a
                                   // notch and gets flattered by the notch
                                   // rather than by the module's linearity
    {
        // ONE tone rendered, then the harmonic bins of THAT render read. (The
        // wrong way to do this is to render each harmonic separately and read
        // its own bin, which measures the response to a tone that was never
        // present.)
        Phaser engine;
        configure_frozen(engine, cfg);
        const double w = 2.0 * kPi * static_cast<double>(fundamental) / kSr;
        std::vector<double> in(static_cast<std::size_t>(settle + window));
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = std::sin(w * static_cast<double>(i));
        std::vector<double> ol, orr;
        render(engine, in, ol, orr);

        const auto bin_amplitude = [&](int cycles) {
            std::complex<double> acc(0.0, 0.0);
            for (int i = 0; i < window; ++i) {
                const double phase = -2.0 * kPi * static_cast<double>(cycles) *
                                     static_cast<double>(i) /
                                     static_cast<double>(window);
                acc += ol[static_cast<std::size_t>(settle + i)] *
                       std::complex<double>(std::cos(phase), std::sin(phase));
            }
            return 2.0 * std::abs(acc) / static_cast<double>(window);
        };

        const double first = bin_amplitude(fundamental);
        REQUIRE(first > 0.1);
        for (int harmonic : {2, 3, 4, 5})
            REQUIRE(db(bin_amplitude(fundamental * harmonic) / first) < -100.0);
    }

    // Part two: superposition and homogeneity, the definition of linearity,
    // checked on the MODULATING engine — a linear time-varying system is still
    // linear, which is why sweeping coefficients needs no oversampling either.
    const auto render_with = [](const std::vector<double>& in) {
        Phaser engine;
        engine.prepare(kSr);
        engine.set_stage_count(8);
        engine.set_feedback(static_cast<float>(Phaser::kFeedbackMax));
        engine.set_rate_hz(5.0);
        engine.set_depth(1.0f);
        engine.reset();
        std::vector<double> ol, orr;
        render(engine, in, ol, orr);
        return ol;
    };

    Xorshift32 rng(0xC0FFEEu);
    std::vector<double> x(4096), y(4096), sum(4096);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = rng.next_bipolar<double>();
        y[i] = rng.next_bipolar<double>();
        sum[i] = x[i] + y[i];
    }
    const auto rx = render_with(x);
    const auto ry = render_with(y);
    const auto rsum = render_with(sum);
    for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE_THAT(rsum[i], WithinAbs(rx[i] + ry[i], 1e-9));

    std::vector<double> scaled(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) scaled[i] = 7.5 * x[i];
    const auto rscaled = render_with(scaled);
    for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE_THAT(rscaled[i], WithinAbs(7.5 * rx[i], 1e-9));
}

// ═══════════════════════════════════════════════════════════════════════════
// The stagger extension, and the sample-type parity
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("Stagger is off by default and detunes the notch set when engaged",
          "[signal][phaser][stagger]") {
    Phaser engine;
    engine.prepare(kSr);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerDefault, 1e-12));

    // Off: every stage shares one corner, so the tangent law holds exactly —
    // which the notch tests above already depend on.
    // On: the stages' corners spread, the tangent law's premise (N IDENTICAL
    // sections) no longer holds, and the notches move. Undocumented as hardware
    // behaviour, so the only claim made for it is that it does something.
    const double f1 =
        Phaser::notch_frequency_hz(1, kSmallStoneStages, kRefCenterHz, kSr);

    Config staggered;
    staggered.stagger = 1.08;
    const auto h = frozen_impulse_response(staggered, kIrLenOpenLoop);
    const double moved = refine_minimum(h, f1 * 0.8, f1 * 1.3);
    REQUIRE(std::abs(moved - f1) / f1 > 0.01);

    // Bounded to its declared range, both ways.
    engine.set_stagger_ratio(5.0);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerMax, 1e-12));
    engine.set_stagger_ratio(0.1);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerMin, 1e-12));

    // Even fully staggered, the gain bound survives — every stage is still an
    // allpass, so |A| = 1 regardless of where the individual corners sit.
    Config worst;
    worst.stagger = Phaser::kStaggerMax;
    worst.stages = Phaser::kMaxStages;
    worst.feedback = Phaser::kFeedbackMax;
    worst.mix = 1.0;
    const auto s = spectrum_of(frozen_impulse_response(worst, kIrLenClosedLoop));
    REQUIRE(*std::max_element(s.magnitude.begin(), s.magnitude.end()) <=
            Phaser::worst_case_gain() + 1e-3);
}

TEST_CASE("The float and double instantiations agree on the physics",
          "[signal][phaser][parity]") {
    // `PhaserStages64` exists so an analysis path can measure a 200 dB null.
    // `PhaserStages` is what ships in a plugin. They must place their notches
    // in the same place, and the float one must still reach a null deep enough
    // to be the effect rather than a wobble.
    Config cfg;
    cfg.stages = 8;
    const auto h32 = frozen_impulse_response<PhaserStages>(cfg, kIrLenOpenLoop);
    const auto h64 = frozen_impulse_response<PhaserStages64>(cfg, kIrLenOpenLoop);

    for (int k = 1; k <= Phaser::notch_count(cfg.stages); ++k) {
        const double predicted =
            Phaser::notch_frequency_hz(k, cfg.stages, cfg.center_hz, kSr);
        const double f32 = refine_minimum(h32, predicted * 0.9, predicted * 1.1);
        const double f64 = refine_minimum(h64, predicted * 0.9, predicted * 1.1);
        REQUIRE_THAT(f32, WithinRel(f64, 1e-4));
        // Comfortably past the spec's 20 dB floor even in single precision.
        REQUIRE(db(magnitude_at(h32, 0.0) / magnitude_at(h32, f32)) > 60.0);
    }

    REQUIRE_THAT(PhaserStages::worst_case_gain(),
                 WithinRel(PhaserStages64::worst_case_gain(), 1e-12));
}

// ═══════════════════════════════════════════════════════════════════════════
// The shipped defaults are the documented preset
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("A default instance is the documented Small Stone preset",
          "[signal][phaser][defaults]") {
    Phaser engine;
    REQUIRE(engine.stage_count() == Phaser::kStageCountDefault);
    REQUIRE_THAT(engine.center_hz(), WithinRel(kRefCenterHz, 1e-12));
    REQUIRE_THAT(engine.mix(), WithinRel(Phaser::kMixDefault, 1e-12));
    REQUIRE_THAT(engine.feedback(), WithinAbs(Phaser::kColorOffFeedback, 1e-12));
    REQUIRE_THAT(engine.rate_hz(), WithinRel(Phaser::kRateDefaultHz, 1e-12));
    REQUIRE_THAT(engine.stereo_spread(), WithinAbs(0.0, 1e-12));
    REQUIRE(engine.wave() == LfoWave::triangle);
    REQUIRE_THAT(engine.stagger_ratio(), WithinRel(Phaser::kStaggerDefault, 1e-12));

    // The two documented "Color" positions are preset POINTS on a continuous
    // control, not a mode flag — both are reachable through the same setter.
    REQUIRE(Phaser::kColorOffFeedback < Phaser::kColorOnFeedback);
    REQUIRE(Phaser::kColorOnFeedback < Phaser::kFeedbackMax);

}

TEST_CASE("Colour feedback adds a resonant peak and does not deepen the notch",
          "[signal][phaser][feedback][spec-defect]") {
    // SPEC DEFECT, with the arithmetic. Spec section 3.5 says positive feedback
    // "sharpens/deepens notches ... audibly deeper, more resonant notches with
    // Color engaged". Measured on the shipped code at N = 4, fc = 400 Hz,
    // mix = 0.5, relative to each configuration's own peak:
    //
    //     feedback   deepest null   null at      response peak
    //     0.00        88.5 dB        964.6 Hz     1.000  (0.0 dB)
    //     0.65        19.9 dB       1025.3 Hz     1.929  (+5.7 dB)
    //     0.90        27.5 dB       1102.4 Hz     5.499 (+14.8 dB)
    //
    // Feedback makes the null SHALLOWER, MOVES it, and adds a large resonant
    // peak. "More resonant" is right; "deeper notches" is backwards. The
    // mechanism is one line of algebra: with the loop closed the wet path is
    // W = A/(1 - k·z^-1·A), whose magnitude is 1 only where
    // cos(angle(A) - w) = k/2. At k = 0 that is everywhere, which is what makes
    // exact cancellation possible at mix = 0.5; at k != 0 it is a handful of
    // isolated frequencies, so the two paths no longer carry equal amplitude at
    // the phase-inversion point and cannot fully cancel.
    //
    // This is also WHY every notch-position test above sets feedback to zero:
    // the tangent law is an OPEN-LOOP law. It describes the cascade, not the
    // loop around it.
    const double f1_open =
        Phaser::notch_frequency_hz(1, kSmallStoneStages, kRefCenterHz, kSr);

    struct Measured {
        double peak, peak_hz, audible_peak_hz, null_depth_db, null_hz;
    };
    const auto measure = [&](double fb) {
        Config cfg;
        cfg.feedback = fb;
        const auto h = frozen_impulse_response(cfg, kIrLenClosedLoop);
        const auto s = spectrum_of(h);
        Measured m{};
        const auto highest = static_cast<std::size_t>(
            std::max_element(s.magnitude.begin(), s.magnitude.end()) -
            s.magnitude.begin());
        m.peak = s.magnitude[highest];
        m.peak_hz = s.frequency(highest);

        // The largest response inside the audible band, which excludes both
        // exact-attainment endpoints below.
        const auto lo = s.magnitude.begin() +
                        static_cast<std::ptrdiff_t>(s.bin_for(20.0));
        const auto hi = s.magnitude.begin() +
                        static_cast<std::ptrdiff_t>(s.bin_for(20000.0));
        m.audible_peak_hz = s.frequency(static_cast<std::size_t>(
            std::max_element(lo, hi) - s.magnitude.begin()));
        // The deepest null of the whole response, wherever the loop has put it.
        const auto lowest =
            static_cast<std::size_t>(std::min_element(s.magnitude.begin() + 1,
                                                      s.magnitude.end()) -
                                     s.magnitude.begin());
        m.null_hz = refine_minimum(h, s.frequency(lowest) - 2.0 * s.bin_hz,
                                   s.frequency(lowest) + 2.0 * s.bin_hz);
        m.null_depth_db = db(m.peak / magnitude_at(h, m.null_hz));
        return m;
    };

    const auto off = measure(Phaser::kColorOffFeedback);
    const auto on = measure(Phaser::kColorOnFeedback);
    const auto maxed = measure(Phaser::kFeedbackMax);

    // Open loop: flat passband, and the null is exactly where the law says.
    REQUIRE_THAT(off.peak, WithinAbs(1.0, 1e-9));
    REQUIRE(off.null_depth_db > 60.0);

    // Colour on: a real resonant peak, rising with feedback...
    REQUIRE(on.peak > off.peak);
    REQUIRE(maxed.peak > on.peak);
    // ...whose height is the closed form, not a fitted number. At mix m the
    // peak is bounded by (1-m) + m/(1-k), and the bound is essentially
    // attained because the loop's phase returns to zero at a low enough
    // frequency that the wet term is nearly real and positive there.
    const auto inverted = measure(-Phaser::kColorOnFeedback);
    const auto inverted_max = measure(-Phaser::kFeedbackMax);

    for (auto [fb, m] : {std::pair{Phaser::kColorOnFeedback, on},
                         std::pair{Phaser::kFeedbackMax, maxed},
                         std::pair{-Phaser::kColorOnFeedback, inverted},
                         std::pair{-Phaser::kFeedbackMax, inverted_max}}) {
        // The bound depends on |k|, so both signs reach the same height. That
        // is not a coincidence to be tolerated, it is the bound being tight.
        const double bound = (1.0 - Phaser::kMixDefault) +
                             Phaser::kMixDefault / (1.0 - std::abs(fb));
        REQUIRE(m.peak <= bound + 1e-3);
        REQUIRE(m.peak > 0.995 * bound);
    }

    // And the notch is neither deeper nor where the open-loop law puts it.
    REQUIRE(on.null_depth_db < off.null_depth_db);
    REQUIRE(maxed.null_depth_db < off.null_depth_db);
    REQUIRE(on.null_hz > f1_open);
    REQUIRE(maxed.null_hz > on.null_hz);

    // WHERE the bound is attained is a closed form at both ends of the
    // spectrum, and it is opposite for the two signs. An allpass cascade is
    // exactly +1 at DC (each section is `(a+1)/(1+a) = 1`) and, for EVEN stage
    // counts, exactly +1 at Nyquist too (each section is `(a-1)/(1-a) = -1`,
    // and there is an even number of them). Meanwhile `z^-1` is +1 at DC and
    // -1 at Nyquist. So the loop term is exactly `+k` at DC and exactly `-k`
    // at Nyquist, and the denominator `1 - loop` hits its minimum `1 - |k|`:
    //
    //     positive feedback -> attained exactly at DC
    //     negative feedback -> attained exactly at Nyquist
    //
    // Not approached, attained — so this is asserted to 1e-6, not to a
    // grid-resolution tolerance.
    const double peak_bound = (1.0 - Phaser::kMixDefault) +
                              Phaser::kMixDefault * Phaser::worst_case_gain();
    REQUIRE_THAT(maxed.peak_hz, WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(maxed.peak, WithinRel(peak_bound, 1e-6));
    REQUIRE_THAT(inverted_max.peak_hz, WithinAbs(kSr / 2.0, 1.0));
    REQUIRE_THAT(inverted_max.peak, WithinRel(peak_bound, 1e-6));

    // Inside the audible band — away from both of those endpoints — positive
    // feedback's resonance tracks `fc`, which is what makes it read as a
    // vocal formant riding the sweep rather than as a bass or treble lift.
    // That is the "Color" sound.
    REQUIRE_THAT(on.audible_peak_hz, WithinRel(kRefCenterHz, 0.05));
    REQUIRE_THAT(maxed.audible_peak_hz, WithinRel(kRefCenterHz, 0.05));

    // Negative feedback instead leaves the audible band flattened: its
    // resonance is parked at Nyquist and its deepest null is much shallower.
    // No hardware source documents it; it is a catalog-only extension.
    REQUIRE(inverted.null_depth_db < on.null_depth_db);
}
TEST_CASE("phaser retains controls and recovers from non-finite audio",
          "[signal][phaser][nonfinite]") {
    for (double bad : {NAN, INFINITY, -INFINITY}) {
        Phaser a, b;
        for (auto* p : {&a, &b}) { p->prepare(kSr); p->set_rate_hz(1.2); p->set_depth(.7f);
            p->set_center_hz(913); p->set_feedback(.31f); p->set_mix(.62f);
            p->set_stereo_spread(.17f); p->set_stagger_ratio(1.13); p->reset(); }
        a.set_rate_hz(bad); a.set_depth(static_cast<float>(bad)); a.set_center_hz(bad);
        a.set_feedback(static_cast<float>(bad)); a.set_mix(static_cast<float>(bad));
        a.set_stereo_spread(static_cast<float>(bad)); a.set_stagger_ratio(bad);
        REQUIRE(a.center_hz() == b.center_hz()); REQUIRE(a.feedback() == b.feedback());
        double inl=bad,inr=.2,al=1,ar=1; a.process(&inl,&inr,&al,&ar,1);
        REQUIRE(al==0); REQUIRE(ar==0); b.reset();
        for(int i=0;i<64;++i){ double x=.2,ya=0,za=0,yb=0,zb=0; a.process(&x,&x,&ya,&za,1); b.process(&x,&x,&yb,&zb,1); REQUIRE(ya==yb); REQUIRE(za==zb); }
    }
}
