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
///   distortion does.** Raising the source impedance both attenuates the signal
///   AND lightens the loading on Q1's base, which lowers the loop gain. Two
///   effects, one ratio, both pushing the same direction. A model that
///   implements only the attenuation reproduces half the behaviour — see
///   `set_source_impedance_kohm`.
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
/// Each stage is a common-emitter amplifier in normalised units, with the
/// operating point at 1.0 calibration unit of collector current:
///
/// ```
/// i(v_be) = Is·(exp(v_be/(n·V_T)) − 1)          // Ebers-Moll, forward-active term
/// v_c     = clamp(1 − R_c·i, 0, 1)              // collector node, normalised rail
/// ```
///
/// The collector load is what bounds the circuit. Without it the junction
/// current grows without limit — at the germanium defaults a 0.9 V drive gives
/// `i ≈ 7.2e7` — and a feedback path fed by an unbounded current has no
/// solution at all. `R_c` is set so the quiescent collector sits mid-rail,
/// which is also where the clipping headroom is symmetric.
///
/// The stages couple through collector-voltage DEVIATION from quiescent, which
/// is what the feedback resistor's DC servo achieves in the real circuit while
/// keeping the interstage coupling direct.
///
/// ## Why six fixed iterations are enough
///
/// The loop is solved by fixed-point iteration, and its convergence condition
/// is exactly its stability condition: the iteration contracts when the
/// linearised loop gain `|A·β| < 1`, which is precisely what
/// `kLoopGainCeiling` enforces. So the bound that keeps the circuit from
/// oscillating is the same bound that keeps the solver converging — the fixed
/// iteration count is safe by construction rather than by luck, and the
/// parameter sweep that asserts the gain bound also asserts convergence.
///
/// The loop is **explicitly bounded rather than unity-compensated** (series law
/// 1). Compensating it to unity would remove the module's reason for existing:
/// this circuit is supposed to run near the edge.
///
/// ## Bias starvation
///
/// Starvation lowers the available collector current, and therefore the
/// QUIESCENT BIAS, rather than clamping the current mid-solve. That distinction
/// decides whether the module gates or merely goes quiet: with the bias lowered
/// the stage sits near cutoff, so small signals do not turn it on and loud
/// transients do — which is gating that tracks per-note dynamics. Clamping the
/// current instead pins the collector at a constant and produces silence.
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
    /// [design parameter] default 6, range 3 .. 10.
    static constexpr int kSolverIterations = 8;

    /// Per-iteration Newton step limit, in thermal voltages. Junction-limiting:
    /// the standard remedy for Newton on an exponential, where an unlimited
    /// step taken from the steep region lands somewhere it takes many
    /// iterations to climb back from.
    /// [design parameter] default 8, range 3 .. 20.
    static constexpr double kNewtonStepThermal = 20.0;

    /// Residual tolerance the fixed iteration count must clear across the whole
    /// parameter sweep. Verified rather than assumed.
    /// [design parameter] default 1e-6, range 1e-8 .. 1e-4 calibration units.
    static constexpr double kResidualTolerance = 1e-6;

    /// Input scale: what full-scale audio represents in volts of base drive.
    /// Chosen so a full-scale input drives both devices past their knees.
    /// [design parameter] default 0.9 V, range 0.2 .. 2.0 V.
    static constexpr double kInputDriveVolts = 0.9;

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
        v1_ = bias_voltage_;
        v2_ = bias_voltage_;
        collector1_ = kQuiescentCollector;
        collector2_ = kQuiescentCollector;
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

    /// The quiescent base voltage, i.e. the conduction knee at the current
    /// available collector current. Unstarved this is the §3 worked example's
    /// value: ~363 mV germanium, ~693 mV silicon.
    double bias_voltage() const { return bias_voltage_; }

    /// Available collector current after starvation.
    double available_current() const { return available_current_; }

    /// The linearised small-signal loop gain at the CURRENT operating point.
    /// This is both the stability margin and the solver's contraction factor.
    /// The linearised small-signal loop gain at the DC operating point — the
    /// quantity `kLoopGainCeiling` bounds, and the one the parameter sweep
    /// asserts.
    ///
    /// It is `β · g_couple² · R_c² · gm² · loading`, where `gm` is the
    /// transconductance at the quiescent current. Two collector loads appear
    /// because the loop passes through both stages' collector nodes; the
    /// specification's own formula omits them, which is what leaves it
    /// unbounded (it evaluates to ~560 at the defaults). Loading multiplies it,
    /// so a higher source impedance LOWERS the loop gain — the behaviour the
    /// spec's acceptance test 2 demands, though its §5.2 prose says the
    /// opposite. See adjudication A-14.
    double loop_gain() const {
        const double gm = available_current_ / (ideality_ * kThermalVoltage);
        return feedback_conductance_ * feedback_norm_ * kCouplingGain * kCouplingGain *
               collector_load_ * collector_load_ * gm * gm * loading_;
    }

    /// The largest solver residual seen since `reset()` — so the fixed
    /// iteration count's adequacy is verified rather than assumed.
    double worst_residual() const { return worst_residual_; }

    /// Reported exactly, never estimated (series law 5). A linear-phase FIR
    /// pair cannot be zero-latency, so this is not claimed to be.
    int latency_samples() const noexcept { return oversample_ ? latency_ : 0; }

    // ── Processing ────────────────────────────────────────────────────────

    SampleType process(SampleType input) {
        const double dry = static_cast<double>(input);
        const auto stage = [this](SampleType x) {
            return static_cast<SampleType>(solve_stage(static_cast<double>(x)));
        };
        const double wet = oversample_ ? static_cast<double>(oversampler_.process(input, stage))
                                       : static_cast<double>(stage(input));
        return static_cast<SampleType>(snap_to_zero((dry + mix_ * (wet - dry)) * output_gain_));
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
    /// tuning problem. Newton converges quadratically regardless.
    ///
    /// The Jacobian is closed-form. With `v_c = 1 − R_c·i(v)`, so
    /// `dv_c/dv = −R_c·gm(v)`:
    ///
    /// ```
    /// ∂F1/∂v1 = 1                     ∂F1/∂v2 = +β·g_couple·R_c·gm(v2)
    /// ∂F2/∂v1 = +g_couple·R_c·gm(v1)  ∂F2/∂v2 = 1
    /// det     = 1 − loop_gain
    /// ```
    ///
    /// The determinant IS one minus the loop gain — which is why the stability
    /// ceiling and the solve's conditioning are the same quantity, and why
    /// bounding one bounds the other.
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

        const double drive = input * kInputDriveVolts * loading_;
        double v1 = v1_;
        double v2 = v2_;
        double residual = 0.0;

        const double beta = feedback_conductance_ * feedback_norm_;

        // Warm start. Stage 1's equation is very nearly explicit — its base
        // voltage is the bias plus the drive plus the feedback from the
        // PREVIOUS collector — so evaluating it directly puts the iteration
        // within one Newton step of the answer even after a full-scale jump.
        // Without it the solve starts wherever the last sample left it, which
        // on a transient is far enough away that a steep exponential's first
        // Newton step overshoots and six iterations do not recover.
        v1 = bias_voltage_ + drive + beta * (collector2_ - kQuiescentCollector) * kCouplingGain;

        // Per-iteration step limit, in thermal voltages. The standard remedy
        // for Newton on a junction: unlimited, a step taken where the
        // exponential is steep lands somewhere it can take many iterations to
        // climb back from.
        const double step_limit = kNewtonStepThermal * ideality_ * kThermalVoltage;

        for (int iter = 0; iter < kSolverIterations; ++iter) {
            const double i1 = scale * junction_current(v1);
            const double i2 = scale * junction_current(v2);
            const double c1 = collector(i1);
            const double c2 = collector(i2);

            // Stage 1's base sees the bias, the loaded input, and the feedback
            // from Stage 2's collector DEVIATION — the deviation rather than the
            // absolute voltage because the feedback resistor's DC servo is what
            // holds the quiescent point in the real circuit.
            const double f1 =
                v1 - (bias_voltage_ + drive + beta * (c2 - kQuiescentCollector) * kCouplingGain);
            // Direct coupling: Stage 2's base tracks Stage 1's collector.
            const double f2 =
                v2 - (bias_voltage_ + (c1 - kQuiescentCollector) * kCouplingGain);
            // The CONVERGED residual, not the running maximum: the first
            // iteration's residual is the initial guess's error, which after a
            // large input step is legitimately big and says nothing about
            // whether the solve converged.
            residual = std::max(std::abs(f1), std::abs(f2));
            if (residual < kResidualTolerance) break;

            // The collector clamps flatten the derivative to zero where they
            // engage, which is physically right — a saturated stage has no gain
            // — and keeps the Jacobian well conditioned there.
            const double slope1 = c1 > 0.0 && c1 < 1.0 ? collector_load_ * scale * transconductance(v1) : 0.0;
            const double slope2 = c2 > 0.0 && c2 < 1.0 ? collector_load_ * scale * transconductance(v2) : 0.0;
            const double j12 = beta * kCouplingGain * slope2;
            const double j21 = kCouplingGain * slope1;
            const double det = 1.0 - j12 * j21;
            if (!(std::abs(det) > 1e-12)) break;

            v1 -= std::clamp((f1 - j12 * f2) / det, -step_limit, step_limit);
            v2 -= std::clamp((f2 - j21 * f1) / det, -step_limit, step_limit);
        }

        collector1_ = collector(scale * junction_current(v1));
        collector2_ = collector(scale * junction_current(v2));

        v1_ = v1;
        v2_ = v2;
        worst_residual_ = std::max(worst_residual_, residual);

        // The output is Q2's collector deviation — bounded to the rail by
        // construction, which is also where the hard fuzz clipping comes from.
        return static_cast<double>(
            blocker_.process(static_cast<SampleType>(collector2_ - kQuiescentCollector)));
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

        // The quiescent bias IS the conduction knee at the available current —
        // the same formula §3's worked example evaluates, so the documented
        // 363 mV / 693 mV fall out of the shipped constants rather than being
        // restated. Starvation lowers it toward cutoff, which is the gating
        // mechanism.
        // The quiescent bias IS the conduction knee at the available current, and
        // it is INVERTED FROM THE SAME LAW the forward path uses rather than
        // written out again — so the documented 363 mV / 693 mV figures are
        // consequences of the shipped (n, Is) rows and cannot drift from them.
        bias_voltage_ = network_.knee_voltage(available_current_);

        // The collector load puts the quiescent collector mid-rail.
        collector_load_ = (1.0 - kQuiescentCollector) / kNominalCollectorCurrent;

        // One ratio, two consequences: attenuation and loop-gain loading.
        loading_ = kInputImpedanceKohm / (source_kohm_ + kInputImpedanceKohm);

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
        constexpr double worst_ideality = std::min(kGermaniumIdeality, kSiliconIdeality);
        const double worst_gm = kNominalCollectorCurrent / (worst_ideality * kThermalVoltage);
        const double worst_open_loop = kCouplingGain * kCouplingGain * collector_load_ *
                                       collector_load_ * worst_gm * worst_gm;
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
    double bias_voltage_ = 0.0;
    double collector_load_ = 0.5;
    double loading_ = 1.0;
    double feedback_conductance_ = 0.0;
    double feedback_norm_ = 1.0;
    int latency_ = 0;

    double v1_ = 0.0;
    double v2_ = 0.0;
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
