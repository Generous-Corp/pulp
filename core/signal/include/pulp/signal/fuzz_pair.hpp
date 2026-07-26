#pragma once

/// @file fuzz_pair.hpp
/// The two-transistor direct-coupled feedback pair — the fuzz circuit whose
/// character comes from its FEEDBACK, not from its clipping.
///
/// A diode clipper distorts because a diode conducts. This circuit distorts
/// because two exponential junctions are wrapped in a feedback loop that runs
/// deliberately close to instability, and the loop's proximity to that edge is
/// the sag and bloom the family is known for. Everything characteristic about
/// it follows from that one structural fact:
///
/// - **It cleans up from the guitar's volume knob far more than a gain-staged
///   distortion does.** Raising the source impedance attenuates the signal AND
///   reduces how much of the feedback the input node returns to Q1's base, which
///   lowers the loop gain. Two effects, one ratio, both pushing the same
///   direction — at the tested settings the divider accounts for 11 dB of the
///   drop and the loop gain for another 7. A model that implements only the
///   attenuation reproduces half the behaviour — see
///   `set_source_impedance_kohm` and the adjudication in `loop_gain()`.
/// - **Starving the bias makes it gate and sputter per note**, not per level. A
///   loud attack passes, the decaying tail of that same note falls below the
///   available current and cuts out, and the next attack re-opens it. That is
///   the starved stage's own behaviour, not a noise gate bolted on the end — a
///   gate would trigger on absolute level and get the dynamics wrong.
/// - **Misbiasing it produces octave-up content** with no octave parameter
///   anywhere: an asymmetric operating point generates even harmonics.
/// - **Germanium and silicon differ only in `(n, Is)`.** The ~0.36 V and
///   ~0.69 V conduction knees are EMERGENT from a six-decade difference in
///   saturation current, not hard-coded thresholds. Hard-coding them would lose
///   the continuous, current- and temperature-dependent softness that is the
///   actual difference between the two devices.
///
/// ## The circuit model
///
/// Each stage is a common-emitter amplifier with emitter degeneration, in
/// normalised units, biased at 1.0 calibration unit of collector current:
///
/// ```
/// i(v_be) = Is·(exp(v_be/(n·V_T)) − 1)          // Ebers-Moll, forward-active term
/// v_b     = v_be + R_e·i                        // base node — the emitter rises with i
/// v_c     = clamp(1 − R_c·i, 0, 1)              // collector node, normalised rail
/// ```
///
/// The collector load is what bounds the circuit. Without it the junction
/// current grows without limit — at the germanium defaults a 0.9 V drive gives
/// `i ≈ 7.2e7` — and a feedback path fed by an unbounded current has no
/// solution at all. `R_c` is set so the quiescent collector sits mid-rail,
/// which is also where the clipping headroom is symmetric.
///
/// The emitter degeneration `R_e` is what makes the two stages composable, and
/// it is the element a reduced model is most tempted to drop. Drop it and
/// `R_c` — pinned by the mid-rail bias — forces a stage gain of ~19, so a
/// stage's whole usable input window is 50 mV while the stage before it hands
/// over 85 % of a full rail. Every setting then clips identically, the cleanup
/// has nothing to show, and the solver never leaves the steep part of the
/// exponential. With it, `gm_eff = gm/(1 + gm·R_e)`, the gain approaches the
/// resistor ratio `R_c/R_e`, and the two stages' scales match. See
/// `kEmitterDegeneration`.
///
/// The stages couple through collector-voltage DEVIATION from each stage's own
/// quiescent point, which is what the feedback resistor's DC servo achieves in
/// the real circuit while keeping the interstage coupling direct.
///
/// ## Why the solver has a fixed cost
///
/// The pair is solved by a fixed-count safeguarded 2×2 Newton descent. The
/// small-signal loop-gain bound keeps the operating point stable, but does not
/// by itself bound nonlinear conditioning at a collector-rail kink or after a
/// full-scale discontinuity. The solve therefore uses a capped ladder of
/// trust-region Newton and residual-gradient candidates and accepts the one with
/// the smallest coupled residual. The full device/control/source-impedance grid,
/// including full-scale square transitions, independently asserts the residual
/// tolerance.
///
/// The loop is **explicitly bounded rather than unity-compensated** (series law
/// 1). Compensating it to unity would remove the module's reason for existing:
/// this circuit is supposed to run near the edge.
///
/// ## Bias starvation
///
/// Starvation lowers the current available to the SECOND stage — the supply
/// feeding its collector node is what runs down — and therefore lowers that
/// stage's quiescent bias, rather than clamping the current mid-solve. Two
/// distinctions, both of which decide whether the module gates or merely goes
/// quiet:
///
/// - **Lowering the bias, not clamping the current.** With the bias lowered the
///   stage sits near cutoff, so small signals do not turn it on and loud
///   transients do — gating that tracks per-note dynamics. Clamping the current
///   instead pins the collector at a constant, which compresses loud passages
///   and leaves quiet ones alone: the opposite of a gate.
/// - **Starving one stage, not both.** A healthy first stage still swinging its
///   full rail into a second stage that can no longer follow it is what gates.
///   Starving both leaves the loop gain falling as the SQUARE of the current, so
///   the feedback path dies long before the operating point is asymmetric enough
///   to fold — and the feedback amplifying that asymmetry is the whole mechanism
///   behind the octave-up.
///
/// ## RT contract
///
/// `prepare()` may allocate — it builds the oversampler's filter state. `set_*`,
/// `process()`, `process_block()` and `reset()` never allocate, never lock, and
/// never perform I/O. The implicit solve runs a COMPILE-TIME-FIXED iteration
/// count, so the worst-case time per sample is bounded and not data-dependent.
/// All randomness is the seeded house `OuWalkT`, rewound by `reset()` and never
/// automatable (series law 2).
///
/// References: Ebers & Moll, "Large-Signal Behavior of Junction Transistors",
/// Proc. IRE 42(12):1761–1772, 1954, for the forward-active junction law.
/// ElectroSmash's public Fuzz Face analysis for the topology and its documented
/// behaviour only — no component values are reproduced and every default here
/// is independently chosen and independently ranged.

#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/junction.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/oversampling.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// The two device families. They differ only in `(n, Is)`.
enum class FuzzDevice : std::uint8_t {
    germanium,  ///< Large `Is`: conducts from ~0.36 V, softer knee, drifts more.
    silicon,    ///< Small `Is`: conducts from ~0.69 V, harder knee, stable.
};

/// The two-transistor direct-coupled feedback pair.
template <typename SampleType = float>
class FuzzPairT {
public:
    using Device = FuzzDevice;

    // ── Device rows ───────────────────────────────────────────────────────
    // Ideality factor sets knee SOFTNESS; saturation current sets where the
    // knee falls. Both are honest [design parameter]s standing for a device
    // family's published order of magnitude, never a measured part.

    /// [design parameter] germanium n 1.15 (range 1.05 .. 1.30).
    static constexpr double kGermaniumIdeality = 1.15;
    /// [design parameter] germanium Is 5e-6 calibration units (range 1e-7 .. 5e-5).
    static constexpr double kGermaniumSaturation = 5e-6;
    /// [design parameter] silicon n 1.03 (range 1.00 .. 1.08).
    static constexpr double kSiliconIdeality = 1.03;
    /// [design parameter] silicon Is 5e-12 calibration units (range 1e-13 .. 5e-11).
    static constexpr double kSiliconSaturation = 5e-12;

    /// The shared thermal voltage. Named here so this module's own formulas
    /// read in its terms, but there is exactly one definition, in
    /// `junction.hpp`.
    static constexpr double kThermalVoltage = junction::kThermalVoltage;

    // ── Circuit constants ─────────────────────────────────────────────────

    /// Quiescent collector current, the unit everything else is relative to.
    /// [design parameter] default 1.0, range 0.5 .. 2.0 calibration units.
    static constexpr double kNominalCollectorCurrent = 1.0;

    /// Where the quiescent collector sits on the normalised rail. Mid-rail
    /// gives symmetric clipping headroom.
    /// [design parameter] default 0.5, range 0.3 .. 0.7.
    static constexpr double kQuiescentCollector = 0.5;

    /// Emitter degeneration `R_e`, in the same normalised volts-per-calibration-
    /// unit as the collector load.
    ///
    /// This is the element that makes the stage gain a DESIGN CHOICE instead of
    /// an accident of the bias point, and leaving it out is not a simplification
    /// — it is a different circuit. Without it, putting the quiescent collector
    /// mid-rail fixes `R_c = 0.5/I_q`, and the stage gain `R_c·gm` collapses to
    /// `0.5/(n·V_T) ≈ 19` no matter what else is chosen. A gain of 19 means the
    /// base-voltage window that swings the collector across its whole rail is
    /// ~50 mV, while the interstage coupling hands the next stage 85 % of a full
    /// rail — a 14x overdrive before any input arrives, so the second stage is a
    /// square wave at every setting and every source impedance. The circuit's
    /// signature cleanup then does not exist to be measured, and the solver
    /// never leaves the steepest part of the exponential.
    ///
    /// With degeneration the emitter voltage rises with the current the base
    /// asks for, so `gm_eff = gm/(1 + gm·R_e)` and the stage gain approaches the
    /// resistor ratio `R_c/R_e` — independent of the operating current. That is
    /// how the real circuit does it too: the Fuzz-Face-class second stage sits
    /// on an emitter resistor precisely so its input window is volts-scale and
    /// matches the previous collector's swing.
    ///
    /// The default gives a stage gain near 2.4, so a FULL-rail swing out of the
    /// first stage overdrives the second by about 2x — enough that a hot input
    /// slams the pair, little enough that a rolled-back guitar does not.
    /// [design parameter] default 0.18, range 0.05 .. 0.5.
    static constexpr double kEmitterDegeneration = 0.18;

    /// Starvation exponent: how sharply available current falls with the
    /// starve control. [design parameter] default 1.6, range 1.0 .. 3.0.
    static constexpr double kStarveExponent = 1.6;

    /// Floor on available current, so a fully starved stage is near cutoff
    /// rather than exactly at it (which would make the bias voltage −inf).
    /// [design parameter] default 1e-3, range 1e-5 .. 1e-2.
    static constexpr double kMinAvailableCurrent = 1e-3;

    /// The fuzz stage's own input impedance at Q1's base, in kΩ. Forms the
    /// divider with the source impedance.
    /// [design parameter] default 68 kΩ, range 20 .. 200 kΩ.
    static constexpr double kInputImpedanceKohm = 68.0;

    /// Feedback conductance travel. The top of the range is what
    /// `kLoopGainCeiling` constrains.
    /// [design parameter] min 0.05 (range 0 .. 0.2), max 1.0 (range 0.5 .. 1.5).
    static constexpr double kFeedbackConductanceMin = 0.05;
    static constexpr double kFeedbackConductanceMax = 1.0;

    /// Interstage coupling loss, Q1's collector into Q2's base.
    /// [design parameter] default 0.85, range 0.6 .. 1.0.
    static constexpr double kCouplingGain = 0.85;

    /// The enforced loop-gain ceiling. Deliberately short of the 1.0
    /// instability boundary: the floor of its range keeps audible sag headroom,
    /// the ceiling keeps the measured maximum safely stable. It is also the
    /// solver's contraction condition — see the file doc block.
    /// [design parameter] default 0.94, range 0.85 .. 0.98.
    static constexpr double kLoopGainCeiling = 0.94;

    /// Fixed solver iterations. Not a loop bound to be tuned at runtime — the
    /// RT contract depends on it being compile-time constant.
    /// [design parameter] 24 iterations, sufficient for the fixed trust-region
    /// ladder to clear the residual tolerance at the nonlinear rail kinks.
    static constexpr int kSolverIterations = 24;

    /// Per-iteration Newton step limit, in thermal voltages. Junction-limiting:
    /// the standard remedy for Newton on an exponential, where an unlimited
    /// step taken from the steep region lands somewhere it takes many
    /// iterations to climb back from.
    /// [design parameter] 2 thermal voltages.
    static constexpr double kNewtonStepThermal = 2.0;

    /// Residual tolerance the fixed iteration count must clear across the whole
    /// parameter sweep. Verified rather than assumed.
    /// [design parameter] default 1e-6, range 1e-8 .. 1e-4 calibration units.
    static constexpr double kResidualTolerance = 1e-6;

    /// How far a full-scale input drives the first stage, measured in
    /// RAIL-TO-RAIL SWINGS of that stage rather than in volts.
    ///
    /// This has to be relative, not absolute, and getting it wrong is silent: an
    /// absolute scale of a few hundred millivolts slams a stage whose whole
    /// input window is tens of millivolts, so every input level clips
    /// identically and the source-impedance cleanup has nothing to show.
    /// Expressed as a multiple of the stage's own swing it tracks the device row
    /// and the collector load automatically — series law 7.
    ///
    /// The swing it is measured against is the NOMINAL one, taken at the
    /// unstarved operating point, because the input network is a fixed property
    /// of the circuit. Referring it to the starved operating point instead would
    /// turn the drive up by exactly as much as starvation turned the stage's
    /// gain down, and bias starvation would be close to inaudible.
    ///
    /// The linear window is half a swing either side of the quiescent point, so
    /// the number to compare against is 0.5. At the default, a full-scale input
    /// at a low source impedance lands past that before the feedback loop
    /// amplifies it further — it clips hard — while the same input through the
    /// ~27 % divider of a rolled-back guitar volume stays inside it. That
    /// difference IS the cleanup the circuit is famous for, and Acceptance Test
    /// 2 measures precisely the gap.
    ///
    /// At the default, a full-scale input through a 10 kΩ source arrives as 0.35
    /// swings, which the feedback loop's ~2.8x closed-loop gain turns into about
    /// two full windows — hard clipping — while the same input through 220 kΩ
    /// arrives as 0.09 and stays under a quarter of a window.
    /// [design parameter] default 0.4, range 0.2 .. 3.0 rail-to-rail swings.
    static constexpr double kInputDriveRails = 0.4;

    /// Thermal-drift depth, in octaves of saturation current. Germanium's
    /// reverse saturation current is far more temperature-sensitive than
    /// silicon's, which is why germanium units are famous for changing
    /// character with the weather.
    /// [design parameter] germanium 0.6 (range 0.1 .. 1.5), silicon 0.05
    /// (range 0 .. 0.2).
    static constexpr double kGermaniumDriftOctaves = 0.6;
    static constexpr double kSiliconDriftOctaves = 0.05;

    /// DC-blocker corner, in Hz. The starved operating point introduces a real
    /// DC step, so this has to actually remove it while staying below the
    /// lowest musical fundamental.
    /// [design parameter] default 12 Hz, range 2 .. 30 Hz.
    static constexpr double kDcBlockerCornerHz = 12.0;

    /// Oversampling factor. The junction law is a hard exponential and the loop
    /// runs near its gain ceiling, so harmonic content extends well past
    /// Nyquist at the base rate. ADAA is not used: its published formulations
    /// target MEMORYLESS nonlinearities, and this is a stateful coupled solve
    /// with feedback whose loop has no closed-form antiderivative.
    /// [design parameter] default 4, range 2 .. 8.
    static constexpr int kOversampleFactor = 4;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    FuzzPairT() { update(); }

    /// Builds the oversampler's filter state. May allocate.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        using Os = OversamplerT<SampleType>;
        oversampler_.set_kind(Os::Kind::linear_phase_fir);
        oversampler_.set_quality(Os::Quality::standard);
        oversampler_.set_factor(Os::Factor::x4);
        oversampler_.set_sample_rate(static_cast<SampleType>(sample_rate_));
        latency_ = oversampler_.latency_samples();

        // The blocker runs at the OVERSAMPLED rate, so its pole is set for
        // that rate: a corner low enough to pass the bass but high enough to
        // clear the DC step a starved operating point introduces.
        blocker_.set_pole(static_cast<SampleType>(
            1.0 - kDcBlockerCornerHz * 6.283185307179586 /
                      (sample_rate_ * kOversampleFactor)));
        drift_.prepare(sample_rate_ * kOversampleFactor);
        update();
        reset();
    }

    void reset() {
        collector1_ = quiescent1_;
        collector2_ = quiescent2_;
        blocker_.reset();
        drift_.reset();
        oversampler_.reset();
        worst_residual_ = 0.0;
    }

    // ── Controls ──────────────────────────────────────────────────────────

    void set_device(Device device) {
        device_ = device;
        update();
    }

    Device device() const { return device_; }

    /// Feedback-network engagement, 0..1. At 1 the loop gain approaches the
    /// tested ceiling.
    void set_fuzz(double amount01) {
        fuzz_ = std::clamp(amount01, 0.0, 1.0);
        update();
    }

    /// 0 = healthy supply, 1 = fully starved. Lowers the quiescent bias toward
    /// cutoff, which is what makes the stage gate rather than go quiet.
    void set_bias_starve(double starve01) {
        starve_ = std::clamp(starve01, 0.0, 1.0);
        update();
    }

    /// The Thévenin source impedance at the input jack: pickup, cable and — the
    /// part that matters — whatever the guitar's volume pot contributes as it is
    /// rolled back. ONE ratio with TWO consequences: it attenuates the signal
    /// AND lightens Q1's input loading, which lowers the loop gain. Both push
    /// toward cleaner, which is why this circuit family cleans up so much more
    /// dramatically than a plain gain stage.
    void set_source_impedance_kohm(double kohm) {
        source_kohm_ = std::clamp(kohm, 0.1, 1000.0);
        update();
    }

    void set_output_level_db(double db) {
        output_gain_ = units::db_to_linear(std::clamp(db, -24.0, 24.0));
    }

    void set_mix(double mix01) { mix_ = std::clamp(mix01, 0.0, 1.0); }

    /// Test-only escape hatch: runs the nonlinearity at the base rate so the
    /// aliasing the oversampler removes can be measured.
    void set_oversampling_enabled(bool on) { oversample_ = on; }

    void set_seed(std::uint32_t seed) { drift_.set_seed(seed); }

    /// Enables the seeded thermal-drift channel. Off by default so a render is
    /// exactly repeatable without relying on the seed discipline.
    void set_drift_enabled(bool on) { drift_enabled_ = on; }

    // ── Observables (for metering and for tests) ──────────────────────────

    /// The divider ratio the source impedance produces. Both the attenuation
    /// and the loop-gain loading factor — one number, deliberately.
    double loading_factor() const { return loading_; }

    /// The STARVED stage's quiescent junction voltage: the conduction knee at
    /// whatever current is still available to it. Unstarved this is the §3
    /// worked example's value — ~363 mV germanium, ~693 mV silicon — because
    /// unstarved is exactly the nominal current. The base sits one emitter drop
    /// above it, see `base_bias_voltage()`.
    ///
    /// This reports Stage 2 because Stage 2 is the one the starve control moves;
    /// Stage 1 stays at the nominal knee by construction.
    double bias_voltage() const { return knee2_; }

    /// The starved stage's quiescent BASE voltage: its junction knee plus the
    /// drop its quiescent current develops across the emitter degeneration.
    double base_bias_voltage() const { return base_bias2_; }

    /// Where the starved stage's collector rests. Unstarved it is mid-rail;
    /// starving walks it toward the supply rail, and the headroom asymmetry that
    /// creates is what folds the waveform into even harmonics.
    double quiescent_collector() const { return quiescent2_; }

    /// One stage's small-signal gain `R_c·gm_eff` at the nominal operating
    /// point. A design consequence of the collector load and the emitter
    /// degeneration together, not of the bias point alone.
    double stage_gain() const { return stage_gain_; }

    /// Volts of base drive a full-scale input produces, derived from the
    /// nominal stage's own rail-to-rail swing.
    double input_scale_volts() const { return input_scale_; }

    /// Available collector current after starvation.
    double available_current() const { return available_current_; }

    /// The linearised small-signal loop gain at the CURRENT operating point.
    /// This is both the stability margin and the solver's contraction factor.
    /// The linearised small-signal loop gain at the DC operating point — the
    /// quantity `kLoopGainCeiling` bounds, and the one the parameter sweep
    /// asserts.
    ///
    /// It is `β · g_couple² · (R_c·gm_eff1) · (R_c·gm_eff2) · loading`, where
    /// `gm_eff = gm/(1 + gm·R_e)` is the DEGENERATED transconductance at each
    /// stage's own operating current. The two differ under starvation, which is
    /// why the loop gain falls only in proportion to the surviving stage rather
    /// than as its square — the feedback path stays alive far enough into
    /// starvation to amplify the operating point's asymmetry, which is the
    /// stated mechanism for the octave-up fold.
    ///
    /// Two collector loads appear because the loop passes through both stages'
    /// collector nodes; the specification's own formula omits them, which is
    /// what leaves it unbounded (it evaluates to ~560 at the defaults). Loading
    /// multiplies it, so a higher source impedance LOWERS the loop gain — the
    /// behaviour the spec's acceptance test 2 demands, though its §5.2 prose
    /// says the opposite. See adjudication A-14.
    ///
    /// This is not a separate formula bolted alongside the solve: it is exactly
    /// the off-diagonal product over the diagonal product of the Jacobian in
    /// `solve_stage`, so the number reported here IS the solver's contraction
    /// factor rather than an estimate of it.
    double loop_gain() const {
        const double stage1 = collector_load_ * (kNominalCollectorCurrent /
                                                 (ideality_ * kThermalVoltage)) /
                              degeneration1_;
        const double stage2 =
            collector_load_ * (available_current_ / (ideality_ * kThermalVoltage)) / degeneration2_;
        return feedback_conductance_ * feedback_norm_ * kCouplingGain * kCouplingGain * stage1 *
               stage2 * loading_;
    }

    /// The largest solver residual seen since `reset()` — so the fixed
    /// iteration count's adequacy is verified rather than assumed.
    double worst_residual() const { return worst_residual_; }

    /// Reported exactly, never estimated (series law 5). A linear-phase FIR
    /// pair cannot be zero-latency, so this is not claimed to be.
    int latency_samples() const noexcept { return oversample_ ? latency_ : 0; }

    // ── Processing ────────────────────────────────────────────────────────

    SampleType process(SampleType input) {
        // Every stage below is recursive. Treat a non-finite sample as a
        // recovery boundary before it reaches either the oversampler or the
        // coupled operating-point state.
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const double dry = static_cast<double>(input);
        const auto stage = [this](SampleType x) {
            return static_cast<SampleType>(solve_stage(static_cast<double>(x)));
        };
        const double wet = oversample_ ? static_cast<double>(oversampler_.process(input, stage))
                                       : static_cast<double>(stage(input));
        const double output = (dry + mix_ * (wet - dry)) * output_gain_;
        // Also recover if an already-poisoned composed primitive or state ever
        // reaches this boundary. `reset()` is RT-safe and clears every recursive
        // state owned by the pair.
        if (!std::isfinite(output)) {
            reset();
            return SampleType{0};
        }
        return static_cast<SampleType>(snap_to_zero(output));
    }

    void process_block(const SampleType* in, SampleType* out, int n) {
        for (int i = 0; i < n; ++i) out[i] = process(in[i]);
    }

private:
    /// `i(v_be)` — the Ebers-Moll forward-active term, in calibration units.
    ///
    /// A transistor's base-emitter junction is a single forward junction, so
    /// the shared network is configured with its reverse leg removed
    /// (`leg_b = 0`) and this is exactly `Is·(exp(v/(n·V_T)) − 1)`.
    double junction_current(double v_be) const { return network_.current(v_be); }

    /// `di/dv_be` — the transconductance the loop-gain formula needs.
    double transconductance(double v_be) const { return network_.conductance(v_be); }

    /// One stage's collector voltage on the normalised rail. This is what
    /// bounds the circuit: without it the junction current is unbounded and the
    /// feedback loop has no solution.
    double collector(double current) const {
        return std::clamp(1.0 - collector_load_ * current, 0.0, 1.0);
    }

    /// One oversampled sample through the coupled pair.
    ///
    /// A true 2×2 Newton solve, not the fixed-point iteration the equations
    /// invite. Fixed-point contracts at the loop gain itself, so near the
    /// ceiling — where this circuit is SUPPOSED to run — it converges at 0.94
    /// per iteration and six of them reduce the residual by only a third. The
    /// specification asks for a residual under 1e-6 after six iterations, which
    /// no fixed-point scheme can deliver at a contraction factor of 0.94; that
    /// slowness is the mathematical shadow of running near instability, not a
    /// tuning problem. Newton converges quadratically once it is in the local
    /// basin; the fixed trust-region ladder below gets it there without assuming
    /// the collector clamps remain on the same active set.
    ///
    /// ## The unknowns are the JUNCTION voltages, not the base voltages
    ///
    /// With emitter degeneration a stage's own equation
    /// `i = Is·(exp((v_b − R_e·i)/θ) − 1)` is implicit in `i` — which would put
    /// a nested solve inside every iteration of the outer one. Solving for
    /// `v_be` instead removes it: the base node is then the EXPLICIT function
    /// `v_b = v_be + R_e·i(v_be)`, and the degeneration appears as a term added
    /// to the Jacobian's diagonal rather than as an inner loop. Same circuit,
    /// same fixed cost per sample, no nesting.
    ///
    /// The Jacobian is closed-form. With `v_c = 1 − R_c·i(v)`, so
    /// `dv_c/dv = −R_c·gm(v)`:
    ///
    /// ```
    /// ∂F1/∂v1 = 1 + R_e·gm(v1)          ∂F1/∂v2 = +β·loading·g_couple·R_c·gm(v2)
    /// ∂F2/∂v1 = +g_couple·R_c·gm(v1)    ∂F2/∂v2 = 1 + R_e·gm(v2)
    /// det / (∂F1/∂v1 · ∂F2/∂v2) = 1 − loop_gain
    /// ```
    ///
    /// The diagonal terms carry the degeneration, which is where
    /// `gm_eff = gm/(1 + gm·R_e)` comes from: dividing the off-diagonal product
    /// by them turns each `gm` into a `gm_eff`, and the result is exactly
    /// `loop_gain()`. So the determinant is one minus the loop gain up to that
    /// positive scale — which is why the stability ceiling and the solve's
    /// conditioning are the same quantity, and why bounding one bounds the
    /// other. The degeneration also damps every Newton step by the same factor
    /// where the exponential is steepest, which is what lets a fixed iteration
    /// count clear the residual tolerance at full-scale drive.
    double solve_stage(double input) {
        // Thermal drift rides on the saturation current, which is the
        // temperature-sensitive quantity — germanium's far more so than
        // silicon's.
        double saturation = saturation_;
        if (drift_enabled_) {
            const double octaves = device_ == Device::germanium ? kGermaniumDriftOctaves
                                                                : kSiliconDriftOctaves;
            saturation *= std::pow(2.0, octaves * (static_cast<double>(drift_.next()) - 1.0));
        }
        const double scale = saturation / saturation_;

        const double drive = input * input_scale_ * loading_;
        double residual = 0.0;

        // The feedback path carries the source-impedance loading for the same
        // reason the input does: it is the same node. One ratio, two
        // consequences — and because it multiplies the loop here, the ceiling
        // `loop_gain()` reports is the one the solver actually runs at.
        const double beta = feedback_conductance_ * feedback_norm_ * loading_;

        // Warm start. Both stages' base voltages are explicit given the PREVIOUS
        // sample's collectors, so the per-stage equation `v + R_e·i(v) = base`
        // can be pre-solved approximately before the coupled iteration begins.
        // Without it the solve starts wherever the last sample left it, which on
        // a transient is far enough away that the fixed iteration count does not
        // recover: a junction Newton descends by only about one thermal voltage
        // per step while it is deep in conduction, so an initial guess 400 mV
        // high needs a dozen iterations to walk back.
        //
        // TWO estimates are taken and the smaller kept, because each is a bound
        // that is tight in the regime the other is loose in, and both sit ABOVE
        // the root (the base-voltage function is convex, so Newton from above
        // descends monotonically — starting below it can overshoot into the
        // steep region instead):
        //
        //   * the linearised inverse — divide the base excursion by `1 + R_e·gm`
        //     at the bias point — which is exact for small excursions and
        //     increasingly conservative for large ones;
        //   * the conduction estimate `θ·ln(base/(R_e·Is))`, which assumes the
        //     whole base voltage is dropped across the degeneration. That is the
        //     right asymptote once the stage is driven hard, and it is what
        //     rescues the deeply starved settings: there the bias sits near
        //     cutoff, `1 + R_e·gm` is barely above 1, and the linear estimate
        //     alone lands hundreds of millivolts too high.
        //
        // The estimate is asked for at `R_e·scale` rather than `R_e` so the
        // drift-perturbed saturation current is the one it inverts — the ratio
        // it forms is `base/(R_e·Is·scale)`, which is the quantity that matters.
        const double base1 =
            base_bias1_ + drive + beta * (collector2_ - quiescent2_) * kCouplingGain;
        const double base2 = base_bias2_ + (collector1_ - quiescent1_) * kCouplingGain;
        const double degeneration_scale = kEmitterDegeneration * scale;
        double v1 = std::min(knee1_ + (base1 - base_bias1_) / degeneration1_,
                             network_.conduction_estimate(base1, degeneration_scale));
        double v2 = std::min(knee2_ + (base2 - base_bias2_) / degeneration2_,
                             network_.conduction_estimate(base2, degeneration_scale));

        // Per-iteration step limit, in thermal voltages. The standard remedy
        // for Newton on a junction: unlimited, a step taken where the
        // exponential is steep lands somewhere it can take many iterations to
        // climb back from.
        const double step_limit = kNewtonStepThermal * ideality_ * kThermalVoltage;
        struct ResidualMetrics {
            double infinity;
            double euclidean;
        };
        const auto residual_metrics_at = [&](double candidate1, double candidate2) {
            const double i1 = scale * junction_current(candidate1);
            const double i2 = scale * junction_current(candidate2);
            const double c1 = collector(i1);
            const double c2 = collector(i2);
            const double f1 =
                candidate1 + kEmitterDegeneration * i1 -
                (base_bias1_ + drive + beta * (c2 - quiescent2_) * kCouplingGain);
            const double f2 =
                candidate2 + kEmitterDegeneration * i2 -
                (base_bias2_ + (c1 - quiescent1_) * kCouplingGain);
            return ResidualMetrics{std::max(std::abs(f1), std::abs(f2)), std::hypot(f1, f2)};
        };

        bool converged = false;
        for (int iter = 0; iter < kSolverIterations; ++iter) {
            // Keep the compile-time-fixed loop bound while avoiding redundant
            // exponentials after this sample has already met the tolerance.
            if (converged) continue;
            const double i1 = scale * junction_current(v1);
            const double i2 = scale * junction_current(v2);
            const double c1 = collector(i1);
            const double c2 = collector(i2);

            // Stage 1's base sees the bias, the loaded input, and the feedback
            // from Stage 2's collector DEVIATION — the deviation rather than the
            // absolute voltage because the feedback resistor's DC servo is what
            // holds the quiescent point in the real circuit. The left-hand side
            // is the base node `v_be + R_e·i`, not the junction voltage: the
            // emitter rises under the current its own base asked for, and that
            // is what keeps the stage gain off the bias point.
            const double f1 =
                v1 + kEmitterDegeneration * i1 -
                (base_bias1_ + drive + beta * (c2 - quiescent2_) * kCouplingGain);
            // Direct coupling: Stage 2's base tracks Stage 1's collector. It is
            // taken in natural units, not renormalised, because the degeneration
            // has already put Stage 2's input window on the same scale as Stage
            // 1's collector swing — a full rail out of Stage 1 overdrives Stage
            // 2 by about `g_couple · R_c/R_e`, which is the ~2x a hot input is
            // supposed to feel and the ~0.5x a rolled-back guitar is supposed to
            // stay under.
            const double f2 =
                v2 + kEmitterDegeneration * i2 -
                (base_bias2_ + (c1 - quiescent1_) * kCouplingGain);
            // The CONVERGED residual, not the running maximum: the first
            // iteration's residual is the initial guess's error, which after a
            // large input step is legitimately big and says nothing about
            // whether the solve converged.
            residual = std::max(std::abs(f1), std::abs(f2));
            if (residual < kResidualTolerance) {
                converged = true;
                continue;
            }
            const double gm1 = scale * transconductance(v1);
            const double gm2 = scale * transconductance(v2);
            // The collector clamps flatten the COUPLING derivative to zero where
            // they engage, which is physically right — a saturated stage passes
            // no signal on — and keeps the Jacobian well conditioned there. The
            // diagonal degeneration terms are unaffected: the emitter still
            // rises with the current, clamped collector or not.
            const double slope1 = c1 > 0.0 && c1 < 1.0 ? collector_load_ * gm1 : 0.0;
            const double slope2 = c2 > 0.0 && c2 < 1.0 ? collector_load_ * gm2 : 0.0;
            const double j11 = 1.0 + kEmitterDegeneration * gm1;
            const double j22 = 1.0 + kEmitterDegeneration * gm2;
            const double j12 = beta * kCouplingGain * slope2;
            const double j21 = kCouplingGain * slope1;
            const double det = j11 * j22 - j12 * j21;
            if (std::abs(det) > 1e-12) {
                const double step1 = (j22 * f1 - j12 * f2) / det;
                const double step2 = (j11 * f2 - j21 * f1) / det;

                // Limit the Newton VECTOR with one common scale. Clamping its
                // two components independently changes the direction, which is
                // especially destructive near the intended 0.94 loop-gain
                // ceiling: a coupled correction becomes two unrelated maximum
                // steps and can cross into the exponential's steep flank.
                const double largest_step = std::max(std::abs(step1), std::abs(step2));
                const double newton_scale =
                    std::min(1.0, step_limit / std::max(largest_step, 1e-30));
                const double gradient1 = j11 * f1 + j21 * f2;
                const double gradient2 = j12 * f1 + j22 * f2;
                const double largest_gradient =
                    std::max(std::abs(gradient1), std::abs(gradient2));
                const double gradient_scale =
                    step_limit / std::max(largest_gradient, 1e-30);

                // Fixed-cap backtracking keeps the Newton correction inside
                // the basin where it actually reduces the coupled residual.
                // Near the loop-gain ceiling the full correction can cross a
                // junction knee even though its direction is right; accepting
                // that step sends the next exponential evaluation orders of
                // magnitude away. Six predetermined candidates retain a hard
                // RT bound and avoid data-dependent retry loops.
                double best1 = v1;
                double best2 = v2;
                double best_merit = std::hypot(f1, f2);
                const double full_newton1 = v1 - newton_scale * step1;
                const double full_newton2 = v2 - newton_scale * step2;
                const double full_newton_merit =
                    residual_metrics_at(full_newton1, full_newton2).euclidean;
                if (full_newton_merit < best_merit) {
                    // In Newton's local basin the full step is the quadratic
                    // path. Avoid evaluating safeguards that cannot improve its
                    // convergence order on the ordinary smooth-audio path.
                    best1 = full_newton1;
                    best2 = full_newton2;
                } else {
                    double damping = 1.0;
                    for (int trial = 0; trial < 6; ++trial) {
                        const double newton1 = v1 - damping * newton_scale * step1;
                        const double newton2 = v2 - damping * newton_scale * step2;
                        const double newton_merit =
                            residual_metrics_at(newton1, newton2).euclidean;
                        if (newton_merit < best_merit) {
                            best1 = newton1;
                            best2 = newton2;
                            best_merit = newton_merit;
                        }

                        // At a collector-rail kink the active-set Jacobian can
                        // point across the kink, where its Newton prediction is
                        // no longer valid. The residual gradient remains a
                        // descent direction there, so include the same fixed
                        // ladder along it as a trust-region fallback.
                        const double gradient_candidate1 =
                            v1 - damping * gradient_scale * gradient1;
                        const double gradient_candidate2 =
                            v2 - damping * gradient_scale * gradient2;
                        const double gradient_merit =
                            residual_metrics_at(gradient_candidate1, gradient_candidate2)
                                .euclidean;
                        if (gradient_merit < best_merit) {
                            best1 = gradient_candidate1;
                            best2 = gradient_candidate2;
                            best_merit = gradient_merit;
                        }
                        damping *= 0.5;
                    }
                }
                v1 = best1;
                v2 = best2;
            }
        }

        // Measure the state that is actually committed. The residual sampled
        // at the top of the final iteration describes the penultimate state,
        // before that iteration's Newton correction.
        residual = residual_metrics_at(v1, v2).infinity;

        // The only state carried to the next sample. The junction voltages are
        // NOT kept: the warm start re-derives them from these collectors, so
        // holding them too would be a second copy of the same operating point
        // that could disagree with this one after a parameter change.
        collector1_ = collector(scale * junction_current(v1));
        collector2_ = collector(scale * junction_current(v2));
        worst_residual_ = std::max(worst_residual_, residual);

        // The output is Q2's collector deviation — bounded to the rail by
        // construction, which is also where the hard fuzz clipping comes from.
        return static_cast<double>(
            blocker_.process(static_cast<SampleType>(collector2_ - quiescent2_)));
    }

    void update() {
        if (device_ == Device::germanium) {
            ideality_ = kGermaniumIdeality;
            saturation_ = kGermaniumSaturation;
        } else {
            ideality_ = kSiliconIdeality;
            saturation_ = kSiliconSaturation;
        }
        network_.ideality = ideality_;
        network_.saturation_current = saturation_;
        network_.leg_a = 1.0;
        // A transistor's base-emitter junction conducts one way only.
        network_.leg_b = 0.0;

        available_current_ =
            std::max(kNominalCollectorCurrent * std::pow(1.0 - starve_, kStarveExponent),
                     kMinAvailableCurrent);

        // The collector load puts the UNSTARVED quiescent collector mid-rail.
        collector_load_ = (1.0 - kQuiescentCollector) / kNominalCollectorCurrent;

        // One ratio, two consequences: attenuation and loop-gain loading.
        loading_ = kInputImpedanceKohm / (source_kohm_ + kInputImpedanceKohm);

        // ── The two operating points ──────────────────────────────────────
        //
        // Starvation is applied to STAGE 2 ONLY, because that is what it is: the
        // supply feeding Q2's collector node runs down. Stage 1 keeps its
        // healthy bias throughout.
        //
        // Starving both stages together is the tempting simplification and it
        // quietly removes two of the three behaviours the starve control exists
        // for. With both stages down, the loop gain falls as the SQUARE of the
        // current, so the feedback path is gone long before the operating point
        // is asymmetric enough to fold — yet the feedback path amplifying the
        // asymmetry is precisely the stated mechanism for octave-up. And a
        // uniformly quiet pair is not a gate: what gates is a healthy first
        // stage still swinging its full rail into a second stage that can no
        // longer follow it.
        //
        // The bias of each stage is its own conduction knee at its own current,
        // INVERTED FROM THE SAME LAW the forward path uses rather than written
        // out again — so the documented 363 mV germanium / 693 mV silicon
        // figures are consequences of the shipped `(n, Is)` rows and cannot
        // drift from them. The base sits one emitter drop above that knee.
        knee1_ = network_.knee_voltage(kNominalCollectorCurrent);
        knee2_ = network_.knee_voltage(available_current_);
        base_bias1_ = knee1_ + kEmitterDegeneration * kNominalCollectorCurrent;
        base_bias2_ = knee2_ + kEmitterDegeneration * available_current_;

        const double gm1 = kNominalCollectorCurrent / (ideality_ * kThermalVoltage);
        const double gm2 = available_current_ / (ideality_ * kThermalVoltage);
        degeneration1_ = 1.0 + gm1 * kEmitterDegeneration;
        degeneration2_ = 1.0 + gm2 * kEmitterDegeneration;

        // Where each collector actually rests. Starving raises Stage 2's toward
        // the supply rail, because less current means less drop across the load
        // — that asymmetric headroom is what folds the waveform and produces the
        // octave, so it must not be assumed away.
        //
        // Every coupling in the solve is taken as a deviation from THESE, not
        // from `kQuiescentCollector`. Referring them to the unstarved mid-rail
        // instead injects a DC step of `R_c·(I_nominal − I_available)` — nearly
        // half a rail at full starvation — into the next stage's base, which
        // drives the starved stage hard ON. That is backwards from what
        // starvation means, and it is also what makes the solve intractable
        // there: the junction is pushed hundreds of millivolts past its knee
        // every sample, where a fixed iteration count cannot follow.
        quiescent1_ = kQuiescentCollector;
        quiescent2_ = std::clamp(1.0 - collector_load_ * available_current_, 0.0, 1.0);

        // The stage gain, and with it the input scale, come from STAGE 1 at its
        // nominal operating point — the stage the input actually drives, and one
        // whose bias the starve control does not touch. Referring the drive to
        // the starved stage instead would raise it by exactly the factor
        // starvation lowered that stage's gain, and the starve control would do
        // almost nothing.
        stage_gain_ = collector_load_ * gm1 / degeneration1_;
        const double rail_to_rail = 1.0 / std::max(stage_gain_, 1e-12);
        input_scale_ = kInputDriveRails * rail_to_rail;

        feedback_conductance_ =
            kFeedbackConductanceMin + fuzz_ * (kFeedbackConductanceMax - kFeedbackConductanceMin);

        // The ceiling is enforced STRUCTURALLY rather than hoped for: the
        // feedback path is normalised so that the worst case over the whole
        // parameter space — maximum feedback, healthy bias, zero source
        // impedance — lands exactly on `kLoopGainCeiling`. The specification
        // describes this as "clamping feedback_conductance at the top of its
        // travel such that the measured maximum never reaches 1.0"; deriving
        // the normaliser is that clamp, with the arithmetic done once instead
        // of a magic number chosen by trial.
        //
        // The worst case uses the SMALLER ideality factor, because a smaller
        // one means a steeper exponential and therefore more transconductance
        // at the same current — silicon is the binding device, not germanium.
        // Degeneration narrows that margin (it divides out most of a `gm`
        // difference) without reversing it, so the binding device is unchanged.
        constexpr double worst_ideality = std::min(kGermaniumIdeality, kSiliconIdeality);
        const double worst_gm = kNominalCollectorCurrent / (worst_ideality * kThermalVoltage);
        const double worst_gm_eff = worst_gm / (1.0 + worst_gm * kEmitterDegeneration);
        const double worst_stage = collector_load_ * worst_gm_eff;
        const double worst_open_loop =
            kCouplingGain * kCouplingGain * worst_stage * worst_stage;
        feedback_norm_ =
            kLoopGainCeiling / std::max(worst_open_loop * kFeedbackConductanceMax, 1e-12);
    }

    Device device_ = Device::germanium;
    double sample_rate_ = 44100.0;
    double fuzz_ = 0.65;
    double starve_ = 0.0;
    double source_kohm_ = 10.0;
    double mix_ = 1.0;
    double output_gain_ = 1.0;
    bool oversample_ = true;
    bool drift_enabled_ = false;

    junction::JunctionPair network_{};
    double ideality_ = kGermaniumIdeality;
    double saturation_ = kGermaniumSaturation;
    double available_current_ = kNominalCollectorCurrent;
    double knee1_ = 0.0;
    double knee2_ = 0.0;
    double base_bias1_ = 0.0;
    double base_bias2_ = 0.0;
    double degeneration1_ = 1.0;
    double degeneration2_ = 1.0;
    double stage_gain_ = 1.0;
    double collector_load_ = 0.5;
    double loading_ = 1.0;
    double input_scale_ = 1.0;
    double feedback_conductance_ = 0.0;
    double feedback_norm_ = 1.0;
    int latency_ = 0;

    double quiescent1_ = kQuiescentCollector;
    double quiescent2_ = kQuiescentCollector;
    double collector1_ = kQuiescentCollector;
    double collector2_ = kQuiescentCollector;
    double worst_residual_ = 0.0;

    DcBlocker<SampleType> blocker_{};
    DriftT<double> drift_{};
    OversamplerT<SampleType> oversampler_{};
};

using FuzzPair = FuzzPairT<float>;
using FuzzPair64 = FuzzPairT<double>;

}  // namespace pulp::signal
