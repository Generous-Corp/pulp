// FuzzPairT — the two-transistor direct-coupled feedback pair.
//
// The spec's acceptance suite (see planning/2026-07-25-dsp-series-round2.md,
// module M03). Expected values are computed from the shipped device rows and
// circuit constants, never restated — so the documented 363 mV / 693 mV
// conduction knees are checked as CONSEQUENCES of (n, Is) rather than as
// independent literals that could drift from them.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/fuzz_pair.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Fuzz = FuzzPairT<double>;
constexpr double kSr = 48000.0;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

Fuzz make_fuzz(FuzzDevice device = FuzzDevice::germanium, double fuzz = 0.65,
               double starve = 0.0, double source_kohm = 10.0) {
    Fuzz f;
    f.prepare(kSr);
    f.set_device(device);
    f.set_fuzz(fuzz);
    f.set_bias_starve(starve);
    f.set_source_impedance_kohm(source_kohm);
    f.reset();
    return f;
}

/// The conduction knee the shipped (n, Is) row implies, at 1 calibration unit
/// of collector current. This is the spec's §3 worked example, evaluated rather
/// than transcribed.
double expected_knee(FuzzDevice device) {
    const double n = device == FuzzDevice::germanium ? Fuzz::kGermaniumIdeality
                                                     : Fuzz::kSiliconIdeality;
    const double is = device == FuzzDevice::germanium ? Fuzz::kGermaniumSaturation
                                                      : Fuzz::kSiliconSaturation;
    return n * Fuzz::kThermalVoltage *
           std::log(Fuzz::kNominalCollectorCurrent / is + 1.0);
}

/// Renders a sine and returns the analysis window.
std::vector<double> render_sine(Fuzz& f, double freq_hz, double amp, int length,
                                int settle = 9600) {
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(length));
    const double w = 2.0 * std::numbers::pi * freq_hz / kSr;
    for (int n = 0; n < settle + length; ++n) {
        const double y = f.process(amp * std::sin(w * n));
        if (n >= settle) out.push_back(y);
    }
    return out;
}

/// Coherent DFT magnitude at harmonic `k`. Exact when the window holds a whole
/// number of periods, which every call below arranges.
double harmonic(const std::vector<double>& x, double fundamental_hz, int k) {
    const double w = 2.0 * std::numbers::pi * k * fundamental_hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

double rms(const std::vector<double>& x) {
    double sum = 0.0;
    for (double v : x) sum += v * v;
    return std::sqrt(sum / static_cast<double>(x.size()));
}

/// Total harmonic distortion over harmonics 2..9.
double thd(const std::vector<double>& x, double fundamental_hz) {
    double sum_sq = 0.0;
    for (int k = 2; k <= 9; ++k) {
        const double h = harmonic(x, fundamental_hz, k);
        sum_sq += h * h;
    }
    return std::sqrt(sum_sq) / harmonic(x, fundamental_hz, 1);
}

}  // namespace

// ── 1. Device-law shape ───────────────────────────────────────────────────

TEST_CASE("1 the conduction knees fall out of the shipped device rows",
          "[fuzz][device]") {
    // The modelling order the spec insists on: pick (n, Is), let the knee
    // EMERGE. A model that hard-codes "0.25 V for Ge, 0.65 V for Si" as a clip
    // threshold loses the continuous, current-dependent softness that is the
    // actual difference between the devices — so this asserts the knee is a
    // consequence, and separately that it lands where the worked example says.
    for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
        auto f = make_fuzz(device);
        REQUIRE_THAT(f.bias_voltage(), WithinRel(expected_knee(device), 1e-9));
    }

    // The worked example's figures, to ±5 % — germanium inside the documented
    // 0.2–0.35 V band and silicon inside 0.6–0.7 V.
    const double germanium = expected_knee(FuzzDevice::germanium);
    const double silicon = expected_knee(FuzzDevice::silicon);
    REQUIRE_THAT(germanium, WithinRel(0.363, 0.05));
    REQUIRE_THAT(silicon, WithinRel(0.693, 0.05));

    // And the ORDERING, which is what the six-decade gap in Is buys.
    REQUIRE(germanium < silicon);
    REQUIRE(silicon - germanium > 0.25);
}

// ── Gain staging: the scale the rest of the suite rests on ────────────────

TEST_CASE("the stage gain is set by the degeneration, not by the bias point",
          "[fuzz][gain-staging]") {
    // Everything downstream — the cleanup, the octave, the solver's
    // conditioning — depends on the two stages sharing a scale, and nothing
    // observable says so directly. Without emitter degeneration the mid-rail
    // bias pins `R_c`, the stage gain collapses to `R_c/(n*V_T)`, and the
    // interstage coupling hands a stage 85% of a full rail through a window
    // fourteen times narrower than that. Every setting then clips identically.
    // These are the numbers that made that true, so a regression in the
    // staging is caught here rather than as three unrelated-looking failures.
    for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
        auto f = make_fuzz(device);
        const double theta = (device == FuzzDevice::germanium ? Fuzz::kGermaniumIdeality
                                                              : Fuzz::kSiliconIdeality) *
                             Fuzz::kThermalVoltage;
        const double load = (1.0 - Fuzz::kQuiescentCollector) / Fuzz::kNominalCollectorCurrent;
        const double gm = Fuzz::kNominalCollectorCurrent / theta;
        const double gm_eff = gm / (1.0 + gm * Fuzz::kEmitterDegeneration);

        // The gain is the degenerated one, computed from the shipped rows.
        REQUIRE_THAT(f.stage_gain(), WithinRel(load * gm_eff, 1e-12));

        // ...and it is an order of magnitude below what the bias point alone
        // would force, which is the whole point of the element.
        REQUIRE(f.stage_gain() < 0.15 * load * gm);

        // The interstage overdrive: how many of the receiving stage's input
        // windows a FULL rail out of the sending stage represents. A couple is
        // the design intent — a hot input slams the pair, a rolled-back guitar
        // does not. Fourteen is the failure this replaced.
        const double overdrive = Fuzz::kCouplingGain * f.stage_gain();
        REQUIRE(overdrive > 1.0);
        REQUIRE(overdrive < 4.0);
    }

    // The cleanup's precondition, asserted on the drive itself rather than only
    // through the rendered level: a full-scale input past a 10 kohm source
    // exceeds the stage's linear window (half a swing either side of quiescent)
    // and clips, while the same input past 220 kohm stays well inside it.
    //
    // The comparison is against the CLOSED-loop drive. The feedback is not a
    // trim on top of the divider here — at these settings it multiplies the
    // drive by nearly three, so the open-loop figure alone sits below the window
    // at both impedances and says nothing about which one clips.
    auto low = make_fuzz(FuzzDevice::germanium, 0.8, 0.0, 10.0);
    auto high = make_fuzz(FuzzDevice::germanium, 0.8, 0.0, 220.0);
    const double half_window = 0.5 / low.stage_gain();
    const auto closed_loop_drive = [](const Fuzz& f) {
        return f.input_scale_volts() * f.loading_factor() / (1.0 - f.loop_gain());
    };
    REQUIRE(closed_loop_drive(low) > half_window);
    REQUIRE(closed_loop_drive(high) < 0.5 * half_window);
}

// ── 2. Source impedance: two consequences, one ratio ──────────────────────

TEST_CASE("2 a higher source impedance cleans up beyond the level drop",
          "[fuzz][source-impedance]") {
    // The defining behaviour, and the one most emulations implement only half
    // of. Raising the source impedance attenuates the signal AND lightens Q1's
    // loading, which lowers the loop gain. If only the attenuation were
    // modelled, the level drop would exactly equal the divider ratio and the
    // distortion would be unchanged.
    constexpr double kLowKohm = 10.0;
    constexpr double kHighKohm = 220.0;
    constexpr double kToneHz = 200.0;
    constexpr int kLength = 24000;  // whole periods at 200 Hz

    auto low = make_fuzz(FuzzDevice::germanium, 0.8, 0.0, kLowKohm);
    auto high = make_fuzz(FuzzDevice::germanium, 0.8, 0.0, kHighKohm);

    const double amp = units::db_to_linear(-6.0);
    const auto low_out = render_sine(low, kToneHz, amp, kLength);
    const auto high_out = render_sine(high, kToneHz, amp, kLength);

    // The attenuation-only prediction, computed from the shipped input
    // impedance rather than restated.
    const double attenuation_only_db =
        units::linear_to_db(high.loading_factor() / low.loading_factor());
    const double measured_db = units::linear_to_db(rms(high_out) / rms(low_out));

    // (a) The measured drop EXCEEDS the divider's prediction — evidence of the
    // loop-gain reduction on top of the pure attenuation.
    REQUIRE(measured_db < attenuation_only_db - 1.0);

    // (b) ...and it is cleaner, not merely quieter.
    REQUIRE(thd(high_out, kToneHz) < thd(low_out, kToneHz));

    // The loop gain really did fall, which is the mechanism behind both.
    REQUIRE(high.loop_gain() < low.loop_gain());
}

// ── 3. The loop-gain bound: the registry invariant ────────────────────────

TEST_CASE("3 loop gain never reaches the ceiling anywhere on the grid",
          "[fuzz][gain]") {
    // Series law 1's tested invariant and what the Forge registry cites. The
    // registry value is the MEASURED maximum, with the ceiling as its budget.
    double measured_max = 0.0;
    constexpr int kPointsPerAxis = 33;

    for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
        for (int fi = 0; fi < kPointsPerAxis; ++fi) {
            for (int si = 0; si < kPointsPerAxis; ++si) {
                for (int zi = 0; zi < kPointsPerAxis; ++zi) {
                    const double fuzz = static_cast<double>(fi) / (kPointsPerAxis - 1);
                    const double starve = static_cast<double>(si) / (kPointsPerAxis - 1);
                    const double z = units::taper_log(
                        static_cast<double>(zi) / (kPointsPerAxis - 1), 0.1, 1000.0);
                    auto f = make_fuzz(device, fuzz, starve, z);
                    const double gain = f.loop_gain();
                    REQUIRE(gain >= 0.0);
                    REQUIRE(gain <= Fuzz::kLoopGainCeiling);
                    measured_max = std::max(measured_max, gain);
                }
            }
        }
    }

    // Not a vacuous bound: the worst case actually approaches it, so the
    // ceiling is the budget rather than a number chosen far above the truth.
    REQUIRE(measured_max > 0.9 * Fuzz::kLoopGainCeiling);
    // The registry populates from THIS number.
    INFO("Forge registry worst_case_gain should cite the measured maximum: " << measured_max);
    REQUIRE(measured_max <= Fuzz::kLoopGainCeiling);
}

TEST_CASE("3 the ceiling is reached at the configuration the design predicts",
          "[fuzz][gain]") {
    // Maximum feedback, healthy bias, zero source impedance, and SILICON —
    // whose smaller ideality factor means a steeper exponential and therefore
    // more transconductance at the same current. Germanium is not the binding
    // device, which is worth pinning because it is the counterintuitive half.
    auto silicon = make_fuzz(FuzzDevice::silicon, 1.0, 0.0, 0.1);
    auto germanium = make_fuzz(FuzzDevice::germanium, 1.0, 0.0, 0.1);
    REQUIRE(silicon.loop_gain() > germanium.loop_gain());
    REQUIRE_THAT(silicon.loop_gain(), WithinRel(Fuzz::kLoopGainCeiling, 0.01));
}

// ── 4. Gating and sputter ─────────────────────────────────────────────────

TEST_CASE("4 a starved bias gates, and the gate tracks the envelope",
          "[fuzz][starvation]") {
    // Starvation lowers the QUIESCENT BIAS toward cutoff. The consequence is
    // that a decaying note crosses the point where it can no longer turn the
    // stage on — an amplitude-dependent dropout, not a fixed threshold applied
    // to the output.
    const auto tail_rms = [](double starve) {
        auto f = make_fuzz(FuzzDevice::germanium, 0.8, starve);
        const double w = 2.0 * std::numbers::pi * 220.0 / kSr;
        const auto attack = static_cast<int>(kSr * 0.2);
        const auto decay = static_cast<int>(kSr * 2.0);

        double sustained_sum = 0.0, tail_sum = 0.0;
        int sustained_n = 0, tail_n = 0;
        for (int i = 0; i < attack + decay; ++i) {
            // 200 ms at −3 dBFS, then a 2 s exponential decay.
            const double envelope =
                i < attack ? units::db_to_linear(-3.0)
                           : units::db_to_linear(-3.0) *
                                 std::exp(-5.0 * static_cast<double>(i - attack) / decay);
            const double y = f.process(envelope * std::sin(w * i));
            if (i > attack / 2 && i < attack) {
                sustained_sum += y * y;
                ++sustained_n;
            } else if (i > attack + decay * 3 / 4) {
                tail_sum += y * y;
                ++tail_n;
            }
        }
        const double sustained = std::sqrt(sustained_sum / sustained_n);
        const double tail = std::sqrt(tail_sum / tail_n);
        return units::linear_to_db(tail / sustained);
    };

    const double healthy = tail_rms(0.0);
    const double starved = tail_rms(0.85);

    // The starved render's tail collapses far below its own sustained level —
    // at least 20 dB, per the spec — and much further than the healthy one's.
    REQUIRE(starved < -20.0);
    REQUIRE(starved < healthy - 10.0);
}

TEST_CASE("4 starvation lowers the bias toward cutoff", "[fuzz][starvation]") {
    // The mechanism, asserted directly: the available current falls with the
    // stated exponent and the quiescent bias follows it down. Clamping the
    // current mid-solve instead would leave the bias where it was and produce
    // silence rather than gating.
    auto healthy = make_fuzz(FuzzDevice::germanium, 0.8, 0.0);
    auto starved = make_fuzz(FuzzDevice::germanium, 0.8, 0.85);

    const double expected_available =
        std::pow(1.0 - 0.85, Fuzz::kStarveExponent) * Fuzz::kNominalCollectorCurrent;
    REQUIRE_THAT(starved.available_current(), WithinRel(expected_available, 1e-9));
    // The spec's worked example: roughly 4.8 % of nominal.
    REQUIRE_THAT(starved.available_current(), WithinAbs(0.048, 0.005));
    REQUIRE(starved.bias_voltage() < healthy.bias_voltage());

    // The bias falls by exactly the knee shift the same junction law predicts
    // for that current — starvation moves the operating point along the device
    // curve rather than applying a separate offset.
    const double theta = Fuzz::kGermaniumIdeality * Fuzz::kThermalVoltage;
    const double expected_shift =
        theta * std::log((Fuzz::kNominalCollectorCurrent / Fuzz::kGermaniumSaturation + 1.0) /
                         (expected_available / Fuzz::kGermaniumSaturation + 1.0));
    REQUIRE_THAT(healthy.bias_voltage() - starved.bias_voltage(),
                 WithinRel(expected_shift, 1e-9));

    // Starvation is applied to the SECOND stage only: its collector walks toward
    // the supply rail, which is the headroom asymmetry the octave-up needs,
    // while the first stage keeps a full mid-rail swing to drive it with. A
    // model that starves both stages instead loses the loop gain as the square
    // of the current and gates nothing — see the header's bias-starvation note.
    REQUIRE_THAT(healthy.quiescent_collector(), WithinRel(Fuzz::kQuiescentCollector, 1e-12));
    REQUIRE(starved.quiescent_collector() > 0.9);
    // The first stage is untouched, so the pair's overall gain is NOT simply
    // squared down: the loop gain falls by one stage's worth, not two.
    const double stage_ratio = expected_available / Fuzz::kNominalCollectorCurrent;
    REQUIRE(starved.loop_gain() > healthy.loop_gain() * stage_ratio * stage_ratio);
}

// ── 5. Octave-up under misbias ────────────────────────────────────────────

TEST_CASE("5 a misbiased operating point generates even harmonics",
          "[fuzz][harmonics]") {
    // No octave parameter is involved anywhere: the even-harmonic content is
    // the spectral consequence of an asymmetric operating point.
    constexpr double kToneHz = 220.0;
    // 220 Hz at 48 kHz is 2400/11 samples per period, so 24000 samples is
    // exactly 110 periods — a leakage-free window without a Hann correction.
    constexpr int kLength = 24000;

    const auto second_over_first = [](double starve) {
        auto f = make_fuzz(FuzzDevice::germanium, 1.0, starve);
        const auto out = render_sine(f, kToneHz, 0.5, kLength);
        return units::linear_to_db(harmonic(out, kToneHz, 2) / harmonic(out, kToneHz, 1));
    };

    const double healthy = second_over_first(0.0);
    const double misbiased = second_over_first(0.7);
    REQUIRE(misbiased > healthy + 6.0);
}

// ── 6. Germanium drifts, silicon barely does ──────────────────────────────

TEST_CASE("6 germanium's thermal drift far exceeds silicon's", "[fuzz][drift]") {
    // Germanium's reverse saturation current is far more temperature-sensitive
    // than silicon's, which is why germanium units are famous for changing
    // character with the room. The drift depth ratio is 12x by the shipped
    // constants; a 5x floor leaves margin for the walk's own variance.
    const auto drift_deviation = [](FuzzDevice device) {
        auto f = make_fuzz(device, 0.8);
        f.set_drift_enabled(true);
        f.set_seed(20260725u);
        f.reset();

        // Silence in: the drift channel is the only thing moving.
        double sum = 0.0, sum_sq = 0.0;
        constexpr int n = 240000;  // 5 s
        for (int i = 0; i < n; ++i) {
            const double y = f.process(0.0);
            sum += y;
            sum_sq += y * y;
        }
        const double mean = sum / n;
        return std::sqrt(std::max(sum_sq / n - mean * mean, 0.0));
    };

    const double germanium = drift_deviation(FuzzDevice::germanium);
    const double silicon = drift_deviation(FuzzDevice::silicon);
    REQUIRE(germanium > 0.0);
    REQUIRE(germanium > silicon * 5.0);

    // The shipped depths really are an order of magnitude apart.
    REQUIRE(Fuzz::kGermaniumDriftOctaves > Fuzz::kSiliconDriftOctaves * 10.0);
}

// ── 7. Solver convergence ─────────────────────────────────────────────────

TEST_CASE("7 the fixed iteration count converges across the grid",
          "[fuzz][solver]") {
    // The spec asks for a residual under `kResidualTolerance` after the fixed
    // iteration count at every grid point, and both devices now clear it —
    // including the deeply starved settings, which is where a fixed count has
    // the least room. Two things buy that: the emitter degeneration puts
    // `1 + R_e·gm` on the Jacobian's diagonal, damping every step by the same
    // factor that makes the exponential steep; and the warm start pre-solves
    // each stage's base-voltage equation so the iteration begins a step or two
    // away rather than hundreds of millivolts away.
    //
    // The assertion is non-vacuous: the solve only records a residual below
    // tolerance when it actually broke out early, so a regression that stops
    // converging leaves the last iteration's residual here instead.
    double worst = 0.0;
    constexpr int kPointsPerAxis = 9;

    for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
        for (int fi = 0; fi < kPointsPerAxis; ++fi) {
            for (int si = 0; si < kPointsPerAxis; ++si) {
                const double fuzz = static_cast<double>(fi) / (kPointsPerAxis - 1);
                const double starve = static_cast<double>(si) / (kPointsPerAxis - 1);
                auto f = make_fuzz(device, fuzz, starve);
                // Full-scale drive: the largest operating-point excursions the
                // solver will ever be asked for.
                const double w = 2.0 * std::numbers::pi * 440.0 / kSr;
                for (int i = 0; i < 4800; ++i) f.process(std::sin(w * i));
                worst = std::max(worst, f.worst_residual());
            }
        }
    }

    INFO("worst converged residual across the grid: " << worst);
    REQUIRE(worst < Fuzz::kResidualTolerance);
}

TEST_CASE("7 the fixed-cost solve holds its tolerance across source impedance and transients",
          "[fuzz][solver]") {
    // The loop-gain ceiling is a small-signal statement, not a convergence
    // proof for a nonlinear solve away from the operating point. Exercise the
    // full device/fuzz/starve grid at source impedances spanning the complete
    // accepted range, including the binding 0.1 kohm endpoint, and use a
    // full-scale square so every configuration has to recover from the largest
    // legitimate sample-to-sample step. A sine at the nominal impedance does
    // not cover either of those conditioning axes.
    constexpr int kPointsPerControlAxis = 9;
    constexpr double kSourceKohm[] = {0.1, 1.0, 10.0, 68.0, 220.0, 1000.0};
    constexpr int kSamplesPerPolarity = 8;
    constexpr int kPeriods = 4;

    double worst = 0.0;
    FuzzDevice worst_device = FuzzDevice::germanium;
    double worst_fuzz = 0.0;
    double worst_starve = 0.0;
    double worst_source = 0.0;

    for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
        for (int fi = 0; fi < kPointsPerControlAxis; ++fi) {
            for (int si = 0; si < kPointsPerControlAxis; ++si) {
                const double fuzz = static_cast<double>(fi) / (kPointsPerControlAxis - 1);
                const double starve = static_cast<double>(si) / (kPointsPerControlAxis - 1);
                for (double source_kohm : kSourceKohm) {
                    auto f = make_fuzz(device, fuzz, starve, source_kohm);
                    // The solver itself is independent of the reconstruction
                    // filter. Bypassing it makes the square's discontinuity
                    // arrive intact and avoids disguising a bad solve with FIR
                    // smoothing.
                    f.set_oversampling_enabled(false);
                    f.reset();
                    for (int i = 0; i < 2 * kSamplesPerPolarity * kPeriods; ++i) {
                        const double x = (i / kSamplesPerPolarity) % 2 == 0 ? 1.0 : -1.0;
                        f.process(x);
                    }

                    if (f.worst_residual() > worst) {
                        worst = f.worst_residual();
                        worst_device = device;
                        worst_fuzz = fuzz;
                        worst_starve = starve;
                        worst_source = source_kohm;
                    }
                }
            }
        }
    }

    INFO("worst residual=" << worst
                            << " device="
                            << (worst_device == FuzzDevice::germanium ? "germanium" : "silicon")
                            << " fuzz=" << worst_fuzz << " starve=" << worst_starve
                            << " source_kohm=" << worst_source);
    REQUIRE(worst < Fuzz::kResidualTolerance);
}

TEST_CASE("7 both devices hold the tolerance over a long full-scale render",
          "[fuzz][solver]") {
    // The grid above covers breadth; this covers duration, so a slow drift in
    // the operating point that only shows up after a second of audio is caught
    // here rather than nowhere.
    for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
        auto f = make_fuzz(device, 0.8);
        const double w = 2.0 * std::numbers::pi * 440.0 / kSr;
        for (int i = 0; i < 48000; ++i) f.process(std::sin(w * i));
        REQUIRE(f.worst_residual() < Fuzz::kResidualTolerance);
    }
}

TEST_CASE("a non-finite sample resets recursive state and finite audio recovers",
          "[fuzz][nan-recovery]") {
    auto f = make_fuzz(FuzzDevice::silicon, 1.0, 0.75, 0.1);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double w = 2.0 * std::numbers::pi * 8000.0 / kSr;

    // A non-finite sample must never enter the coupled collector state or the
    // oversampling filters. Alternating it with finite 8 kHz input was the
    // shortest way to demonstrate the old permanent latch.
    for (int i = 0; i < 512; ++i) {
        const double input = (i % 2 == 0) ? nan : std::sin(w * i);
        const double output = f.process(input);
        REQUIRE(std::isfinite(output));
        if (!std::isfinite(input)) REQUIRE(output == 0.0);
    }

    // Recovery means more than returning a finite sentinel for the bad sample:
    // subsequent ordinary audio must traverse the rebuilt recursive state and
    // become non-silent again.
    REQUIRE(f.process(nan) == 0.0);
    auto fresh = make_fuzz(FuzzDevice::silicon, 1.0, 0.75, 0.1);
    double peak = 0.0;
    for (int i = 0; i < 4096; ++i) {
        const double output = f.process(0.5 * std::sin(w * i));
        REQUIRE(std::isfinite(output));
        REQUIRE(output == fresh.process(0.5 * std::sin(w * i)));
        peak = std::max(peak, std::abs(output));
    }
    REQUIRE(peak > 1e-4);
}

// ── 8. Aliasing suppression ───────────────────────────────────────────────

TEST_CASE("8 oversampling suppresses the aliased image band", "[fuzz][aliasing]") {
    // A 4 kHz tone at maximum fuzz generates harmonics far past Nyquist. With
    // oversampling off they fold back into the audible band; with it on the
    // half-band pair removes them.
    constexpr double kToneHz = 4000.0;
    constexpr int kLength = 24000;

    const auto image_energy = [](bool oversampled) {
        auto f = make_fuzz(FuzzDevice::silicon, 1.0);
        f.set_oversampling_enabled(oversampled);
        f.reset();
        const auto out = render_sine(f, kToneHz, 1.0, kLength);

        // 200 Hz – 3 kHz: below the fundamental and clear of its harmonics, so
        // anything here folded down from above Nyquist.
        double energy = 0.0;
        for (double hz = 200.0; hz <= 3000.0; hz += 50.0) {
            const double m = harmonic(out, hz, 1);
            energy += m * m;
        }
        return energy;
    };

    const double raw = image_energy(false);
    const double filtered = image_energy(true);
    const double reduction_db = 10.0 * std::log10(raw / filtered);
    INFO("alias-band reduction: " << reduction_db << " dB");
    REQUIRE(reduction_db > 12.0);
}

// ── 9. Determinism ────────────────────────────────────────────────────────

TEST_CASE("9 render, reset, re-render is bit-identical", "[fuzz][determinism]") {
    for (bool drift : {false, true}) {
        for (auto device : {FuzzDevice::germanium, FuzzDevice::silicon}) {
            auto f = make_fuzz(device, 0.8, 0.3);
            f.set_drift_enabled(drift);
            f.set_seed(31337u);
            f.reset();

            const auto render = [&](int n) {
                std::vector<double> out;
                out.reserve(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i) {
                    const double t = i / kSr;
                    out.push_back(f.process(0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * t) +
                                            0.3 * std::sin(2.0 * std::numbers::pi * 660.0 * t)));
                }
                return out;
            };

            const auto first = render(static_cast<int>(kSr * 2));
            f.reset();
            const auto second = render(static_cast<int>(kSr * 2));
            REQUIRE(first.size() == second.size());
            for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
        }
    }
}

// ── 10. Latency ───────────────────────────────────────────────────────────

TEST_CASE("10 latency is reported exactly and matches the measured delay",
          "[fuzz][latency]") {
    // A linear-phase FIR pair cannot be zero-latency, so this is not claimed to
    // be. The spec states 16 samples from a 65-tap pair at 4x; Pulp's shared
    // oversampler cascades a 129-tap and a 49-tap stage instead, so the honest
    // number is ITS reported latency — see adjudication A-3.
    auto f = make_fuzz();
    const int reported = f.latency_samples();
    REQUIRE(reported > 0);

    // Confirm against the impulse response rather than trusting the claim.
    f.set_fuzz(0.0);
    f.set_source_impedance_kohm(0.1);
    f.reset();
    int peak_index = -1;
    double peak_value = 0.0;
    for (int i = 0; i < reported * 4; ++i) {
        const double y = f.process(i == 0 ? 0.05 : 0.0);
        if (std::abs(y) > peak_value) {
            peak_value = std::abs(y);
            peak_index = i;
        }
    }
    REQUIRE(peak_index == reported);

    // With oversampling disabled there is no filter and therefore no latency.
    f.set_oversampling_enabled(false);
    REQUIRE(f.latency_samples() == 0);
}

// ── float/double parity ───────────────────────────────────────────────────

TEST_CASE("the float and double instantiations agree", "[fuzz]") {
    FuzzPairT<float> single;
    FuzzPairT<double> dbl;
    for (auto* p : {static_cast<void*>(&single), static_cast<void*>(&dbl)}) (void)p;
    single.prepare(kSr);
    dbl.prepare(kSr);
    single.set_fuzz(0.7);
    dbl.set_fuzz(0.7);
    single.reset();
    dbl.reset();

    REQUIRE(single.latency_samples() == dbl.latency_samples());

    const double w = 2.0 * std::numbers::pi * 330.0 / kSr;
    for (int i = 0; i < 24000; ++i) {
        const double x = 0.6 * std::sin(w * i);
        REQUIRE_THAT(static_cast<double>(single.process(static_cast<float>(x))),
                     WithinAbs(dbl.process(x), 1e-3));
    }
}

// ── 11. RT allocation probe ───────────────────────────────────────────────

TEST_CASE("11 the fuzz pair allocates nothing on the audio thread",
          "[fuzz][rt-safety]") {
    FuzzPairT<float> single;
    FuzzPairT<double> dbl;
    single.prepare(kSr);
    dbl.prepare(kSr);
    single.set_drift_enabled(true);
    dbl.set_drift_enabled(true);

    std::vector<float> block_f(256, 0.1f);
    std::vector<float> out_f(256, 0.0f);
    std::vector<double> block_d(256, 0.1);
    std::vector<double> out_d(256, 0.0);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 64; ++i) {
            const auto device = (i % 2) ? FuzzDevice::silicon : FuzzDevice::germanium;
            single.set_device(device);
            single.set_fuzz(0.01 * (i % 100));
            single.set_bias_starve(0.01 * (i % 100));
            single.set_source_impedance_kohm(0.1 + 10.0 * i);
            single.set_output_level_db(-12.0 + 0.25 * i);
            single.set_mix(0.5);
            dbl.set_device(device);
            dbl.set_fuzz(0.01 * (i % 100));
            dbl.set_bias_starve(0.01 * (i % 100));

            (void)single.process(0.5f);
            single.process_block(block_f.data(), out_f.data(), static_cast<int>(block_f.size()));
            (void)dbl.process(0.5);
            dbl.process_block(block_d.data(), out_d.data(), static_cast<int>(block_d.size()));
        }
        (void)single.process(std::numeric_limits<float>::quiet_NaN());
        (void)dbl.process(std::numeric_limits<double>::quiet_NaN());
        single.reset();
        dbl.reset();
    });
}

TEST_CASE("non-finite fuzz controls retain the complete non-default configuration",
          "[fuzz][nan-recovery]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Fuzz poisoned, reference;
        for (auto* fuzz : {&poisoned, &reference}) {
            fuzz->prepare(kSr); fuzz->set_device(FuzzDevice::silicon);
            fuzz->set_fuzz(0.83); fuzz->set_bias_starve(0.42);
            fuzz->set_source_impedance_kohm(137.0);
            fuzz->set_output_level_db(-7.0); fuzz->set_mix(0.61);
        }
        poisoned.set_fuzz(bad); poisoned.set_bias_starve(bad);
        poisoned.set_source_impedance_kohm(bad);
        poisoned.set_output_level_db(bad); poisoned.set_mix(bad);
        for (int i = 0; i < 512; ++i) {
            const double x = 0.29 * std::sin(0.063 * i);
            REQUIRE(poisoned.process(x) == reference.process(x));
        }
    }
}
