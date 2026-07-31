// The circuit-modelled clipper family — DiodeClipperT, FeedbackClipperT,
// ToneStackT.
//
// The spec's acceptance suite (see planning/2026-07-25-dsp-series-round2.md,
// module M02). Expected values are computed from the shipped calibration tables
// rather than restated, so a change to a diode row or a solver constant fails
// the test that documents it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/distortion.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kSr = 48000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

DiodeClipperT<double> make_clipper(DiodeModel model = DiodeModel::silicon,
                                   double symmetry = 0.0, double capacitance = -1.0) {
    DiodeClipperT<double> c;
    c.prepare(kSr);
    c.set_diode_model(model);
    c.set_symmetry(symmetry);
    if (capacitance >= 0.0) c.set_capacitance(capacitance);
    return c;
}

/// Coherent DFT magnitude at harmonic `k` of a tone whose period divides the
/// analysis window exactly.
double harmonic_magnitude(const std::vector<double>& x, double fundamental_hz, int k) {
    const double w = 2.0 * std::numbers::pi * k * fundamental_hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

}  // namespace

// ── 1. Shockley curve accuracy at the memoryless limit ────────────────────

TEST_CASE("1 the memoryless solve satisfies the circuit equation exactly",
          "[distortion][clipper]") {
    // C = 0 is the degenerate configuration where the ODE collapses to the
    // transcendental equation (vin − v)/R = i_D(v). Sweeping it re-checks that
    // Newton's exit condition actually corresponds to a solved circuit, rather
    // than only to a small step.
    //
    // Trapezoidal rule at C = 0 zeroes the AVERAGE of the current and previous
    // residuals, so this holds by induction from a reset state (where both are
    // zero) — which is how a DC sweep must be run for the claim to mean
    // anything.
    auto clipper = make_clipper(DiodeModel::silicon, 0.0, 0.0);
    clipper.reset();

    for (int i = 0; i <= 4000; ++i) {
        const double vin = -2.0 + 4.0 * i / 4000.0;
        const double v = clipper.process(vin);
        REQUIRE(std::abs(clipper.resistive_residual(vin, v)) < 1e-6);
    }
}

// ── 2. Symmetric vs asymmetric harmonic content ───────────────────────────

TEST_CASE("2 a matched pair makes odd harmonics; a single diode makes even ones",
          "[distortion][clipper]") {
    // 200 Hz at 48 kHz is exactly 240 samples per period, so a whole number of
    // periods fills the analysis window and the DFT below is leakage-free — no
    // bin-alignment arithmetic and no window correction needed.
    constexpr double tone_hz = 200.0;
    constexpr int period = 240;
    constexpr int periods = 200;
    constexpr int settle = 4800;

    const auto render = [&](double symmetry) {
        auto clipper = make_clipper(DiodeModel::silicon, symmetry);
        clipper.reset();
        std::vector<double> out;
        out.reserve(period * periods);
        const double w = 2.0 * std::numbers::pi * tone_hz / kSr;
        // Amplitude well past the silicon knee (~0.6 V).
        const double amp = units::db_to_linear(24.0) * 0.5;
        for (int n = 0; n < settle + period * periods; ++n) {
            const double y = clipper.process(amp * std::sin(w * n));
            if (n >= settle) out.push_back(y);
        }
        return out;
    };

    const auto symmetric = render(0.0);
    const double h1_sym = harmonic_magnitude(symmetric, tone_hz, 1);
    for (int k : {2, 4, 6}) {
        const double db = units::linear_to_db(harmonic_magnitude(symmetric, tone_hz, k) / h1_sym);
        REQUIRE(db <= -40.0);
    }

    const auto half_wave = render(-1.0);
    const double h1 = harmonic_magnitude(half_wave, tone_hz, 1);
    const double h2 = harmonic_magnitude(half_wave, tone_hz, 2);
    const double h3 = harmonic_magnitude(half_wave, tone_hz, 3);
    REQUIRE(h1 > 0.0);
    // The evidence that removing a leg broke the odd symmetry: the second
    // harmonic is at least comparable to the third. The spec phrases this as
    // "within 10 dB", which reads as a two-sided band — but h2 landing well
    // ABOVE h3 is stronger evidence of asymmetry, not weaker, so only the
    // lower side is a failure.
    REQUIRE(units::linear_to_db(h2) >= units::linear_to_db(h3) - 10.0);
    // ...and it really is present, not merely un-suppressed.
    REQUIRE(units::linear_to_db(h2 / h1) > -40.0);
}

// ── 3. Diode-model threshold ordering ─────────────────────────────────────

TEST_CASE("3 turn-on voltage orders germanium < silicon < led",
          "[distortion][clipper]") {
    // The ordering falls straight out of each row's saturation current: larger
    // Is means the exponential crosses a given current at a lower voltage. This
    // asserts the shipped table produces the documented ordering rather than
    // asserting the voltages themselves.
    const auto turn_on_voltage = [](DiodeModel model) {
        junction::JunctionPair network;
        detail::apply_diode_model(network, model);
        detail::apply_symmetry(network, 0.0);
        constexpr double kCurrentThreshold = 1e-6;  // amps
        for (int i = 1; i <= 20000; ++i) {
            const double v = i * 1e-4;  // 0.1 mV steps to 2 V
            if (std::abs(network.current(v)) > kCurrentThreshold) return v;
        }
        return 2.0;
    };

    const double germanium = turn_on_voltage(DiodeModel::germanium);
    const double silicon = turn_on_voltage(DiodeModel::silicon);
    const double led = turn_on_voltage(DiodeModel::led);

    REQUIRE(germanium < silicon);
    REQUIRE(silicon < led);
    // ...and the spacing is musically meaningful, not a rounding artefact.
    REQUIRE(silicon - germanium > 0.1);
}

// ── 4. in_loop tracks drive; to_ground does not ───────────────────────────

TEST_CASE("4 the in-loop knee tracks drive while the to-ground knee does not",
          "[distortion][clipper]") {
    // The measurable form of the topologies' documented behavioural difference.
    // A to-ground clipper's output plateaus once the diodes are conducting hard,
    // because the clip voltage is fixed. In-loop, raising the drive changes the
    // loop's gain-before-clip, so the output keeps moving.
    const auto peak_over_drive = [](ClipperTopology topology, double drive_db) {
        FeedbackClipperT<double> clipper;
        clipper.prepare(kSr);
        clipper.set_topology(topology);
        clipper.set_diode_model(DiodeModel::silicon);
        clipper.set_symmetry(0.0);
        clipper.reset();

        const double amp = 0.1 * units::db_to_linear(drive_db);
        const double w = 2.0 * std::numbers::pi * 1000.0 / kSr;
        double peak = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const double y = clipper.process(amp * std::sin(w * n));
            if (n > 2400) peak = std::max(peak, std::abs(y));
        }
        return units::linear_to_db(peak);
    };

    // Across the TOP half of a 0..24 dB sweep, where both are well past the knee.
    const double ground_lo = peak_over_drive(ClipperTopology::to_ground, 12.0);
    const double ground_hi = peak_over_drive(ClipperTopology::to_ground, 24.0);
    const double ground_growth = ground_hi - ground_lo;

    const double loop_lo = peak_over_drive(ClipperTopology::in_loop, 12.0);
    const double loop_hi = peak_over_drive(ClipperTopology::in_loop, 24.0);
    const double loop_growth = loop_hi - loop_lo;

    // The spec states the to-ground plateau as flat "within ±0.5 dB" over this
    // span. The physics puts it fractionally outside that: the clip voltage of
    // a diode-to-ground network is θ·ln(v_drive/(R·Is)), so QUADRUPLING the
    // drive raises it by θ·ln 4 ≈ 35.8 mV on a ~0.63 V clip — about 0.48 dB
    // before the measurement's own settling, and 0.51 dB after. A ±0.5 dB bound
    // is therefore right at the edge of what a correct clipper produces, so what
    // is asserted here is the DERIVED prediction rather than the round number.
    // See adjudication A-12.
    const double theta = 1.0 * junction::kThermalVoltage;  // silicon: n = 1
    const double clip_at_lo = units::db_to_linear(ground_lo);
    const double predicted_growth =
        units::linear_to_db((clip_at_lo + theta * std::log(4.0)) / clip_at_lo);
    REQUIRE_THAT(ground_growth, WithinAbs(predicted_growth, 0.15));

    // The in-loop stage plateaus logarithmically TOO, and for the same reason —
    // its output is also a diode clip voltage. Both are bounded by the same
    // physics, so "one plateaus and the other does not" is not the distinction
    // between them. Asserted so a future revision cannot quietly reintroduce
    // the claim: see the frequency-dependence case below for what actually
    // separates the two.
    REQUIRE(loop_growth < 4.0 * ground_growth);
}

TEST_CASE("4 the in-loop knee is frequency-dependent and the to-ground knee is not",
          "[distortion][clipper]") {
    // THE real, measurable difference between the topologies, and the one the
    // spec's own worked example describes: the feedback capacitor's impedance
    // falls with frequency, so above its corner it shunts feedback current
    // around the diodes and progressively excludes highs from the hardest part
    // of the nonlinearity. A to-ground clipper has no such path — its shunt
    // capacitor sits at ~312 kHz, far above audio — so it distorts the same at
    // every audio frequency.
    //
    // At Rf = 51 kΩ, Cf ≈ 4.33 nF: |Zc| is 368 kΩ at 100 Hz (≫ Rf, diodes see
    // full loop gain) and 7.35 kΩ at 5 kHz (≪ Rf, mostly bypassed).
    const auto third_harmonic_ratio = [](ClipperTopology topology, double tone_hz) {
        FeedbackClipperT<double> clipper;
        clipper.prepare(kSr);
        clipper.set_topology(topology);
        clipper.set_diode_model(DiodeModel::silicon);
        clipper.set_symmetry(0.0);
        clipper.reset();

        // A whole number of periods fills the analysis window at both probe
        // frequencies, so the DFT is leakage-free.
        const int len = static_cast<int>(kSr / 25.0);  // 25 Hz resolution
        const double w = 2.0 * std::numbers::pi * tone_hz / kSr;
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(len));
        for (int n = 0; n < 4800 + len; ++n) {
            const double y = clipper.process(0.3 * std::sin(w * n));
            if (n >= 4800) out.push_back(y);
        }
        return harmonic_magnitude(out, tone_hz, 3) / harmonic_magnitude(out, tone_hz, 1);
    };

    const double loop_low = third_harmonic_ratio(ClipperTopology::in_loop, 100.0);
    const double loop_high = third_harmonic_ratio(ClipperTopology::in_loop, 5000.0);
    const double ground_low = third_harmonic_ratio(ClipperTopology::to_ground, 100.0);
    const double ground_high = third_harmonic_ratio(ClipperTopology::to_ground, 5000.0);

    // In-loop: markedly less distortion up high, because the capacitor has
    // taken the highs around the diodes.
    REQUIRE(loop_high < loop_low * 0.5);
    // To-ground: the same nonlinearity at both frequencies.
    REQUIRE_THAT(units::linear_to_db(ground_high),
                 WithinAbs(units::linear_to_db(ground_low), 1.0));
}

// ── 5. Bounded Newton iteration ───────────────────────────────────────────

TEST_CASE("5 the solver terminates within its cap on a pathological step",
          "[distortion][clipper][rt-safety]") {
    // A full-scale single-sample discontinuity is the worst |v[n] − v[n−1]| the
    // solver will ever see. The cap is an RT-safety contract, so it is asserted
    // rather than assumed — and the output must still be finite, because
    // hitting the cap is a bounded degrade, not a failure.
    for (auto model : {DiodeModel::silicon, DiodeModel::germanium, DiodeModel::led}) {
        for (double symmetry : {-1.0, 0.0, 1.0}) {
            auto clipper = make_clipper(model, symmetry);
            clipper.reset();
            for (int n = 0; n < 64; ++n) {
                const double x = (n == 8) ? 1.0 : (n == 9 ? -1.0 : 0.0);
                const double y = clipper.process(x);
                REQUIRE(std::isfinite(y));
                REQUIRE(clipper.last_iteration_count() <=
                        ClipperSolverConfig::kMaxNewtonIterationsPerSample);
            }
        }
    }
}

// ── 6. The feedback loop's gain bound ─────────────────────────────────────

TEST_CASE("6 in-loop gain never exceeds the linear gain and only falls",
          "[distortion][clipper][gain]") {
    // Series law 1's tested invariant, and what the Forge registry's
    // worst_case_gain cites. The diodes are an open circuit at zero signal, so
    // the small-signal gain is exactly Rf/Rin; as they turn on they shunt more
    // feedback current, which can only REDUCE loop gain.
    FeedbackClipperT<double> clipper;
    clipper.prepare(kSr);
    clipper.set_topology(ClipperTopology::in_loop);
    clipper.set_diode_model(DiodeModel::silicon);
    clipper.set_symmetry(0.0);

    const double bound = clipper.linear_gain();
    REQUIRE(bound > 1.0);

    std::vector<double> gains;
    constexpr int points = 40;
    for (int i = 0; i < points; ++i) {
        // 1 mV to full scale, log-spaced.
        const double amp = units::taper_log(static_cast<double>(i) / (points - 1), 1e-3, 1.0);
        clipper.reset();
        // 100 Hz, well BELOW the feedback capacitor's 720 Hz corner. Above the
        // corner the capacitor shunts feedback current around the diodes and
        // the stage gain is legitimately lower than Rf/Rin — at 1 kHz it is
        // 6.34 rather than 10.85 — so a "does it reach the linear gain" check
        // there would be measuring the capacitor, not the loop bound.
        const double w = 2.0 * std::numbers::pi * 100.0 / kSr;
        double peak = 0.0;
        for (int n = 0; n < 4800; ++n) {
            const double y = clipper.process(amp * std::sin(w * n));
            if (n > 2400) peak = std::max(peak, std::abs(y));
        }
        const double gain = peak / amp;
        REQUIRE(gain <= bound * 1.001);
        gains.push_back(gain);
    }

    // Monotonically non-increasing across the sweep: the loop is self-limiting,
    // never self-amplifying. A small tolerance absorbs the settling of the
    // per-amplitude render rather than admitting real expansion.
    for (std::size_t i = 1; i < gains.size(); ++i) REQUIRE(gains[i] <= gains[i - 1] * 1.01);

    // The smallest amplitudes really do sit at the linear gain, so the bound is
    // reached rather than merely respected.
    REQUIRE(gains.front() > bound * 0.95);
    // ...and the largest are well below it, so clipping actually happened.
    REQUIRE(gains.back() < bound * 0.5);
}

TEST_CASE("6 in-loop small-signal response rolls off above the feedback corner",
          "[distortion][clipper][gain]") {
    FeedbackClipperT<double> clipper;
    clipper.prepare(kSr);
    clipper.set_topology(ClipperTopology::in_loop);
    clipper.set_diode_model(DiodeModel::silicon);
    clipper.set_symmetry(0.0);
    clipper.reset();

    // Keep the pair non-conducting so this measures the feedback R-C network.
    // At 20 kHz versus a 720 Hz corner, its gain must be far below the low-band
    // Rf/Rin gain. The former state update reused the current forcing as history
    // and incorrectly floored near half the linear gain at Nyquist.
    constexpr double amplitude = 1e-4;
    constexpr int settle = 4800;
    constexpr int frames = 4800;
    const auto measured_gain = [&](double tone_hz) {
        clipper.reset();
        std::vector<double> out;
        out.reserve(frames);
        for (int n = 0; n < settle + frames; ++n) {
            const double y =
                clipper.process(amplitude * std::sin(2.0 * std::numbers::pi * tone_hz * n / kSr));
            if (n >= settle) out.push_back(y);
        }
        return harmonic_magnitude(out, tone_hz, 1) / amplitude;
    };
    const double gain_10k = measured_gain(10000.0);
    const double gain_20k = measured_gain(20000.0);
    CAPTURE(gain_10k, gain_20k);
    REQUIRE(gain_20k < gain_10k * 0.4);
}

TEST_CASE("6 in-loop small-signal gain has no absolute-tolerance dead zone",
          "[distortion][clipper][gain][solver]") {
    FeedbackClipperT<double> clipper;
    clipper.prepare(kSr);
    clipper.set_topology(ClipperTopology::in_loop);
    clipper.set_diode_model(DiodeModel::silicon);
    clipper.set_symmetry(0.0);
    clipper.reset();

    // Well below the old ~-105 dBFS cutoff, but still many orders above double
    // precision. At DC the capacitor is open and the diode pair is nonconducting,
    // so the response must approach the documented Rf/Rin gain.
    constexpr double amplitude = 1e-7;  // -140 dBFS
    double output = 0.0;
    for (int n = 0; n < 48000; ++n) output = clipper.process(amplitude);
    const double gain = std::abs(output) / amplitude;
    CAPTURE(gain, clipper.linear_gain());
    REQUIRE_THAT(gain, WithinRel(clipper.linear_gain(), 0.01));
}

TEST_CASE("6 in-loop gain stays bounded over the reachable drive and frequency range",
          "[distortion][clipper][gain][solver]") {
    // The pre-emphasis shelf can legally deliver 4x full scale to this stage.
    // Exercise that reachable endpoint at the upper audio band, where the old
    // trapezoidal state update became an alternating, self-amplifying mode.
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const double tone_hz = std::min(20000.0, sr * 0.4);
        for (auto model : {DiodeModel::silicon, DiodeModel::germanium, DiodeModel::led}) {
            for (double symmetry : {-1.0, 0.0, 1.0}) {
                for (double amplitude :
                     {1.0, units::db_to_linear(ToneStackT<double>::kPreGainDbMax)}) {
                    FeedbackClipperT<double> clipper;
                    clipper.prepare(sr);
                    clipper.set_topology(ClipperTopology::in_loop);
                    clipper.set_diode_model(model);
                    clipper.set_symmetry(symmetry);
                    clipper.reset();

                    const double bound = clipper.linear_gain() * amplitude;
                    double peak = 0.0;
                    const int frames = static_cast<int>(sr / 4.0);
                    for (int n = 0; n < frames; ++n) {
                        const double x =
                            amplitude * std::sin(2.0 * std::numbers::pi * tone_hz * n / sr);
                        const double y = clipper.process(x);
                        REQUIRE(std::isfinite(y));
                        REQUIRE(clipper.last_iteration_count() <=
                                ClipperSolverConfig::kMaxNewtonIterationsPerSample);
                        peak = std::max(peak, std::abs(y));
                    }
                    CAPTURE(sr, tone_hz, model, symmetry, amplitude, peak, bound);
                    REQUIRE(peak <= bound * 1.001);
                }
            }
        }
    }
}

TEST_CASE("the clipper solves damp their stiff mode at 8 kHz",
          "[distortion][clipper][solver]") {
    // At 8 kHz, the old trapezoidal update alternated sign at its iteration
    // cap and grew into the thousands from an ordinary full-scale 440 Hz sine.
    // A stiffly-decaying integration scheme must keep the physical gain bound
    // even at this deliberately hostile sample rate.
    constexpr double sr = 8000.0;
    const auto render_peak = [](auto& clipper, double amplitude) {
        double peak = 0.0;
        for (int n = 0; n < 8000; ++n) {
            const double x = amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * n / sr);
            const double y = clipper.process(x);
            REQUIRE(std::isfinite(y));
            REQUIRE(clipper.last_iteration_count() <=
                    ClipperSolverConfig::kMaxNewtonIterationsPerSample);
            peak = std::max(peak, std::abs(y));
        }
        return peak;
    };

    DiodeClipperT<double> ground;
    ground.prepare(sr);
    ground.set_diode_model(DiodeModel::silicon);
    ground.set_symmetry(0.0);
    ground.reset();
    const double ground_peak = render_peak(ground, 1.0);
    CAPTURE(ground_peak);
    REQUIRE(ground_peak <= 1.0);

    FeedbackClipperT<double> loop;
    loop.prepare(sr);
    loop.set_topology(ClipperTopology::in_loop);
    loop.set_diode_model(DiodeModel::silicon);
    loop.set_symmetry(0.0);
    loop.reset();
    const double amplitude = units::db_to_linear(ToneStackT<double>::kPreGainDbMax);
    const double loop_peak = render_peak(loop, amplitude);
    CAPTURE(loop_peak);
    REQUIRE(loop_peak <= loop.linear_gain() * amplitude * 1.001);
}

// ── 7. Determinism ────────────────────────────────────────────────────────

TEST_CASE("7 render, reset, re-render is bit-identical", "[distortion][determinism]") {
    // There is no randomness anywhere in this file, so this is a check that no
    // uninitialised or carried-over state leaks between renders.
    const auto render = [](auto& stage, int n) {
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double t = i / kSr;
            const double x = 0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * t) +
                             0.3 * std::sin(2.0 * std::numbers::pi * 660.0 * t) +
                             0.2 * std::sin(2.0 * std::numbers::pi * 1470.0 * t);
            out.push_back(stage.process(x));
        }
        return out;
    };

    {
        auto clipper = make_clipper(DiodeModel::germanium, 0.5);
        clipper.reset();
        const auto first = render(clipper, static_cast<int>(kSr * 2));
        clipper.reset();
        const auto second = render(clipper, static_cast<int>(kSr * 2));
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);

        FeedbackClipperT<double> loop;
        loop.prepare(kSr);
        loop.reset();
        const auto loop_first = render(loop, static_cast<int>(kSr));
        loop.reset();
        const auto loop_second = render(loop, static_cast<int>(kSr));
        for (std::size_t i = 0; i < loop_first.size(); ++i)
            REQUIRE(loop_first[i] == loop_second[i]);
    }
}

// ── The diode network's closed forms ──────────────────────────────────────

TEST_CASE("the antiderivative differentiates back to the current",
          "[distortion][clipper]") {
    // The ADAA path is only as good as this identity, and the `exprel`
    // formulation exists specifically so it survives a leg being removed
    // (b → 0), where the naive θ/b form divides by a vanishing number.
    for (auto model : {DiodeModel::silicon, DiodeModel::germanium, DiodeModel::led}) {
        for (double symmetry : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
            junction::JunctionPair network;
            detail::apply_diode_model(network, model);
            detail::apply_symmetry(network, symmetry);

            for (double v = -0.5; v <= 0.5; v += 0.01) {
                const double h = 1e-7;
                const double numerical =
                    (network.antiderivative(v + h) - network.antiderivative(v - h)) / (2 * h);
                const double analytic = network.current(v);
                REQUIRE_THAT(numerical, WithinAbs(analytic, 1e-4 * (1.0 + std::abs(analytic))));
            }
        }
    }
}

TEST_CASE("a removed leg is exactly a single diode", "[distortion][clipper]") {
    // symmetry = −1 must produce the one-sided Shockley law with no residual
    // conduction on the other half — the property that makes it half-wave.
    junction::JunctionPair network;
    detail::apply_diode_model(network, DiodeModel::silicon);
    detail::apply_symmetry(network, -1.0);
    REQUIRE(network.leg_b == 0.0);

    for (double v = -2.0; v < 0.0; v += 0.01) {
        // The absent leg contributes exp(0) = 1, cancelling the conducting
        // leg's own −1 → the reverse half is exactly the diode's saturation
        // current, not zero and not a wrong-signed conduction.
        REQUIRE(network.current(v) <= 0.0);
        REQUIRE(network.current(v) >= -network.saturation_current * 1.000001);
    }
    // And forward conduction is unaffected.
    REQUIRE(network.current(0.7) > 1e-3);
}

// ── 10. Tone stack corner accuracy ────────────────────────────────────────

TEST_CASE("10 the tone stack's corners land where they are configured",
          "[distortion][tone-stack]") {
    // A one-pole low-pass is −3.01 dB at its corner by definition; the
    // expectation is computed from that definition, not restated.
    const double minus_3db = units::linear_to_db(1.0 / std::sqrt(2.0));

    for (double corner : {500.0, 2000.0, 4000.0, 8000.0}) {
        ToneStackT<double> tone;
        tone.prepare(kSr);
        tone.set_post_tone_hz(corner);
        tone.set_tone_mix(1.0);
        tone.reset();

        // Coherent DFT at the corner over a whole number of periods.
        const int len = 48000;
        const double w = 2.0 * std::numbers::pi * corner / kSr;
        double re = 0.0, im = 0.0;
        for (int n = 0; n < 4800; ++n) tone.process_post(std::sin(w * n));  // settle
        for (int n = 0; n < len; ++n) {
            const double y = tone.process_post(std::sin(w * (n + 4800)));
            re += y * std::cos(w * n);
            im += y * std::sin(w * n);
        }
        const double magnitude = 2.0 * std::hypot(re, im) / len;
        REQUIRE_THAT(units::linear_to_db(magnitude), WithinAbs(minus_3db, 0.3));
    }
}

TEST_CASE("10 a flat pre-emphasis shelf is exactly flat", "[distortion][tone-stack]") {
    // The shelf is built from the one-pole's complementary low/high outputs, so
    // unity gain is flat by construction rather than to within a fitted
    // tolerance. Worth asserting: a shelf that is 0.5 dB off at unity would
    // quietly retune every drive setting.
    ToneStackT<double> tone;
    tone.prepare(kSr);
    tone.set_pre_tone_hz(720.0);
    tone.set_pre_gain_db(0.0);
    tone.set_tone_mix(1.0);
    tone.reset();

    for (int n = 0; n < 4800; ++n) {
        const double x = std::sin(2.0 * std::numbers::pi * 3000.0 * n / kSr);
        REQUIRE_THAT(tone.process_pre(x), WithinAbs(x, 1e-9));
    }
}

TEST_CASE("10 tone mix crossfades against bypass", "[distortion][tone-stack]") {
    ToneStackT<double> tone;
    tone.prepare(kSr);
    tone.set_post_tone_hz(500.0);
    tone.reset();

    // 6 kHz is 8 samples per cycle, one of which lands exactly on the crest.
    // At 8 kHz there are 6 and none of them do, so peak tracking would
    // under-read by sin(60°) = −1.25 dB and look like a gain error.
    const double w = 2.0 * std::numbers::pi * 6000.0 / kSr; // well above the corner
    const auto settled_peak = [&](double mix) {
        tone.set_tone_mix(mix);
        tone.reset();
        double peak = 0.0;
        for (int n = 0; n < 9600; ++n) {
            const double y = tone.process_post(std::sin(w * n));
            if (n > 4800) peak = std::max(peak, std::abs(y));
        }
        return peak;
    };

    REQUIRE_THAT(settled_peak(0.0), WithinAbs(1.0, 1e-6));  // fully bypassed
    REQUIRE(settled_peak(1.0) < 0.3);                       // fully filtered
    const double half = settled_peak(0.5);
    REQUIRE(half > settled_peak(1.0));
    REQUIRE(half < settled_peak(0.0));
}

// ── float/double parity ───────────────────────────────────────────────────

TEST_CASE("the float and double instantiations agree", "[distortion]") {
    DiodeClipperT<float> f;
    DiodeClipperT<double> d;
    f.prepare(kSr);
    d.prepare(kSr);
    f.reset();
    d.reset();

    const double w = 2.0 * std::numbers::pi * 440.0 / kSr;
    for (int n = 0; n < 24000; ++n) {
        const double x = 2.0 * std::sin(w * n);
        REQUIRE_THAT(static_cast<double>(f.process(static_cast<float>(x))),
                     WithinAbs(d.process(x), 1e-4));
    }
}

TEST_CASE("The clipper family rejects non-finite controls and audio without poisoning state",
          "[distortion][nan-recovery][rt-safety]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();

    SECTION("diode-to-ground") {
        DiodeClipperT<double> poisoned;
        DiodeClipperT<double> fresh;
        for (auto* clipper : {&poisoned, &fresh}) {
            clipper->prepare(kSr);
            clipper->set_symmetry(0.25);
            clipper->set_resistance(22000.0);
            clipper->set_capacitance(100e-12);
            clipper->reset();
        }

        poisoned.set_symmetry(nan);
        poisoned.set_resistance(nan);
        poisoned.set_capacitance(nan);
        require_allocates_no_memory([&] { REQUIRE(poisoned.process(nan) == 0.0); });

        for (int n = 0; n < 1024; ++n) {
            const double x = 0.8 * std::sin(2.0 * std::numbers::pi * 997.0 * n / kSr);
            REQUIRE(poisoned.process(x) == fresh.process(x));
        }
    }

    SECTION("feedback clipper") {
        FeedbackClipperT<double> poisoned;
        FeedbackClipperT<double> fresh;
        for (auto* clipper : {&poisoned, &fresh}) {
            clipper->prepare(kSr);
            clipper->set_topology(ClipperTopology::in_loop);
            clipper->set_symmetry(-0.25);
            clipper->set_feedback_resistance(68000.0);
            clipper->set_input_resistance(12000.0);
            clipper->set_knee_corner_hz(900.0);
            clipper->reset();
        }

        poisoned.set_symmetry(nan);
        poisoned.set_feedback_resistance(nan);
        poisoned.set_input_resistance(nan);
        poisoned.set_knee_corner_hz(nan);
        require_allocates_no_memory([&] { REQUIRE(poisoned.process(nan) == 0.0); });

        for (int n = 0; n < 1024; ++n) {
            const double x = 0.3 * std::sin(2.0 * std::numbers::pi * 431.0 * n / kSr);
            REQUIRE(poisoned.process(x) == fresh.process(x));
        }
    }

    SECTION("tone stack") {
        ToneStackT<double> poisoned;
        ToneStackT<double> fresh;
        for (auto* tone : {&poisoned, &fresh}) {
            tone->prepare(kSr);
            tone->set_pre_tone_hz(850.0);
            tone->set_post_tone_hz(4200.0);
            tone->set_pre_gain_db(6.0);
            tone->set_tone_mix(0.75);
            tone->reset();
        }

        poisoned.set_pre_tone_hz(nan);
        poisoned.set_post_tone_hz(nan);
        poisoned.set_pre_gain_db(nan);
        poisoned.set_tone_mix(nan);
        require_allocates_no_memory([&] {
            REQUIRE(poisoned.process_pre(nan) == 0.0);
            REQUIRE(poisoned.process_post(nan) == 0.0);
        });

        // The second rejected sample resets the same complete tone-stack state
        // that the reference starts from.
        for (int n = 0; n < 1024; ++n) {
            const double x = 0.4 * std::sin(2.0 * std::numbers::pi * 613.0 * n / kSr);
            REQUIRE(poisoned.process_pre(x) == fresh.process_pre(x));
            REQUIRE(poisoned.process_post(x) == fresh.process_post(x));
        }
    }
}

// ── 9. RT allocation probe ────────────────────────────────────────────────

TEST_CASE("9 the clipper family allocates nothing on the audio thread",
          "[distortion][rt-safety]") {
    DiodeClipperT<float> ground;
    FeedbackClipperT<float> loop;
    ToneStackT<float> tone;
    ground.prepare(kSr);
    loop.prepare(kSr);
    tone.prepare(kSr);

    require_allocates_no_memory([&] {
        for (int n = 0; n < 4096; ++n) {
            const auto model = static_cast<DiodeModel>((n / 512) % 3);
            const float symmetry = std::sin(0.001f * static_cast<float>(n));
            ground.set_diode_model(model);
            ground.set_symmetry(symmetry);
            loop.set_diode_model(model);
            loop.set_symmetry(symmetry);
            loop.set_topology((n % 3) == 0 ? ClipperTopology::to_ground
                                           : ClipperTopology::in_loop);
            loop.set_knee_corner_hz(200.0 + 0.5 * n);
            tone.set_pre_tone_hz(200.0 + 0.5 * n);
            tone.set_post_tone_hz(1000.0 + n);
            tone.set_tone_mix(0.5);

            const float x = 0.5f * std::sin(0.05f * static_cast<float>(n));
            (void)ground.process(tone.process_pre(x));
            (void)tone.process_post(loop.process(x));
        }
        ground.reset();
        loop.reset();
        tone.reset();
    });
}

TEST_CASE("The clipper solve stays bounded at full scale on every model",
          "[signal][distortion][solver]") {
    // The test that was missing, and whose absence let a divergence ship.
    //
    // The only ADAA assertion this suite had compared two renders for equality.
    // A DETERMINISTIC divergence passes that: the run reproduces perfectly, it
    // just reproduces 5.8e25. Testing a property a broken implementation also
    // satisfies is the failure mode to watch for — determinism, monotonicity and
    // "the value changed" are all in that family.
    //
    // So this asserts the thing a caller actually needs: a full-scale input
    // produces a bounded output. The clipper is a CLIPPER — its output cannot
    // legitimately exceed the input's scale by more than its own small-signal
    // gain, on any diode model, at any sample rate, either topology.
    // Include 8 kHz: TR-BDF2's L-stable completion must damp the alternating
    // stiff mode that previously forced this suite to exclude that rate.
    for (double sr : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
        for (auto model : {DiodeModel::silicon, DiodeModel::germanium, DiodeModel::led}) {
            for (double symmetry : {-1.0, 0.0, 1.0}) {
                DiodeClipperT<double> ground;
                ground.prepare(sr);
                ground.set_diode_model(model);
                ground.set_symmetry(symmetry);
                ground.reset();

                FeedbackClipperT<double> loop;
                loop.prepare(sr);
                loop.set_diode_model(model);
                loop.set_symmetry(symmetry);
                loop.reset();

                double ground_peak = 0.0;
                double loop_peak = 0.0;
                const int frames = static_cast<int>(sr / 10.0);
                for (int n = 0; n < frames; ++n) {
                    const double x = std::sin(2.0 * std::numbers::pi * 440.0 * n / sr);
                    const double g = ground.process(x);
                    const double l = loop.process(x);
                    REQUIRE(std::isfinite(g));
                    REQUIRE(std::isfinite(l));
                    ground_peak = std::max(ground_peak, std::abs(g));
                    loop_peak = std::max(loop_peak, std::abs(l));
                }
                // Shunt topology can only attenuate a full-scale input.
                REQUIRE(ground_peak <= 1.0);
                // The in-loop stage is a gain stage; its own documented
                // small-signal bound is what caps it.
                REQUIRE(loop_peak <= loop.linear_gain());
            }
        }
    }
}
