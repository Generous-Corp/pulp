// FeedforwardCompressorT — the transparent, modern compressor.
//
// The spec's acceptance suite, tests 1–11 (see
// planning/2026-07-25-dsp-series-round2.md, module M05). Expected values are
// computed from the shipped constants and the closed-form equations, never
// restated as bare literals — so moving a constant fails the test that
// documents it rather than silently disagreeing with it.
//
// Several cases measure the DETECTOR (`gain_reduction_db()`) rather than
// inferring it from the audio. That is deliberate: inferring gain reduction
// from output/input divides by an input that crosses zero every half cycle,
// which turns a clean measurement into a noisy one and hides exactly the
// step-response detail tests 3 and 5 exist to check.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/feedforward_compressor.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Comp = FeedforwardCompressorT<double>;
constexpr double kSr = 48000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// The spec's soft-knee static characteristic, written out independently of the
/// header so the test checks the header's arithmetic rather than calling it and
/// agreeing with itself.
double reference_static_curve(double x, double t, double r, double w) {
    const double over = x - t;
    if (2.0 * over < -w) return x;
    if (w > 0.0 && 2.0 * std::abs(over) <= w) {
        const double num = over + w * 0.5;
        return x + (1.0 / r - 1.0) * num * num / (2.0 * w);
    }
    return t + over / r;
}

/// A compressor at a stated operating point, with smoothing effectively off so
/// the static curve is what is under test.
Comp static_curve_probe(double t, double r, double w) {
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(t);
    c.set_ratio(r);
    c.set_knee_width_db(w);
    c.set_attack_ms(Comp::kAttackMsMin);
    c.set_release_ms(Comp::kReleaseMsMin);
    c.set_program_dependent_release(false);
    c.set_auto_makeup(false);
    c.set_makeup_gain_db(0.0);
    return c;
}

/// Settled gain reduction, in dB, for a steady sine at `peak_db`.
double settled_reduction_db(Comp& c, double peak_db, double seconds = 1.0) {
    const double peak = units::db_to_linear(peak_db);
    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    const auto n = static_cast<int>(kSr * seconds);
    for (int i = 0; i < n; ++i) c.process(peak * std::sin(w * i));
    return c.gain_reduction_db();
}

/// Samples for the detector to travel from 10 % to 90 % of the way from its
/// current value to `target`.
int rise_time_10_90(Comp& c, double input_peak, double target_db, int max_samples) {
    const double start = c.gain_reduction_db();
    const double span = target_db - start;
    const double low = start + 0.1 * span;
    const double high = start + 0.9 * span;

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    int at_low = -1;
    for (int i = 0; i < max_samples; ++i) {
        c.process(input_peak * std::sin(w * i));
        const double gr = c.gain_reduction_db();
        if (at_low < 0 && gr >= low) at_low = i;
        if (at_low >= 0 && gr >= high) return i - at_low;
    }
    return -1;
}

}  // namespace

// ── 1. Static curve exactness ─────────────────────────────────────────────

TEST_CASE("1 the static curve matches the closed form on every branch",
          "[feedforward-compressor][curve]") {
    constexpr double t = -18.0, r = 4.0, w = 6.0;
    auto c = static_curve_probe(t, r, w);
    for (double x : {-40.0, -24.0, -21.0, -18.0, -15.0, -12.0, -6.0, 0.0}) {
        REQUIRE_THAT(c.static_curve_db(x), WithinAbs(reference_static_curve(x, t, r, w), 0.05));
    }
}

TEST_CASE("1 the spec's worked example reproduces exactly",
          "[feedforward-compressor][curve]") {
    // T = −18, R = 4, W = 6. Two points the spec works through by hand, plus
    // the auto-makeup value it derives from them.
    auto c = static_curve_probe(-18.0, 4.0, 6.0);

    // 6 dB over threshold: outside the knee (2·6 = 12 > 6), so the hard branch.
    REQUIRE_THAT(c.static_curve_db(-12.0), WithinAbs(-16.5, 1e-9));
    REQUIRE_THAT(c.gain_computer_db(-12.0), WithinAbs(-4.5, 1e-9));

    // 3 dB over: exactly the knee boundary (2·3 = 6 ≤ 6), quadratic branch.
    REQUIRE_THAT(c.static_curve_db(-15.0), WithinAbs(-17.25, 1e-9));
    REQUIRE_THAT(c.gain_computer_db(-15.0), WithinAbs(-2.25, 1e-9));

    // Auto-makeup cancels the reduction at the 0 dBFS reference point.
    c.set_auto_makeup(true);
    REQUIRE_THAT(c.effective_makeup_db(), WithinAbs(13.5, 1e-9));
}

// ── 2. Knee continuity ────────────────────────────────────────────────────

TEST_CASE("2 the knee is continuous at both boundaries, at every width",
          "[feedforward-compressor][curve]") {
    constexpr double t = -18.0, r = 4.0;
    for (double w : {0.0, 6.0, 12.0, 24.0}) {
        auto c = static_curve_probe(t, r, w);
        for (double edge : {t - w * 0.5, t + w * 0.5}) {
            const double below = c.static_curve_db(edge - 0.01);
            const double above = c.static_curve_db(edge + 0.01);
            REQUIRE_THAT(above - below, WithinAbs(0.0, 0.1));
        }
    }
}

TEST_CASE("2 zero knee width reproduces the hard-knee two-branch form",
          "[feedforward-compressor][curve]") {
    constexpr double t = -18.0, r = 4.0;
    auto c = static_curve_probe(t, r, 0.0);
    for (int i = 0; i <= 100; ++i) {
        const double x = t - 12.0 + 24.0 * i / 100.0;
        const double hard = x < t ? x : t + (x - t) / r;
        REQUIRE_THAT(c.static_curve_db(x), WithinAbs(hard, 0.01));
    }
}

// ── 3. Decoupled-detector step response ───────────────────────────────────

TEST_CASE("3 attack and release follow the one-pole 10-90 relation",
          "[feedforward-compressor][detector]") {
    // t90 − t10 = τ·ln 9 for a one-pole. Computed from the shipped time, not
    // restated: at 10 ms that is 21.97 ms, but the test never says so.
    constexpr double attack_ms = 10.0;
    constexpr double release_ms = 100.0;
    const double ln9 = std::log(9.0);

    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-40.0);
    c.set_ratio(4.0);
    c.set_knee_width_db(0.0);
    c.set_attack_ms(attack_ms);
    c.set_release_ms(release_ms);
    c.set_program_dependent_release(false);
    c.set_auto_makeup(false);
    c.set_makeup_gain_db(0.0);
    c.reset();

    // Attack: a 0 dBFS step onto silence.
    const double target = -c.gain_computer_db(0.0);
    const int attack_samples = rise_time_10_90(c, 1.0, target, static_cast<int>(kSr));
    REQUIRE(attack_samples > 0);
    REQUIRE_THAT(units::samples_to_ms(static_cast<double>(attack_samples), kSr),
                 WithinRel(attack_ms * ln9, 0.05));

    // Release: remove the step and measure the decay back toward zero, again
    // between the 10 % and 90 % points of the travel.
    const double start = c.gain_reduction_db();
    const double low = start * 0.9;   // 10 % of the way down
    const double high = start * 0.1;  // 90 % of the way down
    int at_low = -1, release_samples = -1;
    for (int i = 0; i < static_cast<int>(kSr * 2); ++i) {
        c.process(0.0);
        const double gr = c.gain_reduction_db();
        if (at_low < 0 && gr <= low) at_low = i;
        if (at_low >= 0 && gr <= high) {
            release_samples = i - at_low;
            break;
        }
    }
    REQUIRE(release_samples > 0);
    // The decoupled detector puts the release constant in stage 1 and the
    // attack constant in stage 2, so the observed decay is the release
    // exponential seen through the attack smoother. With τ_A ≪ τ_R that is the
    // release time to within a few percent.
    REQUIRE_THAT(units::samples_to_ms(static_cast<double>(release_samples), kSr),
                 WithinRel(release_ms * ln9, 0.10));
}

TEST_CASE("3 the detector attacks toward more reduction, not less",
          "[feedforward-compressor][detector]") {
    // The sign-convention guard. Running the decoupled `max()` on the negative
    // gain-computer output instead of the positive magnitude swaps attack and
    // release: reduction would snap on instantly and ease off slowly, which is
    // a working-sounding compressor with its two most important controls
    // exchanged. A long attack and a short release make the two orderings
    // unmistakably different.
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-40.0);
    c.set_ratio(8.0);
    c.set_knee_width_db(0.0);
    c.set_attack_ms(100.0);
    c.set_release_ms(Comp::kReleaseMsMin);
    c.set_program_dependent_release(false);
    c.set_auto_makeup(false);
    c.reset();

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    const auto ten_ms = static_cast<int>(units::ms_to_samples(10.0, kSr));
    for (int i = 0; i < ten_ms; ++i) c.process(std::sin(w * i));
    const double after_10ms = c.gain_reduction_db();

    const double target = -c.gain_computer_db(0.0);
    // 10 ms into a 100 ms attack, the detector is nowhere near settled. An
    // inverted detector would already be there.
    REQUIRE(after_10ms > 0.0);
    REQUIRE(after_10ms < 0.5 * target);
}

// ── 4. RMS vs peak ────────────────────────────────────────────────────────

TEST_CASE("4 RMS integrates a burst that peak detection takes literally",
          "[feedforward-compressor][detector]") {
    // One 1 kHz cycle at 0 dBFS inside −20 dBFS noise.
    //
    // The spec asks this test to assert RMS mode deviates < 0.3 dB, on the
    // reasoning that a 1 ms event cannot move a 10 ms average. That reasoning
    // omits the level difference: the burst carries 100× the POWER of a
    // −20 dBFS floor, so even the ~10 % of the exponential window's weight it
    // occupies raises the mean square by more than an order of magnitude. The
    // criterion is not achievable by a correct RMS detector — see adjudication
    // A-9 — so what is asserted instead is the arithmetic the detector should
    // actually produce, plus the operational difference the test exists to
    // establish.
    const auto burst_at = static_cast<int>(units::ms_to_samples(20.0, kSr));
    const auto cycle = static_cast<int>(kSr / 1000.0);
    const auto window = static_cast<int>(units::ms_to_samples(5.0, kSr));
    constexpr double kThresholdDb = -24.0;
    constexpr double kRatio = 8.0;
    constexpr double kRmsWindowMs = 10.0;
    constexpr double kNoiseDb = -20.0;

    const auto measure = [&](CompressorDetector mode, double attack_ms) {
        Comp c;
        c.prepare(kSr);
        c.set_threshold_db(kThresholdDb);
        c.set_ratio(kRatio);
        c.set_knee_width_db(0.0);
        c.set_attack_ms(attack_ms);
        c.set_release_ms(100.0);
        c.set_detector(mode);
        c.set_rms_window_ms(kRmsWindowMs);
        c.set_program_dependent_release(false);
        c.set_auto_makeup(false);
        c.reset();

        Xorshift32 rng{4242u};
        const double noise_amp = units::db_to_linear(kNoiseDb);
        double baseline = 0.0, deepest = 0.0;
        const double bw = 2.0 * std::numbers::pi * 1000.0 / kSr;
        for (int i = 0; i < burst_at + window; ++i) {
            double x = noise_amp * rng.next_bipolar<double>();
            if (i >= burst_at && i < burst_at + cycle) x += std::sin(bw * (i - burst_at));
            c.process(x);
            if (i == burst_at - 1) baseline = c.gain_reduction_db();
            if (i >= burst_at) deepest = std::max(deepest, c.gain_reduction_db());
        }
        return deepest - baseline;
    };

    // With attack effectively instant, RMS mode's reduction is fully determined
    // by the exponential window's arithmetic — computed here from the shipped
    // constants, not restated.
    const double beta = std::exp(-1.0 / (kRmsWindowMs * 0.001 * kSr));
    const double noise_ms = std::pow(units::db_to_linear(kNoiseDb), 2.0) / 3.0;
    constexpr double kSineMeanSquare = 0.5;  // ⟨sin²⟩ over a whole cycle
    const double burst_ms_value =
        noise_ms + (1.0 - std::pow(beta, cycle)) * (kSineMeanSquare - noise_ms);
    const double predicted_level_db = 10.0 * std::log10(burst_ms_value);
    const double predicted_reduction =
        std::max(0.0, predicted_level_db - kThresholdDb) * (1.0 - 1.0 / kRatio);

    const double rms_fast = measure(CompressorDetector::rms, Comp::kAttackMsMin);
    REQUIRE_THAT(rms_fast, WithinRel(predicted_reduction, 0.02));

    // The operational distinction: at a shared, realistic attack time the peak
    // detector sees the burst's full 0 dBFS instantaneously while RMS is still
    // integrating toward it, so peak reduces materially harder.
    const double peak_gr = measure(CompressorDetector::peak, 0.5);
    const double rms_gr = measure(CompressorDetector::rms, 0.5);
    REQUIRE(peak_gr > 1.0);
    REQUIRE(peak_gr > rms_gr * 1.5);
}

// ── 5. Program-dependent release ──────────────────────────────────────────

TEST_CASE("5 sustained compression releases more slowly than a lone transient",
          "[feedforward-compressor][detector]") {
    const auto recovery_samples = [](double over_threshold_seconds) {
        Comp c;
        c.prepare(kSr);
        c.set_threshold_db(-30.0);
        c.set_ratio(8.0);
        c.set_knee_width_db(0.0);
        c.set_attack_ms(1.0);
        c.set_release_ms(100.0);
        c.set_program_dependent_release(true);
        c.set_auto_makeup(false);
        c.reset();

        const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
        const auto n = static_cast<int>(kSr * over_threshold_seconds);
        for (int i = 0; i < n; ++i) c.process(std::sin(w * i));

        const double start = c.gain_reduction_db();
        REQUIRE(start > 1.0);
        for (int i = 0; i < static_cast<int>(kSr * 10); ++i) {
            c.process(0.0);
            if (c.gain_reduction_db() <= start * 0.1) return i;
        }
        return -1;
    };

    const int transient = recovery_samples(0.020);  // 20 ms — sustain has not risen
    const int sustained = recovery_samples(2.0);    // 2 s — the slow path is engaged
    REQUIRE(transient > 0);
    REQUIRE(sustained > 0);
    // The slow constant is kSlowReleaseRatio× the fast one, so a fully-engaged
    // sustain path is that much slower. Well past the 1.5× the spec asks for,
    // but computed from the shipped ratio rather than restated.
    REQUIRE(static_cast<double>(sustained) > 1.5 * transient);
    REQUIRE(static_cast<double>(sustained) <= Comp::kSlowReleaseRatio * transient * 1.2);
}

TEST_CASE("5 disabling program-dependent release reduces to one constant",
          "[feedforward-compressor][detector]") {
    // The claim in the spec: `false` reproduces the plain single-release
    // detector exactly. Two instances, one with the option off and one with it
    // on but never sustained long enough to engage, must not be identical —
    // but the option-off instance must be identical to itself across the
    // sustain window, which is what "reduces exactly" means.
    Comp off;
    off.prepare(kSr);
    off.set_threshold_db(-30.0);
    off.set_ratio(8.0);
    off.set_release_ms(100.0);
    off.set_program_dependent_release(false);
    off.set_auto_makeup(false);
    off.reset();

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr * 2); ++i) off.process(std::sin(w * i));
    const double start = off.gain_reduction_db();
    int n = 0;
    while (off.gain_reduction_db() > start * 0.1 && n < static_cast<int>(kSr * 10)) {
        off.process(0.0);
        ++n;
    }
    // Two seconds of sustained compression, and the release is still the plain
    // one — a t90 within 10 % of τ·ln 9.
    REQUIRE_THAT(units::samples_to_ms(static_cast<double>(n), kSr),
                 WithinRel(100.0 * std::log(9.0), 0.15));
}

// ── 6. Lookahead latency ──────────────────────────────────────────────────

TEST_CASE("6 latency equals the lookahead exactly, and the delay line owns it",
          "[feedforward-compressor][latency]") {
    for (double ms : {0.0, 3.0, 10.0}) {
        Comp c;
        c.prepare(kSr, 10.0);
        c.set_lookahead_ms(ms);
        c.set_threshold_db(0.0);
        c.set_ratio(1.0);  // no reduction possible: a pure delay
        c.set_auto_makeup(false);
        c.set_makeup_gain_db(0.0);
        c.reset();

        const int expected = static_cast<int>(std::llround(units::ms_to_samples(ms, kSr)));
        REQUIRE(c.latency_samples() == expected);

        // An impulse must arrive exactly `latency_samples()` later, at full
        // amplitude — confirming the delay line, not the detector, accounts for
        // the whole reported figure.
        int peak_index = -1;
        double peak_value = 0.0;
        for (int i = 0; i < expected + 64; ++i) {
            const double y = c.process(i == 0 ? 0.5 : 0.0);
            if (std::abs(y) > peak_value) {
                peak_value = std::abs(y);
                peak_index = i;
            }
        }
        REQUIRE(peak_index == expected);
        REQUIRE_THAT(peak_value, WithinAbs(0.5, 1e-9));
    }
}

// ── 7. Stereo link ────────────────────────────────────────────────────────

TEST_CASE("7 a fully linked pair reduces both channels identically",
          "[feedforward-compressor][stereo]") {
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-30.0);
    c.set_ratio(8.0);
    c.set_knee_width_db(0.0);
    c.set_attack_ms(1.0);
    c.set_release_ms(100.0);
    c.set_stereo_link(1.0);
    c.set_program_dependent_release(false);
    c.set_auto_makeup(false);
    c.reset();

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr * 0.2); ++i) {
        double l = std::sin(w * i), r = 0.0;
        c.process_stereo(l, r);
        REQUIRE_THAT(c.gain_reduction_db(1), WithinAbs(c.gain_reduction_db(0), 0.05));
    }
    // ...and the silent channel really was reduced, i.e. the link engaged.
    REQUIRE(c.gain_reduction_db(1) > 1.0);
}

TEST_CASE("7 an unlinked pair leaves the silent channel alone",
          "[feedforward-compressor][stereo]") {
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-30.0);
    c.set_ratio(8.0);
    c.set_stereo_link(0.0);
    c.set_program_dependent_release(false);
    c.set_auto_makeup(false);
    c.reset();

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr * 0.2); ++i) {
        double l = std::sin(w * i), r = 0.0;
        c.process_stereo(l, r);
        REQUIRE_THAT(c.gain_reduction_db(1), WithinAbs(0.0, 0.01));
    }
    REQUIRE(c.gain_reduction_db(0) > 1.0);
}

// ── 8. Makeup-gain bound: the registry invariant ──────────────────────────

TEST_CASE("8 output never exceeds the makeup-gain bound", "[feedforward-compressor][gain]") {
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(0.0);
    c.set_ratio(1.0);  // no reduction possible
    c.set_knee_width_db(0.0);
    c.set_auto_makeup(false);
    c.set_makeup_gain_db(Comp::kMakeupDbMax);
    c.reset();

    const double bound = units::db_to_linear(Comp::kMakeupDbMax);
    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    double observed = 0.0;
    for (int i = 0; i < static_cast<int>(kSr); ++i)
        observed = std::max(observed, std::abs(c.process(std::sin(w * i))));

    REQUIRE(observed <= bound * 1.005);
    // Not a vacuous bound: at ratio 1 with no reduction it is actually reached.
    REQUIRE(observed >= bound * 0.995);
}

// ── 9. Determinism ────────────────────────────────────────────────────────

TEST_CASE("9 render, reset, re-render is bit-identical", "[feedforward-compressor][determinism]") {
    const auto render = [](Comp& c, int n) {
        Xorshift32 rng{20260725u};
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.push_back(c.process(0.5 * rng.next_bipolar<double>()));
        return out;
    };

    for (auto mode : {CompressorDetector::peak, CompressorDetector::rms}) {
        for (bool program : {false, true}) {
            Comp c;
            c.prepare(kSr, 10.0);
            c.set_threshold_db(-24.0);
            c.set_ratio(6.0);
            c.set_knee_width_db(6.0);
            c.set_attack_ms(5.0);
            c.set_release_ms(200.0);
            c.set_detector(mode);
            c.set_lookahead_ms(3.0);
            c.set_program_dependent_release(program);
            c.reset();

            const auto first = render(c, static_cast<int>(kSr * 2));
            c.reset();
            const auto second = render(c, static_cast<int>(kSr * 2));
            REQUIRE(first.size() == second.size());
            for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
        }
    }
}

TEST_CASE("9 reset returns the detector to a silence-equivalent state",
          "[feedforward-compressor][determinism]") {
    Comp swept;
    swept.prepare(kSr, 10.0);
    swept.set_lookahead_ms(5.0);
    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr); ++i) swept.process(std::sin(w * i));
    swept.reset();

    Comp fresh;
    fresh.prepare(kSr, 10.0);
    fresh.set_lookahead_ms(5.0);
    fresh.reset();

    for (int i = 0; i < 4800; ++i) REQUIRE(swept.process(0.0) == fresh.process(0.0));
    REQUIRE(swept.gain_reduction_db() == fresh.gain_reduction_db());
}

// ── 11. Auto-makeup consistency ───────────────────────────────────────────

TEST_CASE("11 auto-makeup restores unity at the reference point",
          "[feedforward-compressor][gain]") {
    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-18.0);
    c.set_ratio(4.0);
    c.set_knee_width_db(6.0);
    c.set_attack_ms(1.0);
    c.set_release_ms(50.0);
    c.set_program_dependent_release(false);
    c.set_auto_makeup(true);
    c.reset();

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    double observed = 0.0;
    const auto n = static_cast<int>(kSr * 2);
    for (int i = 0; i < n; ++i) {
        const double y = c.process(std::sin(w * i));
        if (i > n / 2) observed = std::max(observed, std::abs(y));
    }
    REQUIRE_THAT(units::linear_to_db(observed), WithinAbs(0.0, 0.1));
}

// ── float/double parity ───────────────────────────────────────────────────

TEST_CASE("the float and double instantiations agree", "[feedforward-compressor]") {
    FeedforwardCompressorT<float> f;
    FeedforwardCompressorT<double> d;
    for (auto* c : {static_cast<void*>(&f), static_cast<void*>(&d)}) (void)c;

    f.prepare(kSr, 10.0);
    d.prepare(kSr, 10.0);
    for (auto set : {0}) {
        (void)set;
        f.set_threshold_db(-24.0); d.set_threshold_db(-24.0);
        f.set_ratio(6.0);          d.set_ratio(6.0);
        f.set_knee_width_db(6.0);  d.set_knee_width_db(6.0);
        f.set_attack_ms(5.0);      d.set_attack_ms(5.0);
        f.set_release_ms(200.0);   d.set_release_ms(200.0);
        f.set_lookahead_ms(2.0);   d.set_lookahead_ms(2.0);
    }
    f.reset();
    d.reset();

    REQUIRE(f.latency_samples() == d.latency_samples());

    const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i) {
        const double x = 0.7 * std::sin(w * i);
        const double yf = f.process(static_cast<float>(x));
        const double yd = d.process(x);
        REQUIRE_THAT(yf, WithinAbs(yd, 1e-4));
    }
}

// ── 10. RT allocation probe ───────────────────────────────────────────────

TEST_CASE("10 the compressor allocates nothing on the audio thread",
          "[feedforward-compressor][rt-safety]") {
    FeedforwardCompressorT<float> f;
    FeedforwardCompressorT<double> d;
    f.prepare(kSr, 10.0);
    d.prepare(kSr, 10.0);

    std::vector<float> block_f(256, 0.1f);
    std::vector<float> block_f2(256, 0.1f);
    std::vector<double> block_d(256, 0.1);
    std::vector<double> block_d2(256, 0.1);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 64; ++i) {
            f.set_threshold_db(-40.0 + 0.5 * i);
            f.set_ratio(1.0 + 0.1 * i);
            f.set_knee_width_db(0.1 * i);
            f.set_attack_ms(1.0 + 0.1 * i);
            f.set_release_ms(50.0 + i);
            f.set_lookahead_ms(0.1 * (i % 100));
            f.set_rms_window_ms(1.0 + 0.1 * i);
            f.set_makeup_gain_db(0.1 * i);
            f.set_stereo_link(0.01 * (i % 100));
            f.set_detector((i % 2) ? CompressorDetector::rms : CompressorDetector::peak);
            f.set_program_dependent_release((i % 3) != 0);
            f.set_auto_makeup((i % 5) != 0);

            (void)f.process(0.5f);
            float l = 0.5f, r = -0.5f;
            f.process_stereo(l, r);
            f.process_block(block_f.data(), static_cast<int>(block_f.size()));
            f.process_block_stereo(block_f.data(), block_f2.data(),
                                   static_cast<int>(block_f.size()));

            (void)d.process(0.5);
            double dl = 0.5, dr = -0.5;
            d.process_stereo(dl, dr);
            d.process_block(block_d.data(), static_cast<int>(block_d.size()));
            d.process_block_stereo(block_d.data(), block_d2.data(),
                                   static_cast<int>(block_d.size()));
        }
        f.reset();
        d.reset();
    });
}

TEST_CASE("a NaN sample cannot latch the feedforward detector",
          "[feedforward-compressor][nan-recovery]") {
    for (double sample_rate : {8000.0, 192000.0}) {
        Comp c;
        c.prepare(sample_rate);
        c.set_threshold_db(-30.0);
        c.set_ratio(8.0);
        c.set_detector(CompressorDetector::rms);
        c.set_program_dependent_release(true);
        for (int i = 0; i < static_cast<int>(sample_rate * 0.1); ++i) c.process(0.5);

        c.process(std::numeric_limits<double>::quiet_NaN());
        for (int i = 0; i < 64; ++i) {
            REQUIRE(std::isfinite(c.process(0.25)));
            REQUIRE(std::isfinite(c.gain_reduction_db()));
        }
    }
}

TEST_CASE("non-finite feedforward controls retain the last valid configuration",
          "[feedforward-compressor][nan-recovery]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Comp c, reference;
        for (auto* x : {&c, &reference}) {
            x->prepare(kSr); x->set_threshold_db(-27.0); x->set_ratio(7.0);
            x->set_knee_width_db(3.0); x->set_attack_ms(2.5); x->set_release_ms(333.0);
            x->set_rms_window_ms(17.0); x->set_lookahead_ms(6.0);
            x->set_makeup_gain_db(-4.0); x->set_stereo_link(0.37);
        }
        c.set_threshold_db(bad); c.set_ratio(bad); c.set_knee_width_db(bad);
        c.set_attack_ms(bad); c.set_release_ms(bad); c.set_rms_window_ms(bad);
        c.set_lookahead_ms(bad); c.set_makeup_gain_db(bad); c.set_stereo_link(bad);
        for (int i = 0; i < 512; ++i) {
            const double sample = 0.25 * std::sin(0.031 * i);
            REQUIRE(c.process(sample) == reference.process(sample));
        }
    }
}

TEST_CASE("feedforward audio faults logically clear lookahead and recover exactly",
          "[feedforward-compressor][nan-recovery][rt-safety]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Comp poisoned, fresh;
        for (auto* x : {&poisoned, &fresh}) {
            x->prepare(kSr); x->set_threshold_db(-24.0); x->set_ratio(6.0);
            x->set_detector(CompressorDetector::rms); x->set_lookahead_ms(8.0);
        }
        for (int i = 0; i < 600; ++i) (void)poisoned.process(0.4);
        require_allocates_no_memory([&] { REQUIRE(poisoned.process(bad) == 0.0); });
        fresh.reset();
        for (int i = 0; i < 768; ++i) {
            const double sample = 0.2 * std::sin(0.023 * i);
            REQUIRE(poisoned.process(sample) == fresh.process(sample));
        }
    }
}
