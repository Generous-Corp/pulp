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

TEST_CASE("A1 the divider realises its requested gain reduction exactly",
          "[diode-bridge][gain-law]") {
    // At −40 dBFS the shaper's curvature is utterly negligible — with the
    // default character the internal amplitude is ~1.1e-3, so the fundamental's
    // compression factor `1 − β·s²/4` differs from 1 by 1.5e-7 — which is what
    // makes this a measurement of the DIVIDER rather than of the colour.
    //
    // ADAA is off here for the same reason it is on everywhere else: the
    // first-order scheme is exactly a two-tap average, whose magnitude at the
    // probe frequency is `cos(ω/2)` and whose half-sample delay moves the peak
    // off the sampling grid. Together those cost 0.037 dB at 1 kHz — 75 % of
    // the ±0.05 dB budget, for a reason that has nothing to do with the gain
    // law. The next case asserts that offset instead of hiding it.
    for (double x : {0.413, 1.0, 2.981, 9.0}) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(0.35);
        bridge.set_adaa(false);
        bridge.reset();

        const double amplitude = units::db_to_linear(-40.0);
        double peak = 0.0;
        for (int n = 0; n < 9600; ++n) {
            const double y = bridge.process(amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr), x);
            if (n > 4800) peak = std::max(peak, std::abs(y));
        }
        const double expected_db = units::linear_to_db(Bridge::gain_for_control_drive(x));
        REQUIRE_THAT(units::linear_to_db(peak / amplitude), WithinAbs(expected_db, 0.05));
    }

    // The worked table: x = 0.413 / 0.995 / 2.981 / 9.0 are the drives for
    // −3 / −6 / −12 / −20 dB, and the conversion is its own exact inverse.
    for (double gr_db : {-3.0, -6.0, -12.0, -20.0, -30.0}) {
        const double x = Bridge::control_drive_for_gain_db(gr_db);
        REQUIRE_THAT(units::linear_to_db(Bridge::gain_for_control_drive(x)), WithinAbs(gr_db, 1e-12));
    }
}

TEST_CASE("A1 the ADAA offset is the two-tap average, not a gain error",
          "[diode-bridge][gain-law][adaa]") {
    // First-order ADAA of a LINEAR function is `(s[n] + s[n−1])/2` identically,
    // so its effect on a sine is fully determined: magnitude `cos(ω/2)`, delay
    // half a sample. Sampling the peak of a half-sample-delayed sine costs a
    // second factor of `cos(ω/2)` at this probe frequency, where a sample lands
    // exactly on the crest. The measured deviation is therefore `cos²(ω/2)`
    // — asserted, so that a future change to the ADAA path cannot pass this
    // suite by quietly turning into a gain stage.
    const double w = 2.0 * M_PI * kToneHz / kSr;
    const double predicted_db = units::linear_to_db(std::cos(0.5 * w) * std::cos(0.5 * w));

    Bridge bridge;
    bridge.prepare(kSr);
    bridge.set_character(0.35);
    bridge.set_adaa(true);
    bridge.reset();

    const double amplitude = units::db_to_linear(-40.0);
    double peak = 0.0;
    for (int n = 0; n < 9600; ++n) {
        const double y = bridge.process(amplitude * std::sin(w * n), 0.0);
        if (n > 4800) peak = std::max(peak, std::abs(y));
    }
    REQUIRE_THAT(units::linear_to_db(peak / amplitude), WithinAbs(predicted_db, 0.002));
    // ...and it is a LOSS, so it cannot mask a gain the bound would have to
    // account for.
    REQUIRE(predicted_db < 0.0);
}

TEST_CASE("A1 the gain element can never boost", "[diode-bridge][gain-law]") {
    // The structural fact the whole worst-case bound rests on. Asserted over
    // the declared drive range and past it.
    for (double x = 0.0; x <= 2.0 * Bridge::kMaxControlDrive; x += 0.01)
        REQUIRE(Bridge::gain_for_control_drive(x) <= 1.0);
    REQUIRE(Bridge::gain_for_control_drive(0.0) == 1.0);
    // A negative drive is a caller error, not a boost request.
    REQUIRE(Bridge::gain_for_control_drive(-5.0) == 1.0);
}

// ── The junction composition ──────────────────────────────────────────────

TEST_CASE("the control law comes from the shared junction, not a second exponential",
          "[diode-bridge][junction]") {
    // `junction.hpp` owns the thermal voltage and the exponential. This asserts
    // that routing the drive law through `conductance(knee_voltage(I))`
    // reproduces the two textbook closed forms EXACTLY — `r_d = n·V_T/(I + Is)`
    // and `x = Rs·I/(n·V_T)` — which is the evidence that the composition is
    // load-bearing rather than decorative. If someone later inlines an `exp`
    // here, `theta` below stops matching and this fails.
    Bridge bridge;
    const double theta = Bridge::kIdeality * junction::kThermalVoltage;

    for (double amperes : {1e-6, 1e-5, 1e-4, 1e-3, 1e-2}) {
        REQUIRE_THAT(bridge.dynamic_resistance(amperes),
                     WithinRel(theta / (amperes + Bridge::kSaturationCurrent), 1e-9));
        REQUIRE_THAT(bridge.control_drive_for_current(amperes),
                     WithinRel(Bridge::kSeriesResistance * amperes / theta, 1e-6));
    }

    // No current means no reduction, as an identity rather than an
    // approximation: the zero-bias conductance is subtracted out.
    REQUIRE_THAT(bridge.control_drive_for_current(0.0), WithinAbs(0.0, 1e-15));
    REQUIRE(Bridge::gain_for_control_drive(bridge.control_drive_for_current(0.0)) == 1.0);

    // Resistance really is inversely proportional to bias current — the claim
    // the entire topology rests on. A decade of current is a decade of
    // resistance.
    REQUIRE_THAT(bridge.dynamic_resistance(1e-5) / bridge.dynamic_resistance(1e-4),
                 WithinRel(10.0, 1e-6));
}

TEST_CASE("the shaper's antiderivative differentiates back to the shaper",
          "[diode-bridge][adaa]") {
    // The ADAA path is only as good as this identity, in both blocks. A sign
    // error here does not crash — it produces a waveform that is wrong on half
    // of each cycle and still reads as the colour working.
    for (double x : {0.0, 1.0, 9.0, Bridge::kMaxControlDrive}) {
        const double beta = Bridge::curvature(x);
        for (double s = -0.3; s <= 0.3; s += 0.005) {
            const double h = 1e-6;
            const double numerical = (Bridge::shape_antiderivative(s + h, beta) -
                                      Bridge::shape_antiderivative(s - h, beta)) /
                                     (2.0 * h);
            REQUIRE_THAT(numerical, WithinAbs(Bridge::shape(s, beta), 1e-8));
        }
    }

    for (double character : {0.0, 0.35, 1.0}) {
        Bracket bracket;
        bracket.prepare(kSr);
        bracket.set_character(character);
        // Straddles zero, where the piecewise split lives: the two branches
        // must agree in value AND slope or the quotient is wrong every time a
        // waveform crosses the axis.
        for (double u = -1.5; u <= 1.5; u += 0.01) {
            const double h = 1e-6;
            const double numerical = (bracket.saturate_antiderivative(u + h) -
                                      bracket.saturate_antiderivative(u - h)) /
                                     (2.0 * h);
            REQUIRE_THAT(numerical, WithinAbs(bracket.saturate(u), 1e-8));
        }
    }
}

TEST_CASE("the shaper stays monotonic across its whole operating range",
          "[diode-bridge][gain-law]") {
    // The cubic folds back beyond `1/√β`; the clamp is what makes that
    // unreachable. Asserted at the deepest curvature the drive range admits.
    const double beta = Bridge::curvature(Bridge::kMaxControlDrive);
    const double limit = Bridge::max_operating_amplitude(beta);
    REQUIRE(limit < 1.0 / std::sqrt(beta));

    double previous = Bridge::shape(-limit, beta);
    for (double s = -limit + 1e-4; s <= limit; s += 1e-4) {
        const double value = Bridge::shape(s, beta);
        REQUIRE(value > previous);
        previous = value;
    }

    // Full-scale audio at maximum character lands inside that clamp, so the
    // guard is a guard rather than a shaping stage.
    Bridge bridge;
    bridge.set_character(1.0);
    REQUIRE(bridge.drive() <= Bridge::max_operating_amplitude(beta));
    REQUIRE(bridge.drive() == Bridge::kDriveBridgeMax);
}

// ── A2. Static compression curve ──────────────────────────────────────────

TEST_CASE("A2 the realised curve follows the gain computer", "[diode-bridge][static-curve]") {
    // Feed-forward isolates the computer from the loop softening A8 covers.
    //
    // TWO DEVIATIONS FROM THE SPEC'S RECIPE, both stated rather than absorbed:
    //
    // (1) Gain reduction is measured RELATIVE to the small-signal gain rather
    //     than absolutely. The colour stages have a fixed −0.063 dB insertion
    //     loss at 1 kHz even at `character = 0` — 0.054 dB of ADAA two-tap
    //     averaging plus 0.009 dB of bracket filter roll-off — which is a
    //     level-independent constant, not a gain-computer error. Charging it to
    //     the computer would spend 63 % of a ±0.1 dB budget before the first
    //     comparison.
    //
    // (2) The release is set to the top of its range. A one-pole PEAK follower
    //     decays between the peaks of a tone, so it reads a sine slightly under
    //     its true peak, and the resulting error is entirely a function of how
    //     much it decays per cycle: at 400 / 1000 / 2000 ms the worst
    //     hard-region error is 0.193 / 0.113 / 0.085 dB. A static-curve
    //     measurement wants peak-hold, which the spec's recipe does not pin.
    Comp c = make_compressor();
    c.set_release_ms(Comp::kReleaseMsMax);

    const double reference_db =
        units::linear_to_db(settled_peak(c, units::db_to_linear(-40.0), 5.0)) + 40.0;

    for (int db = -40; db <= 6; ++db) {
        c.reset();
        const double amplitude = units::db_to_linear(static_cast<double>(db));
        const double measured =
            units::linear_to_db(settled_peak(c, amplitude, 5.0)) - db - reference_db;
        const double expected = c.static_curve_db(static_cast<double>(db));
        const bool in_knee = std::abs(static_cast<double>(db) - (-12.0)) <= 0.5 * 6.0;
        REQUIRE_THAT(measured, WithinAbs(expected, in_knee ? 0.3 : 0.1));
    }
}

TEST_CASE("A2 the limit region engages at the top of the ratio control",
          "[diode-bridge][static-curve]") {
    // `kLimitRatio` must be reachable from the parameter table, or the
    // brickwall position is unreachable — the specific failure the spec's
    // "must be ≥ the ratio parameter's max" note guards against.
    REQUIRE(Comp::kLimitRatio <= Comp::kRatioMax);

    Comp c = make_compressor();
    c.set_knee_db(0.0);
    c.set_ratio(Comp::kRatioMax);
    // Above the threshold the curve pins the output AT the threshold: every dB
    // of input past it becomes a dB of reduction.
    for (double level : {-6.0, 0.0, 6.0})
        REQUIRE_THAT(c.static_curve_db(level), WithinAbs(-(level + 12.0), 1e-12));
}

// ── A3. Third-harmonic colour ─────────────────────────────────────────────

TEST_CASE("A3 the bridge's third harmonic matches the closed form",
          "[diode-bridge][colour]") {
    // SPEC DEFECT. §3.4's worked example computes the third-harmonic ratio as
    // `β·s²/3`, taking the cubic term's amplitude `β·s³/3` over the fundamental
    // `s`. That drops the `sin³θ = (3·sinθ − sin3θ)/4` expansion: only a
    // QUARTER of the cubic term lands on the third harmonic, and the other
    // three quarters subtract from the fundamental. The correct ratio is
    //
    //     THD3 = (β·s³/12) / (s·(1 − β·s²/4)) = β·s²/12 / (1 − β·s²/4)
    //
    // — four times smaller. At the spec's own worked point (−6 dBFS,
    // character 0.35, at rest) that is 0.0124 %, not 0.0497 %, and A3's
    // "≈0.05 %, tol ±0.02 %-absolute" band of [0.03 %, 0.07 %] excludes the
    // right answer by a factor of 2.4. Asserted against the shipped closed form
    // `Bridge::third_harmonic_ratio()`, which is derived above rather than
    // restated here.
    const auto measure = [](double control_drive) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(0.35);
        bridge.reset();
        const double amplitude = units::db_to_linear(-6.0);
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
            const double y =
                bridge.process(amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr), control_drive);
            if (n >= 4800) out.push_back(y);
        }
        return harmonic_magnitude(out, kToneHz, 3) / harmonic_magnitude(out, kToneHz, 1);
    };

    Bridge reference;
    reference.set_character(0.35);
    const double s = units::db_to_linear(-6.0) * reference.drive();

    const double at_rest = measure(0.0);
    const double predicted_rest = Bridge::third_harmonic_ratio(s, Bridge::curvature(0.0));
    REQUIRE_THAT(at_rest, WithinRel(predicted_rest, 0.05));
    // Small enough to be the documented "low distortion by design" regime, and
    // large enough to be a colour rather than a rounding error.
    REQUIRE(at_rest > 1e-4);
    REQUIRE(at_rest < 1e-3);

    // The spec's figure, shown to be the one that is wrong.
    REQUIRE(std::abs(at_rest - Bridge::curvature(0.0) * s * s / 3.0) > 2e-4);
}

TEST_CASE("A3 curvature grows with control drive — the colour comes from the gain element",
          "[diode-bridge][colour]") {
    // THE mechanism that separates this lineage from a VCA or FET design: the
    // gain element itself generates more harmonics the harder it is
    // compressing. Measured with the INPUT HELD CONSTANT and only the control
    // drive changed, so the effect cannot be confused with "a louder input
    // distorts more" — the spec's own A3 recipe raises the input past the
    // threshold, which conflates the two.
    //
    // The prediction is exact: `β(x)/β(0) = 1 + κ·x`, and THD3 is proportional
    // to β to first order.
    const auto thd3 = [](double control_drive) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(0.35);
        bridge.reset();
        const double amplitude = units::db_to_linear(-6.0);
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
            const double y =
                bridge.process(amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr), control_drive);
            if (n >= 4800) out.push_back(y);
        }
        return harmonic_magnitude(out, kToneHz, 3) / harmonic_magnitude(out, kToneHz, 1);
    };

    const double drive_12db = Bridge::control_drive_for_gain_db(-12.0);
    const double at_rest = thd3(0.0);
    const double compressing = thd3(drive_12db);

    REQUIRE(compressing > at_rest);
    REQUIRE_THAT(compressing / at_rest,
                 WithinRel(1.0 + Bridge::kDriveCurvature * drive_12db, 0.02));

    // ...and it is odd-symmetric: the balanced bridge produces no even
    // harmonics, which is the physical claim the four-diode topology makes.
    Bridge bridge;
    bridge.prepare(kSr);
    bridge.set_character(1.0);
    bridge.set_adaa(false);
    bridge.reset();
    std::vector<double> out;
    for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
        const double y = bridge.process(0.9 * std::sin(2.0 * M_PI * kToneHz * n / kSr), 3.0);
        if (n >= 4800) out.push_back(y);
    }
    const double h1 = harmonic_magnitude(out, kToneHz, 1);
    REQUIRE(harmonic_magnitude(out, kToneHz, 3) / h1 > 1e-3);
    for (int k : {2, 4, 6}) REQUIRE(harmonic_magnitude(out, kToneHz, k) / h1 < 1e-9);
}

TEST_CASE("A3 the node's colour deepens as it compresses", "[diode-bridge][colour]") {
    // The same claim end to end, through the full device, which is where a
    // listener meets it.
    const auto node_thd3 = [](double input_db) {
        Comp c = make_compressor();
        c.set_character(0.6);
        c.set_release_ms(Comp::kReleaseMsMax);
        c.reset();
        const double amplitude = units::db_to_linear(input_db);
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < static_cast<int>(kSr) + kTonePeriod * 500; ++n) {
            const double y = c.process(amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr));
            if (n >= static_cast<int>(kSr)) out.push_back(y);
        }
        return harmonic_magnitude(out, kToneHz, 3) / harmonic_magnitude(out, kToneHz, 1);
    };

    REQUIRE(node_thd3(0.0) > node_thd3(-30.0));
}

// ── A4. Anti-aliasing ─────────────────────────────────────────────────────

TEST_CASE("A4 ADAA suppresses the folded harmonic", "[diode-bridge][adaa][aliasing]") {
    // SPEC DEFECT. A4 probes at 8 kHz / 48 kHz. The shaper is a pure cubic, so
    // its ONLY generated harmonic is the third — at exactly 24 kHz, which is
    // Nyquist. `sin(3ωn) = sin(πn) = 0` for every integer n, so the naive
    // shaper's aliased energy at that probe is identically zero (measured
    // −200 dBFS, i.e. the double-precision floor). ADAA is not a pointwise map
    // and does produce a component there (−56.7 dBFS), so at 8 kHz the spec's
    // comparison reports ADAA as 143 dB WORSE than naive. The criterion is not
    // merely hard, it is inverted.
    //
    // The probe is moved to a frequency whose third harmonic genuinely folds
    // and lands on a distinguishable bin: bin 947 of a 4096-point window is
    // 11097.66 Hz, whose third harmonic at 33.29 kHz folds to bin 1255
    // (14.7 kHz). Everything else about the recipe — full scale, maximum
    // character, comparison against the same shaper with ADAA off — is the
    // spec's. The window is 4096 rather than 65536 because a bin-aligned probe
    // needs no window function and the DFT here is direct.
    constexpr int kWindow = 4096;
    constexpr int kProbeBin = 947;

    const auto render = [](bool adaa) {
        Bridge bridge;
        bridge.prepare(kSr);
        bridge.set_character(1.0);
        bridge.set_adaa(adaa);
        bridge.reset();
        std::vector<double> out;
        out.reserve(kWindow);
        for (int n = 0; n < kWindow + kWindow; ++n) {
            const double y =
                bridge.process(std::sin(2.0 * M_PI * kProbeBin * n / double(kWindow)), 0.0);
            if (n >= kWindow) out.push_back(y);
        }
        return out;
    };

    // Every bin except the fundamental: for a cubic the only real content is
    // the folded third harmonic, so summing the rest is a conservative
    // accounting of aliased energy plus the numerical floor.
    const auto analyse = [](const std::vector<double>& x, double* fundamental, double* worst) {
        double energy = 0.0;
        *worst = 0.0;
        for (int bin = 1; bin < kWindow / 2; ++bin) {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < kWindow; ++n) {
                const double w = 2.0 * M_PI * bin * n / double(kWindow);
                re += x[static_cast<std::size_t>(n)] * std::cos(w);
                im += x[static_cast<std::size_t>(n)] * std::sin(w);
            }
            const double magnitude = 2.0 * std::hypot(re, im) / kWindow;
            if (bin == kProbeBin) {
                *fundamental = magnitude;
                continue;
            }
            energy += magnitude * magnitude;
            *worst = std::max(*worst, magnitude);
        }
        return std::sqrt(energy);
    };

    double naive_fundamental = 0.0, naive_worst = 0.0;
    double adaa_fundamental = 0.0, adaa_worst = 0.0;
    const double naive_alias = analyse(render(false), &naive_fundamental, &naive_worst);
    const double adaa_alias = analyse(render(true), &adaa_fundamental, &adaa_worst);

    // The spec's criterion, absolute.
    REQUIRE(units::linear_to_db(naive_alias / adaa_alias) >= 18.0);
    // ...and normalised by the fundamental, so the first-order scheme's own
    // `cos(ω/2)` roll-off cannot be mistaken for alias suppression. This is the
    // stricter reading and it also clears 18 dB at this probe.
    REQUIRE(units::linear_to_db((naive_alias / naive_fundamental) /
                                (adaa_alias / adaa_fundamental)) >= 18.0);
    // No aliased component above −60 dBFS on the shipped path.
    REQUIRE(units::linear_to_db(adaa_worst) < -60.0);
    // The naive path is over that line, so the criterion is discriminating
    // rather than vacuous.
    REQUIRE(units::linear_to_db(naive_worst) > -60.0);
}

// ── A5. Determinism ───────────────────────────────────────────────────────

TEST_CASE("A5 render, reset, re-render is bit-identical", "[diode-bridge][determinism]") {
    // There is no randomness anywhere in the module, so this checks that no
    // uninitialised or carried-over state leaks between renders.
    const auto render = [](Comp& c, int samples) {
        Xorshift32 noise(0xB12DE5u);
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(samples));
        // Pink-ish: a one-pole-integrated white source, deterministic by seed.
        double state = 0.0;
        for (int n = 0; n < samples; ++n) {
            state = 0.98 * state + 0.02 * noise.next_bipolar<double>();
            out.push_back(c.process(0.7 * (state * 6.0)));
        }
        return out;
    };

    Comp c;
    c.prepare(kSr);
    c.set_threshold_db(-15.0);
    c.set_ratio(6.0);
    c.set_knee_db(9.0);
    c.set_attack_ms(7.0);
    c.set_release_ms(250.0);
    c.set_character(0.7);
    c.set_makeup_db(9.0);
    c.set_mix_percent(65.0);
    c.set_sc_hpf_hz(160.0);
    c.set_auto_release(true);
    c.set_feedback(true);
    c.reset();

    const auto first = render(c, static_cast<int>(kSr * 5));
    c.reset();
    const auto second = render(c, static_cast<int>(kSr * 5));

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
}

// ── A6. Latency ───────────────────────────────────────────────────────────

TEST_CASE("A6 the node reports and measures zero latency", "[diode-bridge][latency]") {
    Comp c = make_compressor();
    c.set_character(1.0);
    c.set_feedback(true);
    REQUIRE(c.latency_samples() == 0);

    c.reset();
    const double first = c.process(1.0);
    REQUIRE(std::abs(first) > 0.0);

    // ...and the rest of the impulse response is finite and decays, so "nonzero
    // at n = 0" is not being bought with an unstable path.
    double previous = std::abs(first);
    for (int n = 1; n < 4096; ++n) {
        const double y = c.process(0.0);
        REQUIRE(std::isfinite(y));
        if (n > 512) REQUIRE(std::abs(y) <= previous + 1e-6);
        previous = std::max(previous, std::abs(y));
    }
}

// ── A7. Worst-case gain bound ─────────────────────────────────────────────

TEST_CASE("A7 the worst-case gain bound is the one the registry cites",
          "[diode-bridge][gain][worst-case]") {
    // Series law 8: a tested invariant, not an estimate.
    REQUIRE_THAT(Comp::worst_case_gain(),
                 WithinRel(units::db_to_linear(Comp::kMakeupDbMax), 1e-12));
    REQUIRE_THAT(Comp::worst_case_gain(), WithinAbs(15.8489319, 1e-6));

    // SPEC DEFECT. A7 asks for "instantaneous |output| never exceeds |input| ×
    // 15.85". No signal path containing a filter can satisfy that, and the
    // counterexample is one line long: send a single impulse and the input is
    // zero from the next sample on while the output is still ringing, so the
    // instantaneous ratio is unbounded by inspection. What is bounded — and
    // what the registry constant actually means — is the ratio of output PEAK
    // to input PEAK over a render, which is what is asserted.
    //
    // The measurement is taken after the brackets' cold-start transient. That
    // is not a convenience: a linear filter's peak-to-peak gain on a transient
    // is its impulse response's L1 norm, which is strictly larger than the
    // supremum of its magnitude response, so a cold start legitimately exceeds
    // a frequency-domain bound. The excess is measured and accounted for
    // below rather than waved at.
    const auto sweep = [](double seconds, bool skip_transient) {
        double worst = 0.0;
        for (double db = -60.0; db <= 12.001; db += 3.0) {
            Comp c;
            c.prepare(kSr);
            c.set_makeup_db(Comp::kMakeupDbMax);
            c.set_character(1.0);
            c.set_ratio(Comp::kRatioMin);
            c.set_threshold_db(Comp::kThresholdDbMax);
            c.reset();
            const double amplitude = units::db_to_linear(db);
            const int total = static_cast<int>(kSr * seconds);
            const int start = skip_transient ? static_cast<int>(kSr * 0.1) : 0;
            double in_peak = 0.0, out_peak = 0.0;
            for (int n = 0; n < total; ++n) {
                const double x = amplitude * std::sin(2.0 * M_PI * kToneHz * n / kSr);
                const double y = c.process(x);
                if (n < start) continue;
                in_peak = std::max(in_peak, std::abs(x));
                out_peak = std::max(out_peak, std::abs(y));
            }
            worst = std::max(worst, out_peak / in_peak);
        }
        return worst;
    };

    // THE INVARIANT: the settled peak ratio never exceeds the registry bound,
    // at any level, at the least-reducing setting with makeup wide open.
    REQUIRE(sweep(0.5, true) <= Comp::worst_case_gain());
    // ...and it gets close, so the bound is reached rather than merely
    // respected — a bound nothing approaches proves nothing.
    REQUIRE(sweep(0.5, true) > 0.98 * Comp::worst_case_gain());

    // The cold-start excess, accounted for from a COMPONENT measurement rather
    // than a fudge factor: it is exactly the two brackets' own small-signal
    // cold-start peak gain, squared.
    Bracket bracket;
    bracket.prepare(kSr);
    bracket.set_character(1.0);
    bracket.reset();
    double bracket_in = 0.0, bracket_out = 0.0;
    for (int n = 0; n < static_cast<int>(kSr * 0.1); ++n) {
        const double x = 1e-3 * std::sin(2.0 * M_PI * kToneHz * n / kSr);
        bracket_in = std::max(bracket_in, std::abs(x));
        bracket_out = std::max(bracket_out, std::abs(bracket.process(x)));
    }
    const double bracket_transient = bracket_out / bracket_in;
    REQUIRE(bracket_transient > 1.0);   // a real, if tiny, transient overshoot
    REQUIRE(bracket_transient < 1.02);  // and a small one
    REQUIRE(sweep(0.5, false) <=
            Comp::worst_case_gain() * bracket_transient * bracket_transient * 1.005);
}

TEST_CASE("A7 the bound holds for noise and impulses too", "[diode-bridge][gain][worst-case]") {
    Comp c;
    c.prepare(kSr);
    c.set_makeup_db(Comp::kMakeupDbMax);
    c.set_character(1.0);
    c.set_ratio(Comp::kRatioMin);
    c.set_threshold_db(Comp::kThresholdDbMax);
    c.reset();

    Xorshift32 noise(0x5EED1234u);
    double in_peak = 0.0, out_peak = 0.0;
    for (int n = 0; n < static_cast<int>(kSr * 2); ++n) {
        double x = noise.next_bipolar<double>();
        if (n % 7919 == 0) x = units::db_to_linear(12.0);  // a full-scale-plus spike
        in_peak = std::max(in_peak, std::abs(x));
        out_peak = std::max(out_peak, std::abs(c.process(x)));
    }
    REQUIRE(out_peak / in_peak <= Comp::worst_case_gain());
}

TEST_CASE("A7 the dry path cannot push the mix past the bound",
          "[diode-bridge][gain][worst-case]") {
    // Makeup applies to the wet path only, so a parallel blend interpolates
    // between unity and the wet gain and can never exceed the larger of the
    // two. Asserted because the alternative wiring — makeup after the sum —
    // would put the dry signal through the makeup stage and is an easy thing
    // to "simplify" into later.
    for (double mix : {0.0, 25.0, 50.0, 75.0, 100.0}) {
        Comp c;
        c.prepare(kSr);
        c.set_makeup_db(Comp::kMakeupDbMax);
        c.set_character(1.0);
        c.set_ratio(Comp::kRatioMin);
        c.set_threshold_db(Comp::kThresholdDbMax);
        c.set_mix_percent(mix);
        c.reset();
        double in_peak = 0.0, out_peak = 0.0;
        const int total = static_cast<int>(kSr * 0.5);
        for (int n = 0; n < total; ++n) {
            const double x = 0.01 * std::sin(2.0 * M_PI * kToneHz * n / kSr);
            const double y = c.process(x);
            if (n < static_cast<int>(kSr * 0.1)) continue;
            in_peak = std::max(in_peak, std::abs(x));
            out_peak = std::max(out_peak, std::abs(y));
        }
        REQUIRE(out_peak / in_peak <= Comp::worst_case_gain());
    }
}

// ── A8. Ballistics ────────────────────────────────────────────────────────

TEST_CASE("A8 the follower's nominal time is a 10-90 percent rise time",
          "[diode-bridge][ballistics]") {
    // SPEC DEFECT, and its independent ground truth. A8 asks for the 63 % point
    // to land "within ±15 % of attack_ms / release_ms". `BallisticsFilterT`'s
    // coefficient is `1 − exp(−2.2/(ms·fs))`, and 2.2 is `ln 9` — the
    // 10 %-to-90 % rise-time convention `units.hpp` documents explicitly. Its
    // 63 % point is therefore at `ms/ln 9` = 45.5 % of nominal, so the spec's
    // criterion is off by a factor of 2.2 before the gain computer's dB-domain
    // mapping is even considered. Measured directly here so the adjudication
    // rests on the shipped follower rather than on reading its source.
    for (double ms : {3.0, 10.0, 400.0}) {
        BallisticsFilter64 follower;
        follower.prepare(kSr);
        follower.set_attack_ms(ms);
        follower.set_release_ms(ms);
        follower.reset();
        int n = 0;
        while (follower.process(1.0) < 0.63212) ++n;
        REQUIRE_THAT((1000.0 * n / kSr) / ms, WithinRel(1.0 / std::log(9.0), 0.015));
    }
}

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

TEST_CASE("A8 the release lands where the follower's convention predicts",
          "[diode-bridge][ballistics]") {
    // With the reference value corrected, the spec's ±15 % tolerance is kept.
    //
    // The prediction is computed here from the SHIPPED pieces: the `ln 9`
    // convention proved above, and the gain computer's own inverse. The
    // envelope decays as `e(n) = lo + (hi − lo)·exp(−n·ln9/N)`; the reduction
    // has fallen 63.2 % when `e` reaches the level whose static-curve output is
    // 36.8 % of the loud value; solving for n gives the fraction of nominal
    // below. For a −20 → 0 dBFS step at threshold −12, ratio 4, that is 0.474.
    constexpr double kQuietDb = -20.0;
    constexpr double kLoudDb = 0.0;
    const double quiet = units::db_to_linear(kQuietDb);
    const double loud = units::db_to_linear(kLoudDb);

    Comp curve = make_compressor();
    curve.set_knee_db(0.0);
    const double loud_reduction = -curve.static_curve_db(kLoudDb);
    const double target_reduction = 0.368 * loud_reduction;
    // Invert the hard-region characteristic to the envelope that produces it.
    const double target_db = -12.0 + target_reduction / (1.0 - 1.0 / 4.0);
    const double target_envelope = units::db_to_linear(target_db);
    const double predicted_fraction =
        -std::log((target_envelope - quiet) / (loud - quiet)) / std::log(9.0);

    for (double release : {100.0, 400.0, 1600.0}) {
        const auto step = measure_step(3.0, release, false);
        REQUIRE_THAT(step.release_ms / release, WithinRel(predicted_fraction, 0.15));
    }
}

TEST_CASE("A8 the ballistics controls are calibrated in their stated units",
          "[diode-bridge][ballistics]") {
    // The release is proportional to `release_ms` to well inside 5 %: the
    // follower decays continuously, so nothing gates it.
    const double a = measure_step(3.0, 100.0, false).release_ms / 100.0;
    const double b = measure_step(3.0, 400.0, false).release_ms / 400.0;
    const double c = measure_step(3.0, 1600.0, false).release_ms / 1600.0;
    REQUIRE_THAT(b, WithinRel(a, 0.05));
    REQUIRE_THAT(c, WithinRel(a, 0.05));

    // The ATTACK is not proportional, and the reason is structural rather than
    // a calibration error: a peak follower can only rise while `|x|` exceeds
    // its state, so a tonal stimulus gates the attack for most of each cycle
    // while nothing gates the release. The measured 63 % point runs from 1.18×
    // `attack_ms` at 3 ms down to 0.72× at 30 ms. What IS assertable — and what
    // a user needs — is that the control is monotonic and lands within a factor
    // well under two of its stated value.
    const double fast = measure_step(3.0, 400.0, false).attack_ms;
    const double medium = measure_step(10.0, 400.0, false).attack_ms;
    const double slow = measure_step(30.0, 400.0, false).attack_ms;
    REQUIRE(fast < medium);
    REQUIRE(medium < slow);
    REQUIRE(fast > 0.4 * 3.0);
    REQUIRE(fast < 1.3 * 3.0);
    REQUIRE(slow > 0.4 * 30.0);
    REQUIRE(slow < 1.3 * 30.0);

    // And the attack is far faster than the release at the defaults, which is
    // the ordering that makes the device a compressor rather than a gate.
    const auto defaults = measure_step(3.0, 400.0, false);
    REQUIRE(defaults.attack_ms * 20.0 < defaults.release_ms);
}

TEST_CASE("A8 feedback detection softens the realised ratio", "[diode-bridge][ballistics]") {
    // The documented signature of this lineage, asserted as the strict
    // inequality the spec asks for — plus the closed-form fixed point, which is
    // stronger and which the module ships as `static_curve_feedback_db()`.
    const auto forward = measure_step(3.0, 400.0, false);
    const auto looped = measure_step(3.0, 400.0, true);

    REQUIRE(looped.steady_reduction_db < forward.steady_reduction_db);

    Comp c = make_compressor();
    c.set_knee_db(0.0);
    // Feed-forward at 0 dBFS with threshold −12 and ratio 4: 12 dB over,
    // reduction 9 dB. Feedback: 9/1.75 = 5.14 dB, an effective 1.75:1.
    REQUIRE_THAT(-c.static_curve_db(0.0), WithinAbs(9.0, 1e-9));
    REQUIRE_THAT(-c.static_curve_feedback_db(0.0), WithinAbs(9.0 / 1.75, 1e-9));

    // The measurements track those closed forms once the detector's own
    // under-read of a tone is allowed for.
    REQUIRE_THAT(forward.steady_reduction_db, WithinAbs(9.0, 0.3));
    REQUIRE_THAT(looped.steady_reduction_db, WithinAbs(9.0 / 1.75, 0.3));

    // The effective ratio really is gentler than the knob says.
    const double effective_ratio = 12.0 / (12.0 - looped.steady_reduction_db);
    REQUIRE(effective_ratio < 4.0);
    REQUIRE(effective_ratio > 1.0);
}

TEST_CASE("A8 makeup gain stays outside the feedback loop", "[diode-bridge][ballistics]") {
    // The spec pins the tap at `output_pre_makeup`, and the reason is not
    // cosmetic: makeup inside the loop would make the makeup knob a second,
    // hidden threshold control, so raising the output level by 12 dB would
    // silently add ~7 dB of reduction and the compressor would fight its own
    // gain staging. The invariant is that gain reduction is INDEPENDENT of
    // makeup — which is also the property no other case in this suite covers,
    // because every other feedback measurement runs at 0 dB makeup.
    const auto reduction_with_makeup = [](double makeup_db) {
        Comp c = make_compressor();
        c.set_knee_db(0.0);
        c.set_feedback(true);
        c.set_release_ms(Comp::kReleaseMsMax);
        c.set_makeup_db(makeup_db);
        c.reset();
        for (int n = 0; n < static_cast<int>(kSr * 3.0); ++n)
            c.process(0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr));
        return c.gain_reduction_db();
    };

    const double baseline = reduction_with_makeup(0.0);
    REQUIRE(baseline < -1.0);  // it really is compressing, so the check has teeth
    for (double makeup : {6.0, 12.0, Comp::kMakeupDbMax})
        REQUIRE_THAT(reduction_with_makeup(makeup), WithinAbs(baseline, 1e-9));
}

TEST_CASE("A8 the feedback fixed point converges across the whole ratio range",
          "[diode-bridge][ballistics]") {
    // The solve is averaged specifically because the plain iteration `gr ←
    // f(gr)` has slope `1/ρ − 1`, which is exactly −1 in the limit region: it
    // oscillates between two values forever without narrowing, and returns
    // whichever one the iteration cap lands on. This asserts the residual is
    // driven to double precision at every ratio, including the top of the
    // control where the plain form fails outright.
    Comp c = make_compressor();
    c.set_knee_db(0.0);
    for (double ratio : {Comp::kRatioMin, 2.0, 4.0, 10.0, Comp::kLimitRatio, Comp::kRatioMax}) {
        c.set_ratio(ratio);
        for (double level : {-6.0, 0.0, 6.0, 18.0}) {
            const double gr = c.static_curve_feedback_db(level);
            REQUIRE_THAT(c.static_curve_db(level + gr), WithinAbs(gr, 1e-12));
        }
    }

    // The hard-region closed form, which is what the doc block's worked example
    // quotes: GR = (L − thr)·(1/ρ − 1)/(2 − 1/ρ).
    c.set_ratio(4.0);
    const double over = 12.0;
    const double slope = 1.0 / 4.0 - 1.0;
    REQUIRE_THAT(c.static_curve_feedback_db(0.0),
                 WithinAbs(over * slope / (1.0 - slope), 1e-12));
}

TEST_CASE("A8 auto release is program-dependent, not just slow",
          "[diode-bridge][ballistics]") {
    // SPEC AMENDMENT, documented in the header: `max(fast, slow)` is
    // identically the slow follower, because during release both decay from the
    // same value and the slow one is always the larger. That would make "auto"
    // a synonym for "six times the release time" with nothing program-dependent
    // about it. The shipped blend is asserted to sit strictly BETWEEN the two
    // manual settings — which is the observable difference between a blend and
    // a maximum.
    const double manual_fast = measure_step(3.0, 200.0, false).release_ms;
    const double manual_slow =
        measure_step(3.0, 200.0 * Comp::kAutoSlowFactor, false).release_ms;
    REQUIRE(manual_slow > manual_fast);

    Comp c = make_compressor();
    c.set_knee_db(0.0);
    c.set_release_ms(200.0);
    c.set_auto_release(true);
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
    const double loud = reduction[static_cast<std::size_t>(pre + hold - 1)];
    const double recovered = reduction.back();
    const double target = loud + 0.63212 * (recovered - loud);
    int index = pre + hold;
    while (index < static_cast<int>(reduction.size()) &&
           reduction[static_cast<std::size_t>(index)] > target)
        ++index;
    const double automatic = 1000.0 * (index - pre - hold) / kSr;

    REQUIRE(automatic > manual_fast);
    REQUIRE(automatic <= manual_slow);
}

// ── A9. Sidechain high-pass ───────────────────────────────────────────────

TEST_CASE("A9 the sidechain high-pass de-sensitises the low end",
          "[diode-bridge][sidechain]") {
    // SPEC DEFECT in the rationale, though the criterion survives at a stated
    // ratio. A9 asserts "the 60 Hz tone produces ≥ 4 dB less GR than the 1 kHz
    // tone", and justifies it with the high-pass's ~5.7 dB of relative
    // attenuation at 60 Hz. Those are different quantities: a level change
    // reaches the REDUCTION multiplied by `(1 − 1/ratio)`, so at the ratio
    // control's minimum of 1.5 the same 5.7 dB of rejection is only 1.9 dB of
    // reduction difference and the criterion fails. A9 does not pin the ratio;
    // it is pinned here, and the derived lower bound is asserted alongside the
    // spec's round number so the mechanism is what is being tested.
    const auto reduction_at = [](double tone_hz, double ratio) {
        Comp c = make_compressor();
        c.set_threshold_db(-20.0);
        c.set_ratio(ratio);
        c.set_knee_db(0.0);
        c.set_sc_hpf_hz(100.0);
        c.set_release_ms(Comp::kReleaseMsMax);
        c.reset();
        double sum = 0.0;
        int count = 0;
        const int total = static_cast<int>(kSr * 3.0);
        for (int n = 0; n < total; ++n) {
            c.process(0.5 * std::sin(2.0 * M_PI * tone_hz * n / kSr));
            if (n > total - static_cast<int>(kSr)) {
                sum += -c.gain_reduction_db();
                ++count;
            }
        }
        return sum / count;
    };

    constexpr double kCorner = 100.0;
    constexpr double kRatio = 4.0;
    // A one-pole high-pass at `kCorner` passes `f/√(f² + fc²)`; the derived
    // reduction difference is that relative attenuation times the gain
    // computer's slope.
    const double relative_attenuation_db =
        units::linear_to_db(1000.0 / std::hypot(1000.0, kCorner)) -
        units::linear_to_db(60.0 / std::hypot(60.0, kCorner));
    const double derived = relative_attenuation_db * (1.0 - 1.0 / kRatio);

    const double low = reduction_at(60.0, kRatio);
    const double mid = reduction_at(1000.0, kRatio);
    REQUIRE(mid - low >= 4.0);            // the spec's criterion, at a stated ratio
    REQUIRE(mid - low >= 0.95 * derived);  // ...and the mechanism that produces it

    // The ratio dependence, asserted rather than described: at the control's
    // minimum the same high-pass cannot deliver 4 dB, which is why the ratio
    // has to be pinned.
    REQUIRE(relative_attenuation_db * (1.0 - 1.0 / Comp::kRatioMin) < 4.0);

    // Opening the corner right down makes the compressor respond to the low end
    // again — the "does it duck to the kick?" knob doing its job.
    Comp wide = make_compressor();
    wide.set_threshold_db(-20.0);
    wide.set_ratio(kRatio);
    wide.set_knee_db(0.0);
    wide.set_sc_hpf_hz(Comp::kScHpfHzMin);
    wide.set_release_ms(Comp::kReleaseMsMax);
    wide.reset();
    double sum = 0.0;
    int count = 0;
    const int total = static_cast<int>(kSr * 3.0);
    for (int n = 0; n < total; ++n) {
        wide.process(0.5 * std::sin(2.0 * M_PI * 60.0 * n / kSr));
        if (n > total - static_cast<int>(kSr)) {
            sum += -wide.gain_reduction_db();
            ++count;
        }
    }
    REQUIRE(sum / count > low + 2.0);
}

// ── The transformer brackets ──────────────────────────────────────────────

TEST_CASE("the bracket generates even harmonics, which needs an asymmetry",
          "[diode-bridge][transformer]") {
    // SPEC DEFECT. §5 prescribes `sat(u) = u − (a/2)·u·|u|` and describes it as
    // adding a second harmonic. It cannot: `u·|u|` is an ODD function, so that
    // shaper is odd-symmetric and produces odd harmonics only — physically the
    // right answer for a SYMMETRIC magnetic core, and the wrong one for the
    // even-harmonic weight §0 and §5 both attribute to the brackets. Even
    // harmonics in a transformer come from asymmetry, so `kEvenAsymmetry`
    // supplies it and the shipped curve is
    //
    //     sat(u) = u − (a/2)·u·|u| − (a·ε/2)·u²
    //
    // which is the spec's formula exactly at ε = 0.
    //
    // Asserted structurally first: the even part of the curve is EXACTLY the
    // added term, so the amendment is measurable rather than asserted.
    Bracket bracket;
    bracket.prepare(kSr);
    bracket.set_character(1.0);
    const double a = Bracket::kSaturationDepth;
    for (double u = 0.05; u <= 1.5; u += 0.05) {
        const double even_part = 0.5 * (bracket.saturate(u) + bracket.saturate(-u));
        REQUIRE_THAT(even_part, WithinAbs(-0.5 * a * Bracket::kEvenAsymmetry * u * u, 1e-12));
    }
    // Slope exactly 1 at the origin, at every depth — series law 1. Asserted
    // against the closed form of the central difference rather than against 1
    // with a fudged tolerance: `(sat(h) − sat(−h))/2h = 1 − a·h/2` exactly, so
    // the residual is the difference scheme's own truncation and vanishes
    // linearly in h. Asserting `≈ 1` would be asserting that truncation is
    // small, which is a weaker claim about a different thing.
    for (double character : {0.0, 0.35, 1.0}) {
        bracket.set_character(character);
        const double depth = Bracket::kSaturationDepth * character;
        for (double h : {1e-4, 1e-6, 1e-8}) {
            REQUIRE_THAT((bracket.saturate(h) - bracket.saturate(-h)) / (2.0 * h),
                         WithinAbs(1.0 - 0.5 * depth * h, 1e-12));
        }
    }

    // ...and it shows up in the spectrum, even-forward.
    const auto render = [](double character) {
        Bracket b;
        b.prepare(kSr);
        b.set_character(character);
        b.reset();
        std::vector<double> out;
        out.reserve(kTonePeriod * 500);
        for (int n = 0; n < 4800 + kTonePeriod * 500; ++n) {
            const double y = b.process(0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr));
            if (n >= 4800) out.push_back(y);
        }
        return out;
    };

    const auto coloured = render(1.0);
    const double h1 = harmonic_magnitude(coloured, kToneHz, 1);
    const double h2 = harmonic_magnitude(coloured, kToneHz, 2);
    const double h3 = harmonic_magnitude(coloured, kToneHz, 3);
    REQUIRE(h2 > h3);  // even-forward, the documented transformer character

    // The second harmonic's amplitude is closed-form: the added term is
    // `−(a·ε/2)·u²`, and `sin²θ = (1 − cos2θ)/2`, so `h2 = a·ε·A²/4`.
    const double amplitude = 0.5;
    REQUIRE_THAT(h2 / h1,
                 WithinRel(a * Bracket::kEvenAsymmetry * amplitude / 4.0, 0.06));

    // At zero character the nonlinearity is exactly off: the bracket is a pure
    // band-limit, not a shaper that happens to be shallow.
    const auto clean = render(0.0);
    const double clean_h1 = harmonic_magnitude(clean, kToneHz, 1);
    for (int k : {2, 3, 4, 5})
        REQUIRE(harmonic_magnitude(clean, kToneHz, k) / clean_h1 < 1e-9);
}

TEST_CASE("the bracket's peak gain is exactly one", "[diode-bridge][transformer][gain]") {
    // The `Tpeak = 1.0` the worst-case bound multiplies by, asserted as the
    // construction it claims to be: every stage has magnitude ≤ 1, so no
    // amplitude and no frequency can produce gain.
    Bracket bracket;
    bracket.prepare(kSr);
    for (double character : {0.0, 0.5, 1.0}) {
        bracket.set_character(character);
        // The memoryless stage, over its whole guarded range and both signs.
        for (double u = -4.0; u <= 4.0; u += 0.01) {
            if (std::abs(u) < 1e-9) continue;
            REQUIRE(std::abs(bracket.saturate(u) / u) <= 1.0 + 1e-12);
        }
        // And the whole stage, settled, across the audio band.
        for (double hz : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) {
            bracket.reset();
            double in_peak = 0.0, out_peak = 0.0;
            const int total = static_cast<int>(kSr * 0.3);
            for (int n = 0; n < total; ++n) {
                const double x = 0.5 * std::sin(2.0 * M_PI * hz * n / kSr);
                const double y = bracket.process(x);
                if (n < total / 2) continue;
                in_peak = std::max(in_peak, std::abs(x));
                out_peak = std::max(out_peak, std::abs(y));
            }
            REQUIRE(out_peak / in_peak <= Bracket::kPeakGain);
        }
    }
}

TEST_CASE("the bracket blocks DC that its own saturation generates",
          "[diode-bridge][transformer]") {
    // The even-harmonic term rectifies, so it produces a program-dependent DC
    // offset. Placing the high-pass AFTER the saturator is what keeps that
    // offset out of the output sum and out of the feedback detector, which
    // would otherwise read it as signal and hold gain down through a silent
    // passage.
    Bracket bracket;
    bracket.prepare(kSr);
    bracket.set_character(1.0);
    bracket.reset();

    double mean = 0.0;
    int count = 0;
    const int total = static_cast<int>(kSr * 2.0);
    for (int n = 0; n < total; ++n) {
        const double y = bracket.process(0.8 * std::sin(2.0 * M_PI * 200.0 * n / kSr));
        if (n > total / 2) {
            mean += y;
            ++count;
        }
    }
    REQUIRE_THAT(mean / count, WithinAbs(0.0, 1e-4));
}

TEST_CASE("the winding corner is clamped below Nyquist at base rates",
          "[diode-bridge][transformer]") {
    // Worth stating rather than discovering: the declared 28 kHz corner is
    // ABOVE Nyquist at 44.1 and 48 kHz, so the house TPT filter clamps it and
    // the bracket is effectively full-bandwidth there. The declared value is
    // only realised from 88.2 kHz up. This is a property of the sample rate,
    // not a defect, and it is asserted so a future change to the corner cannot
    // silently become a change to the audio-band response at 48 kHz.
    TptFilter64 winding;
    winding.prepare(kSr);
    winding.set_cutoff(Bracket::kHighCornerHz);
    REQUIRE(winding.cutoff() < Bracket::kHighCornerHz);
    REQUIRE(winding.cutoff() > 0.4 * kSr);

    TptFilter64 high_rate;
    high_rate.prepare(96000.0);
    high_rate.set_cutoff(Bracket::kHighCornerHz);
    REQUIRE_THAT(high_rate.cutoff(), WithinRel(Bracket::kHighCornerHz, 1e-12));
}

// ── A10. RT allocation ────────────────────────────────────────────────────

TEST_CASE("A10 the module allocates nothing on the audio thread",
          "[diode-bridge][rt-safety]") {
    DiodeBridgeGainT<float> bridge;
    TransformerBracketT<float> bracket;
    DiodeBridgeCompressorT<float> node;
    bridge.prepare(kSr);
    bracket.prepare(kSr);
    node.prepare(kSr);

    require_allocates_no_memory([&] {
        // Ten seconds, with every control moving, because a setter that
        // allocates only on a value change is the failure mode a static
        // configuration would miss.
        for (int n = 0; n < static_cast<int>(kSr * 10.0); ++n) {
            const auto phase = 0.0001f * static_cast<float>(n);
            const float character = 0.5f + 0.5f * std::sin(phase);
            bridge.set_character(character);
            bridge.set_adaa((n % 2) == 0);
            bracket.set_character(character);
            bracket.set_adaa((n % 3) == 0);
            node.set_character(character);
            node.set_threshold_db(-20.0 + 10.0 * std::sin(phase));
            node.set_ratio(2.0 + 8.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_knee_db(9.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_attack_ms(1.0 + 20.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_release_ms(100.0 + 500.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_makeup_db(12.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_mix_percent(100.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_sc_hpf_hz(50.0 + 200.0 * (0.5 + 0.5 * std::sin(phase)));
            node.set_auto_release((n % 4096) < 2048);
            node.set_feedback((n % 8192) < 4096);

            const float x = 0.5f * std::sin(0.05f * static_cast<float>(n));
            (void)bridge.process(x, 1.0);
            (void)bracket.process(x);
            (void)node.process(x);
        }
        bridge.reset();
        bracket.reset();
        node.reset();
    });
}

// ── A11. float / double parity ────────────────────────────────────────────

TEST_CASE("A11 the float and double instantiations agree on the gain law",
          "[diode-bridge][precision]") {
    DiodeBridgeGainT<float> single;
    DiodeBridgeGainT<double> dual;
    single.prepare(kSr);
    dual.prepare(kSr);
    single.set_character(0.35);
    dual.set_character(0.35);
    single.set_adaa(false);
    dual.set_adaa(false);
    single.reset();
    dual.reset();

    for (double x : {0.0, 0.413, 2.981, 9.0}) {
        for (int n = 0; n < 4800; ++n) {
            const double input = 0.25 * std::sin(2.0 * M_PI * 440.0 * n / kSr);
            const double a = static_cast<double>(single.process(static_cast<float>(input), x));
            const double b = dual.process(input, x);
            REQUIRE_THAT(a, WithinAbs(b, 1e-6 * (0.25 + std::abs(b))));
        }
    }

    // The whole node too, to a tolerance the float path can actually hold: the
    // detector is a recursive one-pole, so single precision accumulates rather
    // than merely rounds.
    DiodeBridgeCompressorT<float> node_f;
    DiodeBridgeCompressorT<double> node_d;
    node_f.prepare(kSr);
    node_d.prepare(kSr);
    node_f.set_character(0.5);
    node_d.set_character(0.5);
    node_f.reset();
    node_d.reset();
    for (int n = 0; n < 24000; ++n) {
        const double input = 0.6 * std::sin(2.0 * M_PI * 220.0 * n / kSr);
        REQUIRE_THAT(static_cast<double>(node_f.process(static_cast<float>(input))),
                     WithinAbs(node_d.process(input), 1e-4));
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

TEST_CASE("zero-init is a valid fresh instance", "[diode-bridge][lifecycle]") {
    // POD state, zero-init = fresh (series contract §6). A default-constructed,
    // never-prepared instance must not produce NaN or run away.
    Comp c;
    for (int n = 0; n < 1024; ++n) {
        const double y = c.process(0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr));
        REQUIRE(std::isfinite(y));
    }

    Bridge bridge;
    Bracket bracket;
    for (int n = 0; n < 1024; ++n) {
        const double x = 0.5 * std::sin(2.0 * M_PI * kToneHz * n / kSr);
        REQUIRE(std::isfinite(bridge.process(x, 2.0)));
        REQUIRE(std::isfinite(bracket.process(x)));
    }
}

TEST_CASE("controls clamp to their declared ranges", "[diode-bridge][lifecycle]") {
    // The catalog table's ranges are the module's contract; a setter that lets
    // a host push past them is a way for an automation curve to reach a state
    // no test covers.
    Comp c;
    c.prepare(kSr);
    c.set_knee_db(0.0);

    c.set_ratio(1000.0);
    c.set_threshold_db(-1000.0);
    // Ratio pinned at the maximum is the limit region, so a 6 dB overshoot of
    // the lowest threshold is 6 dB of reduction exactly.
    REQUIRE_THAT(c.static_curve_db(Comp::kThresholdDbMin + 6.0), WithinAbs(-6.0, 1e-9));

    c.set_makeup_db(1000.0);
    Xorshift32 noise(7u);
    double in_peak = 0.0, out_peak = 0.0;
    c.set_mix_percent(1000.0);
    c.set_sc_hpf_hz(-1000.0);
    c.set_attack_ms(-1.0);
    c.set_release_ms(1e9);
    c.reset();
    for (int n = 0; n < static_cast<int>(kSr); ++n) {
        const double x = 0.01 * noise.next_bipolar<double>();
        in_peak = std::max(in_peak, std::abs(x));
        out_peak = std::max(out_peak, std::abs(c.process(x)));
    }
    REQUIRE(out_peak / in_peak <= Comp::worst_case_gain());
}

TEST_CASE("a NaN sample cannot latch the diode-bridge feedback detector",
          "[diode-bridge][nan-recovery]") {
    for (double sample_rate : {8000.0, 192000.0}) {
        Comp c;
        c.prepare(sample_rate);
        c.set_threshold_db(-30.0);
        c.set_feedback(true);
        c.set_auto_release(true);
        for (int i = 0; i < static_cast<int>(sample_rate * 0.1); ++i) c.process(0.5);

        c.process(std::numeric_limits<double>::quiet_NaN());
        for (int i = 0; i < 64; ++i) {
            REQUIRE(std::isfinite(c.process(0.25)));
            REQUIRE(std::isfinite(c.gain_reduction_db()));
            REQUIRE(std::isfinite(c.control_drive()));
        }
    }
}

TEST_CASE("enabling auto release cannot blend against a stale slow follower",
          "[diode-bridge][ballistics]") {
    Comp automatic;
    Comp manual;
    automatic.prepare(kSr);
    manual.prepare(kSr);
    for (Comp* c : {&automatic, &manual}) {
        c->set_feedback(false);
        c->set_threshold_db(-30.0);
        c->set_ratio(4.0);
        c->set_knee_db(0.0);
        c->set_attack_ms(3.0);
        c->set_release_ms(400.0);
        c->set_auto_release(false);
    }

    for (int i = 0; i < static_cast<int>(kSr); ++i) {
        automatic.process(0.5);
        manual.process(0.5);
    }
    REQUIRE_THAT(automatic.gain_reduction_db(), WithinAbs(manual.gain_reduction_db(), 1e-12));

    automatic.set_auto_release(true);
    double maximum_premature_recovery = 0.0;
    for (int i = 0; i < 256; ++i) {
        automatic.process(0.0);
        manual.process(0.0);
        maximum_premature_recovery =
            std::max(maximum_premature_recovery,
                     automatic.gain_reduction_db() - manual.gain_reduction_db());
    }

    // A correctly synchronized slow follower is never below the fast follower
    // during release, so Auto can hold MORE reduction than manual but must not
    // recover prematurely. A frozen-at-zero slow state reverses that ordering.
    REQUIRE(maximum_premature_recovery < 1e-9);
}
