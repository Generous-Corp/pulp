#pragma once

/// @file speaker_cabinet.hpp
/// Physics-based loudspeaker, cabinet and microphone emulation: a Thiele-Small
/// electromechanical model realised as a short chain of digital filters, plus
/// voice-coil-inductance treble rolloff, a cone-breakup modal bank, a
/// level-dependent BL(x) excursion compression stage, closed/open-back cabinet
/// colour with baffle step and edge diffraction, and a microphone-position
/// model.
///
/// RT contract: after `prepare(sample_rate)`, no method allocates, locks, or
///   throws. `process` is a fixed straight-line chain: one TPT state-variable
///   tick (HP and LP taps), one gain multiply (g_bl), a control-cadence
///   coefficient recompute every `kControlBlock` samples, then the fixed
///   inductance / breakup / cabinet / mic biquad chain. Excursion state x is
///   read from the previous sample's LP tap — there is no zero-delay algebraic
///   loop. All state is POD; `reset()` zeroes it; a zero-initialised instance
///   is silent-safe. `latency_samples()` is the literal constant 0.
///
/// ## Sibling: this is the PHYSICS path, `zero_latency_convolver.hpp` is the
/// ## SAMPLED path. A caller picks one.
///
/// Both answer "what does this cabinet sound like", and neither subsumes the
/// other:
///
///   * **This module gives you causes that MOVE.** Driver size, box type and
///     volume, mic distance / traverse / angle are live parameters, and the
///     BL(x) stage means the response depends on LEVEL — a cranked cab
///     compresses and its resonance stiffens. A linear convolution cannot
///     produce level dependence at all, and it cannot move the mic: an IR is a
///     frozen snapshot of one driver, one box, one mic, one level.
///   * **`ZeroLatencyConvolverT` gives you the actual measured cabinet.** Every
///     idiosyncrasy of a real box — the exact modal fingerprint, the room it
///     was captured in, the particular mic — is in the file and no parametric
///     model reproduces it. Use it (with `IrNormalizeMode::peak`, per its
///     cabinet cookbook entry) when fidelity to a specific captured rig is the
///     point.
///
/// Nothing is shared between them at the code level and nothing should be: the
/// convolver owns partitioning, IR ingest and its own zero-latency argument,
/// and this module states its own latency (also 0, for a different and much
/// simpler reason — the chain is a cascade of minimum-phase filters with no
/// block buffering anywhere). The two are alternatives, not layers.
///
/// ## What is composed, and the two places composition did not fit
///
/// Composed: `BiquadT` (peaking and shelving sections for the breakup bank,
/// cabinet and mic stages), `TptFilterT` (the first-order rolloffs),
/// `DcBlocker`, `SmoothedValue`, `units.hpp`, `denormal.hpp`.
///
/// **The cone-breakup bank is peaking biquads, not `ModalBankT`.**
/// `modal_bank.hpp` is a bank of STRUCK RESONATORS — coupled-form phasors with
/// a t60 decay, excited by an input and summed through pickup gains. Its
/// transfer function is a resonator, so it can only ADD energy at a mode. The
/// shipped breakup shape needs a −4 dB DIP at mode 3 (the honk-versus-cut
/// interplay that gives a 12" its vocal midrange), which a parallel resonator
/// bank cannot express, and it is parameterised by Q rather than t60. A
/// cascade of four `BiquadT::Type::peaking` sections is the primitive that
/// matches the specification. `ModalBankT`'s second-order-resonator reasoning
/// still applies and is why the modes are two-pole sections at all.
///
/// **The BL(x) stage is not `SaturatorT`.** `saturator.hpp` is a MEMORYLESS
/// waveshaper: its output is a function of the instantaneous signal sample.
/// The excursion compression here is a gain computed from cone DISPLACEMENT —
/// the state-variable filter's lowpass tap, which is a different, heavily
/// low-passed and phase-shifted signal from the pressure it multiplies. That
/// distinction is the physical content of the model: a low note moves the cone
/// and therefore compresses the treble riding on top of it, which is what a
/// driven cab audibly does. A waveshaper on the pressure signal reproduces
/// neither the low-frequency dominance nor the phase relationship, so it would
/// be a different (and wrong) effect wearing the right name. `g_bl` is also
/// EVEN in x — BL(x) falls symmetrically as the coil leaves the gap either way
/// — so the compression is symmetric and `SaturatorT`'s bias construction,
/// which exists to make asymmetric curves DC-free, has nothing to do here.
///
/// **The resonance stage taps HP and LP from one tick.** `SvfT::process()`
/// returns a single selected mode, so getting both taps from it would mean
/// ticking two instances and advancing two copies of the same state. The
/// dual-tap resonator below is the same Zavalishin TPT topology as `SvfT`,
/// differing only in returning both outputs from one tick. A `process_all()`
/// on `SvfT` would make this unnecessary; that file is not this module's.
///
/// ## Anti-aliasing policy (series law 4): base rate, no oversampling
///
/// The only nonlinearity is the excursion modulation, and its modulator is the
/// SVF LOWPASS state — intrinsically band-limited, falling at −12 dB/oct above
/// fc (~214 Hz for the default archetype and box). A slowly varying gain
/// multiplying the through-signal places sidebands within a few hundred Hz of
/// each carrier component, and the voice-coil rolloff (gone by ~5 kHz)
/// attenuates high carriers before their products can fold. `Aliasing floor`
/// measures the result rather than asserting the argument. Consequently there
/// is no oversampler and `latency_samples()` is 0. If a future high-excursion
/// archetype violates that measurement, the fix is to wrap the excursion stage
/// in the house 65-tap Kaiser (beta = 8) half-band pair and report its group
/// delay exactly — not to widen the test.
///
/// ## References
///   * A. N. Thiele, "Loudspeakers in Vented Boxes: Part I", J. Audio Eng.
///     Soc. 19(5):382-392, 1971 — the second-order high-pass driver model.
///   * R. H. Small, "Direct-Radiator Loudspeaker System Analysis", J. Audio
///     Eng. Soc. 20(5):383-395, 1972 — piston-band pressure as a second-order
///     high-pass; the Thiele-Small parameter framework.
///   * R. H. Small, "Closed-Box Loudspeaker Systems - Part I: Analysis",
///     J. Audio Eng. Soc. 20(10):798-808, 1972 — fc = fs*sqrt(1+alpha),
///     Qtc = Qts*sqrt(1+alpha), alpha = Vas/Vb.
///   * W. Klippel, "Tutorial: Loudspeaker Nonlinearities - Causes, Parameters,
///     Symptoms", J. Audio Eng. Soc. 54(10):907-939, 2006 — BL(x) and Cms(x).
///   * W. M. Leach Jr., "Loudspeaker Voice-Coil Inductance Losses", J. Audio
///     Eng. Soc. 50(6):442-450, 2002 — the semi-inductive HF rolloff.
///   * J. R. Wright, "An Empirical Model for Loudspeaker Motor Impedance",
///     J. Audio Eng. Soc. 38(10):749-754, 1990 — motor-impedance model.
///   * H. F. Olson, "Direct Radiator Loudspeaker Enclosures", J. Audio Eng.
///     Soc. 17(1):22-29, 1969 — baffle step and edge-diffraction concepts.
///
/// The citations above ground concepts and topology. The driver archetype table
/// contains engineered `[design parameter]` starting
/// points inside the ranges published guitar/bass driver datasheets occupy —
/// not a transcription of any one product.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <complex>

namespace pulp::signal {

/// Cabinet topology. Sealed traps air behind the cone and stiffens it (the
/// closed-box alpha of Small 1972); open-back lets the rear wave out, giving a
/// near-free-air driver plus dipole cancellation.
enum class SpeakerBoxType { sealed, open_back };

/// One driver's published-form Thiele-Small set plus the two geometry figures
/// the cabinet and breakup stages need.
///
/// `[design parameter]` rows — engineered starting points inside the ranges
/// real guitar/bass driver datasheets occupy, as described in the file doc
/// block. Ranges are given in `kArchetypeRanges` so a caller (or a
/// test) can see the envelope each figure is drawn from.
struct SpeakerDriverArchetype {
    const char* name;
    double fs_hz;           ///< free-air resonance
    double qts;             ///< total Q at fs
    double vas_litres;      ///< equivalent suspension compliance volume
    double f_breakup_hz;    ///< anchor frequency of the dimensionless mode shape
    double baffle_width_m;  ///< sets the baffle-step transition
};

namespace detail {

/// Dual-tap TPT state-variable resonator (Zavalishin topology, identical to
/// `SvfT`) returning the highpass and lowpass outputs from ONE tick.
///
/// The two taps are the whole trick of the Thiele-Small stage: the highpass IS
/// the radiated pressure (pressure is proportional to cone acceleration, so the
/// piston-band response is second-order high-pass), and the lowpass of the same
/// resonator is proportional to cone DISPLACEMENT. The physics hands the
/// excursion signal over for free, which is what the nonlinear stage needs.
///
/// RT contract: `set_coefficients()` costs one tangent; `tick()` and `reset()`
/// are arithmetic only. Nothing allocates.
template <typename SampleType>
class DualTapResonatorT {
public:
    struct Taps {
        SampleType highpass;
        SampleType lowpass;
    };

    void set_coefficients(double cutoff_hz, double q, double sample_rate) {
        constexpr double pi = 3.14159265358979323846;
        const double nyquist_limit = 0.49 * sample_rate;
        const double fc = std::clamp(cutoff_hz, 1.0, nyquist_limit);
        const double safe_q = std::max(q, 1.0e-3);
        const double g = std::tan(pi * fc / sample_rate);
        const double k = 1.0 / safe_q;
        const double a1 = 1.0 / (1.0 + g * (g + k));
        g_ = static_cast<SampleType>(g);
        k_ = static_cast<SampleType>(k);
        a1_ = static_cast<SampleType>(a1);
        a2_ = static_cast<SampleType>(g * a1);
        a3_ = static_cast<SampleType>(g * g * a1);
    }

    Taps tick(SampleType input) {
        const SampleType v3 = input - ic2_;
        const SampleType v1 = a1_ * ic1_ + a2_ * v3;
        const SampleType v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        // Snap both integrator states: the TPT feedback path has no FTZ guard
        // of its own, exactly as SvfT documents.
        ic1_ = snap_to_zero(SampleType{2} * v1 - ic1_);
        ic2_ = snap_to_zero(SampleType{2} * v2 - ic2_);
        return {input - k_ * v1 - v2, v2};
    }

    void reset() {
        ic1_ = SampleType{0};
        ic2_ = SampleType{0};
    }

private:
    SampleType g_ = SampleType{0};
    SampleType k_ = SampleType{1};
    SampleType a1_ = SampleType{1};
    SampleType a2_ = SampleType{0};
    SampleType a3_ = SampleType{0};
    SampleType ic1_ = SampleType{0};
    SampleType ic2_ = SampleType{0};
};

}  // namespace detail

/// Physics-based speaker + cabinet + microphone emulator.
///
/// See the file doc block for the sibling relationship with
/// `ZeroLatencyConvolverT`, the composition notes, and the anti-aliasing
/// policy. Signal chain, in order: DC block, drive, Thiele-Small resonance
/// (HP tap = pressure, LP tap = excursion), BL(x) compression, voice-coil
/// inductance rolloff, cone-breakup modal bank, cabinet (baffle step, edge
/// ripple, open-back dipole), microphone (proximity, air loss, cap-to-cone
/// traverse, off-axis), output trim.
template <typename SampleType = float>
class SpeakerModelT {
public:
    // ── Driver archetypes ────────────────────────────────────────────────────

    static constexpr int kArchetypeCount = 5;

    /// `[design parameter]` table; see `SpeakerDriverArchetype` and the model
    /// scope above. Ranges below each figure are the datasheet envelope the
    /// value is drawn from.
    static const SpeakerDriverArchetype& archetype(int index) {
        static constexpr SpeakerDriverArchetype kTable[kArchetypeCount] = {
            // name              fs     Qts   Vas(L)  f_breakup  baffle W(m)
            {"Brit-12 ceramic",  75.0,  0.50, 200.0,  1900.0,    0.50},
            {"Amer-12 ceramic",  95.0,  0.60, 150.0,  2200.0,    0.50},
            {"Alnico-12",        85.0,  0.45, 170.0,  2000.0,    0.48},
            {"Brit-10",         100.0,  0.55,  90.0,  2600.0,    0.42},
            {"Bass-15",          45.0,  0.35, 300.0,  1200.0,    0.60},
        };
        return kTable[std::clamp(index, 0, kArchetypeCount - 1)];
    }

    // ── Design parameters ────────────────────────────────────────────────────
    // Every value here is ours to tune, declared with default + range.

    /// Speed of sound in air at ~20 C, standard atmospheric value. PUBLISHED
    /// physical constant, not a tunable — it sets the baffle-step and dipole
    /// frequencies from geometry.
    static constexpr double kSpeedOfSoundMs = 343.0;

    /// Sealed-box internal volume. [dp, default 28 L, range 5..150 L]
    static constexpr double kBoxVolumeLDefault = 28.0;
    static constexpr double kBoxVolumeLMin = 5.0;
    static constexpr double kBoxVolumeLMax = 150.0;

    /// Voicing shift of the computed resonance. [dp, default 0, range -12..+12]
    static constexpr double kResonanceTrimSemitonesMin = -12.0;
    static constexpr double kResonanceTrimSemitonesMax = 12.0;

    /// Box Qtc override. [dp, default = computed from archetype + volume,
    /// range 0.4..2.0]
    static constexpr double kQResonanceMin = 0.4;
    static constexpr double kQResonanceMax = 2.0;

    /// Depth of the cone-breakup bank. [dp, default 100 %, range 0..100 %]
    static constexpr double kConeBreakupAmountDefault = 100.0;

    /// Voice-coil-inductance corner. [dp, default 4000 Hz, range 1500..8000 Hz]
    static constexpr double kTrebleRolloffHzDefault = 4000.0;
    static constexpr double kTrebleRolloffHzMin = 1500.0;
    static constexpr double kTrebleRolloffHzMax = 8000.0;

    /// Leach semi-inductance blend and its shelf floor: the fraction of the
    /// un-rolled signal mixed back through a high shelf to give the "leaky",
    /// gentler-than-6-dB/oct HF decay real motors measure.
    /// [dp, default 0.25, range 0..1] and [dp, default -12 dB, range -18..-6]
    static constexpr double kSemiInductanceDefault = 0.25;
    static constexpr double kSemiInductanceMin = 0.0;
    static constexpr double kSemiInductanceMax = 1.0;
    static constexpr double kSemiInductanceShelfDb = -12.0;
    static constexpr double kSemiInductanceShelfDbMin = -18.0;
    static constexpr double kSemiInductanceShelfDbMax = -6.0;

    /// Excursion drive. [dp, default 0 dB, range -12..+24 dB]
    static constexpr double kDriveDbDefault = 0.0;
    static constexpr double kDriveDbMin = -12.0;
    static constexpr double kDriveDbMax = 24.0;

    /// Depth of the BL(x) / Cms(x) nonlinearity. [dp, default 60 %, range 0..100]
    static constexpr double kCompressionAmountDefault = 60.0;

    /// Normalized reference excursion. Signals here are normalized audio, not
    /// metres, so this and `drive_db` together set WHERE compression begins
    /// rather than claiming a physical Xmax. [dp, default 0.5, range 0.1..2.0]
    static constexpr double kExcursionReference = 0.5;
    static constexpr double kExcursionReferenceMin = 0.1;
    static constexpr double kExcursionReferenceMax = 2.0;

    /// BL(x) falloff rate: `g_bl = 1/(1 + beta*x^2)`, `beta = amount * k_bl`.
    /// [dp, default 1.5, range 0..4]
    static constexpr double kBlFalloff = 1.5;
    static constexpr double kBlFalloffMin = 0.0;
    static constexpr double kBlFalloffMax = 4.0;

    /// Cms(x) stiffening rate: `fc' = fc*(1 + gamma*x^2)`,
    /// `gamma = amount * k_cms`. [dp, default 0.35, range 0..1]
    static constexpr double kCmsStiffening = 0.35;
    static constexpr double kCmsStiffeningMin = 0.0;
    static constexpr double kCmsStiffeningMax = 1.0;

    /// Mic distance. [dp, default 3 cm, range 1..60 cm]
    static constexpr double kMicDistanceCmDefault = 3.0;
    static constexpr double kMicDistanceCmMin = 1.0;
    static constexpr double kMicDistanceCmMax = 60.0;

    /// Proximity shelf. The gain law is
    /// `k_prox * (1/distance - 1/d_ref)`, clamped to [0, ceiling] — the 0 dB
    /// floor is STRUCTURAL (the shelf is boost-only by construction), the
    /// ceiling is a design parameter.
    /// [dp: k_prox 12, range 0..24] [dp: d_ref 30 cm, range 20..50]
    /// [dp: ceiling +8 dB, range +6..+12] [dp: corner 250 Hz, range 150..400]
    static constexpr double kProximityGainK = 12.0;
    static constexpr double kProximityGainKMin = 0.0;
    static constexpr double kProximityGainKMax = 24.0;
    static constexpr double kProximityReferenceCm = 30.0;
    static constexpr double kProximityReferenceCmMin = 20.0;
    static constexpr double kProximityReferenceCmMax = 50.0;
    static constexpr double kProximityCeilingDb = 8.0;
    static constexpr double kProximityCeilingDbMin = 6.0;
    static constexpr double kProximityCeilingDbMax = 12.0;
    static constexpr double kProximityCornerHz = 250.0;
    static constexpr double kProximityCornerHzMin = 150.0;
    static constexpr double kProximityCornerHzMax = 400.0;

    /// Air/distance HF loss above a corner, scaling with distance.
    /// [dp: 0.05 dB/cm, range 0.02..0.10] [dp: corner 6 kHz, range 4..8 kHz]
    static constexpr double kAirLossDbPerCm = 0.05;
    static constexpr double kAirLossDbPerCmMin = 0.02;
    static constexpr double kAirLossDbPerCmMax = 0.10;
    static constexpr double kAirLossCornerHz = 6000.0;
    static constexpr double kAirLossCornerHzMin = 4000.0;
    static constexpr double kAirLossCornerHzMax = 8000.0;

    /// Cap-to-cone traverse: a high shelf lerped from bright at the dust cap to
    /// dark at the cone edge. [dp: +3 dB at cap, -8 dB at edge, range +6..-12]
    /// [dp: corner 2.5 kHz, range 1.5..3.5 kHz]
    static constexpr double kPresenceCapDb = 3.0;
    static constexpr double kPresenceEdgeDb = -8.0;
    static constexpr double kPresenceDbMax = 6.0;
    static constexpr double kPresenceDbMin = -12.0;
    static constexpr double kPresenceCornerHz = 2500.0;
    static constexpr double kPresenceCornerHzMin = 1500.0;
    static constexpr double kPresenceCornerHzMax = 3500.0;
    static constexpr double kMicPositionPctDefault = 30.0;

    /// Off-axis rolloff: `fc = f_on * (1 - k_ax * sin(angle))`.
    /// [dp: f_on 8 kHz, range 5..12 kHz] [dp: k_ax 0.7, range 0..0.95]
    static constexpr double kOffAxisOnAxisHz = 8000.0;
    static constexpr double kOffAxisOnAxisHzMin = 5000.0;
    static constexpr double kOffAxisOnAxisHzMax = 12000.0;
    static constexpr double kOffAxisFactor = 0.7;
    static constexpr double kOffAxisFactorMin = 0.0;
    static constexpr double kOffAxisFactorMax = 0.95;
    static constexpr double kMicAxisDegMin = 0.0;
    static constexpr double kMicAxisDegMax = 90.0;

    /// Baffle step: `+6 dB` high shelf at `c / (pi * W)`, depth scaled by
    /// `diffraction_amount`. The +6 dB is the PUBLISHED half-space-to-full-space
    /// transition (Olson 1969), not a tunable.
    static constexpr double kBaffleStepDb = 6.0;

    /// Edge-diffraction ripple: two low-Q sections at `c/(2*d_edge)` and twice
    /// that. [dp: d_edge 0.15 m, range 0.08..0.30]
    /// [dp: (Q 2.0, +2.5 dB) and (Q 2.5, -2.0 dB), Q range 1.5..4, gain +-4 dB]
    static constexpr double kEdgeDistanceM = 0.15;
    static constexpr double kEdgeDistanceMMin = 0.08;
    static constexpr double kEdgeDistanceMMax = 0.30;
    static constexpr double kRipple1Q = 2.0;
    static constexpr double kRipple1Db = 2.5;
    static constexpr double kRipple2Q = 2.5;
    static constexpr double kRipple2Db = -2.0;
    static constexpr double kRippleQMin = 1.5;
    static constexpr double kRippleQMax = 4.0;
    static constexpr double kRippleDbMax = 4.0;
    static constexpr double kDiffractionAmountDefault = 50.0;

    /// Open-back dipole: first-order high-pass at `c / (2*D)` plus a notch.
    /// [dp: D 0.30 m, range 0.15..0.5] [dp: notch ratio 1.8, range 1.4..2.2]
    /// [dp: notch Q 1.5, range 1.0..3.0] [dp: notch -4 dB, range -8..-2]
    static constexpr double kDipolePathM = 0.30;
    static constexpr double kDipolePathMMin = 0.15;
    static constexpr double kDipolePathMMax = 0.50;
    static constexpr double kDipoleNotchRatio = 1.8;
    static constexpr double kDipoleNotchRatioMin = 1.4;
    static constexpr double kDipoleNotchRatioMax = 2.2;
    static constexpr double kDipoleNotchQ = 1.5;
    static constexpr double kDipoleNotchQMin = 1.0;
    static constexpr double kDipoleNotchQMax = 3.0;
    static constexpr double kDipoleNotchDb = -4.0;
    static constexpr double kDipoleNotchDbMin = -8.0;
    static constexpr double kDipoleNotchDbMax = -2.0;

    /// Output make-up. [dp, default 0 dB, range -24..+24 dB]
    static constexpr double kOutputTrimDbMin = -24.0;
    static constexpr double kOutputTrimDbMax = 24.0;

    /// Input DC-blocker corner. [dp, default 5 Hz, range 2..20 Hz]
    ///
    /// This must be set explicitly rather than left at `DcBlocker`'s default
    /// pole of 0.995, which is a ~38 Hz corner at 48 kHz — inside the passband
    /// of every archetype here and directly on top of the Bass-15's 45 Hz
    /// resonance. A DC blocker's job is removing DC, not shaping the low end;
    /// at 5 Hz it is 18 dB below the lowest resonance in the table and the
    /// second-order-high-pass asymptote below fc is the driver's, not the
    /// blocker's.
    static constexpr double kDcBlockerHz = 5.0;
    static constexpr double kDcBlockerHzMin = 2.0;
    static constexpr double kDcBlockerHzMax = 20.0;

    /// Parameter smoothing time. [dp, default 15 ms, range 5..30 ms]
    static constexpr double kSmoothingMs = 15.0;
    static constexpr double kSmoothingMsMin = 5.0;
    static constexpr double kSmoothingMsMax = 30.0;

    /// Coefficient recompute cadence, in samples. [dp, default 32,
    /// range 16..64] Keeps transcendentals out of the hot loop beyond the
    /// resonator's own tick.
    static constexpr int kControlBlock = 32;
    static constexpr int kControlBlockMin = 16;
    static constexpr int kControlBlockMax = 64;

    /// Sum of per-stage peak magnitudes across the whole parameter space, the
    /// bound the Forge registry cites. Asserted by the worst-case-gain test —
    /// a measured invariant, not an estimate.
    static constexpr double kWorstCaseGain = 10.0;

    // ── Cone-breakup mode shape ─────────────────────────────────────────────

    static constexpr int kBreakupModeCount = 4;

    /// One DIMENSIONLESS breakup mode: a ratio to the archetype's anchor
    /// frequency plus a Q and a gain. All archetypes share this one shape and
    /// differ only in the anchor, so a change of driver moves the "cry" without
    /// re-fitting coefficients (series law 7 — never interpolate independently
    /// fitted sets; find the scale-invariant shape).
    struct BreakupMode {
        double ratio;
        double q;
        double gain_db;
    };

    /// `[design parameter]` shape. Ranges: ratio 1.0..4.0, Q 1.5..8.0,
    /// gain +-6 dB. Honest gap: exact modal maps are driver-specific and are
    /// not published as citable tables; these are engineered presence-region
    /// values.
    static const BreakupMode& breakup_mode(int index) {
        static constexpr BreakupMode kShape[kBreakupModeCount] = {
            {1.00, 3.0, 4.0},
            {1.55, 4.0, 3.0},
            {2.20, 5.0, -4.0},
            {3.10, 6.0, 2.5},
        };
        return kShape[std::clamp(index, 0, kBreakupModeCount - 1)];
    }
    static constexpr double kBreakupRatioMin = 1.0;
    static constexpr double kBreakupRatioMax = 4.0;
    static constexpr double kBreakupQMin = 1.5;
    static constexpr double kBreakupQMax = 8.0;
    static constexpr double kBreakupGainDbMax = 6.0;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /// Recompute every corner coefficient for a new sample rate. No allocation.
    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate > 0.0) sample_rate_ = sample_rate;
        const auto smoothing = static_cast<SampleType>(kSmoothingMs / 1000.0);
        const auto fs = static_cast<SampleType>(sample_rate_);
        drive_lin_.set_ramp_time(smoothing, fs);
        out_trim_lin_.set_ramp_time(smoothing, fs);
        treble_hz_.set_ramp_time(smoothing, fs);
        q_smoothed_.set_ramp_time(smoothing, fs);
        presence_db_.set_ramp_time(smoothing, fs);
        proximity_db_.set_ramp_time(smoothing, fs);
        offaxis_hz_.set_ramp_time(smoothing, fs);
        inductance_lp_.prepare(fs);
        offaxis_lp_.prepare(fs);
        dipole_hp_.prepare(fs);
        // One-pole high-pass pole for the requested corner: p = 1 - 2*pi*fc/fs.
        constexpr double two_pi = 6.283185307179586476925286766559;
        dc_.set_pole(static_cast<SampleType>(
            std::clamp(1.0 - two_pi * kDcBlockerHz / sample_rate_, 0.0, 0.99999)));
        drive_lin_.set_immediate(static_cast<SampleType>(units::db_to_linear(drive_db_)));
        out_trim_lin_.set_immediate(static_cast<SampleType>(units::db_to_linear(out_trim_db_)));
        treble_hz_.set_immediate(static_cast<SampleType>(treble_hz_target_));
        q_smoothed_.set_immediate(static_cast<SampleType>(resonance_q()));
        presence_db_.set_immediate(static_cast<SampleType>(presence_shelf_db()));
        proximity_db_.set_immediate(static_cast<SampleType>(proximity_gain_db()));
        offaxis_hz_.set_immediate(static_cast<SampleType>(offaxis_corner_hz()));
        control_counter_ = 0;
        update_coefficients();
        reset();
    }

    // ── Parameters ───────────────────────────────────────────────────────────

    void set_driver_archetype(int index) {
        archetype_index_ = std::clamp(index, 0, kArchetypeCount - 1);
        retarget_smoothed();
    }
    void set_box_type(SpeakerBoxType type) {
        box_type_ = type;
        retarget_smoothed();
    }
    void set_box_volume_l(double litres) {
        if (!std::isfinite(litres)) return;
        box_volume_l_ = std::clamp(litres, kBoxVolumeLMin, kBoxVolumeLMax);
        retarget_smoothed();
    }
    void set_resonance_trim_semitones(double semitones) {
        if (!std::isfinite(semitones)) return;
        resonance_trim_ =
            std::clamp(semitones, kResonanceTrimSemitonesMin, kResonanceTrimSemitonesMax);
    }
    /// Override the computed box Qtc. Pass a non-positive value to return to
    /// the archetype-and-volume computation.
    void set_q_resonance(double q) {
        if (!std::isfinite(q)) return;
        q_override_ = q > 0.0 ? std::clamp(q, kQResonanceMin, kQResonanceMax) : 0.0;
        retarget_smoothed();
    }
    void set_cone_breakup_amount(double percent) {
        if (!std::isfinite(percent)) return;
        breakup_amount_ = std::clamp(percent, 0.0, 100.0) / 100.0;
    }
    void set_treble_rolloff_hz(double hz) {
        if (!std::isfinite(hz)) return;
        treble_hz_target_ = std::clamp(hz, kTrebleRolloffHzMin, kTrebleRolloffHzMax);
        treble_hz_.set_target(static_cast<SampleType>(treble_hz_target_));
    }
    void set_drive_db(double db) {
        if (!std::isfinite(db)) return;
        drive_db_ = std::clamp(db, kDriveDbMin, kDriveDbMax);
        drive_lin_.set_target(static_cast<SampleType>(units::db_to_linear(drive_db_)));
    }
    void set_compression_amount(double percent) {
        if (!std::isfinite(percent)) return;
        compression_amount_ = std::clamp(percent, 0.0, 100.0) / 100.0;
    }
    void set_mic_distance_cm(double cm) {
        if (!std::isfinite(cm)) return;
        mic_distance_cm_ = std::clamp(cm, kMicDistanceCmMin, kMicDistanceCmMax);
        proximity_db_.set_target(static_cast<SampleType>(proximity_gain_db()));
    }
    void set_mic_position_pct(double percent) {
        if (!std::isfinite(percent)) return;
        mic_position_ = std::clamp(percent, 0.0, 100.0) / 100.0;
        presence_db_.set_target(static_cast<SampleType>(presence_shelf_db()));
    }
    void set_mic_axis_deg(double degrees) {
        if (!std::isfinite(degrees)) return;
        mic_axis_deg_ = std::clamp(degrees, kMicAxisDegMin, kMicAxisDegMax);
        offaxis_hz_.set_target(static_cast<SampleType>(offaxis_corner_hz()));
    }
    void set_diffraction_amount(double percent) {
        if (!std::isfinite(percent)) return;
        diffraction_amount_ = std::clamp(percent, 0.0, 100.0) / 100.0;
    }
    void set_output_trim_db(double db) {
        if (!std::isfinite(db)) return;
        out_trim_db_ = std::clamp(db, kOutputTrimDbMin, kOutputTrimDbMax);
        out_trim_lin_.set_target(static_cast<SampleType>(units::db_to_linear(out_trim_db_)));
    }

    // ── Audio ────────────────────────────────────────────────────────────────

    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        if (control_counter_ == 0) update_coefficients();
        control_counter_ = (control_counter_ + 1) % kControlBlock;

        const SampleType drive = drive_lin_.next();
        const SampleType trim = out_trim_lin_.next();

        // Thiele-Small resonance. HP tap is the radiated pressure; LP tap is
        // proportional to cone displacement and feeds the nonlinear stage.
        const auto taps = resonance_.tick(dc_.process(input) * drive);

        // BL(x): even, strictly compressive, exactly unity at rest.
        const SampleType x_norm = taps.lowpass * excursion_scale_;
        const SampleType g_bl = SampleType{1} / (SampleType{1} + beta_ * x_norm * x_norm);
        excursion_ = x_norm;
        // Peak-track within the control block: the Cms stiffening is an
        // envelope effect, and sampling the instantaneous excursion once per
        // block would alias the carrier's own phase into the coefficient.
        const SampleType magnitude = std::abs(x_norm);
        if (magnitude > excursion_peak_) excursion_peak_ = magnitude;

        SampleType s = taps.highpass * g_bl;
        s = inductance(s);
        for (int i = 0; i < kBreakupModeCount; ++i) s = breakup_[i].process(s);
        s = baffle_.process(s);
        s = ripple_[0].process(s);
        s = ripple_[1].process(s);
        if (box_type_ == SpeakerBoxType::open_back) {
            s = dipole_hp_.process_highpass(s);
            s = dipole_notch_.process(s);
        }
        s = proximity_.process(s);
        s = air_loss_.process(s);
        s = presence_.process(s);
        s = offaxis_lp_.process_lowpass(s);
        const SampleType output = s * trim;
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

    void process(const SampleType* in, SampleType* out, int n) {
        for (int i = 0; i < n; ++i) out[i] = process(in[i]);
    }

    /// Zero every filter and excursion state. No allocation. A zero-initialised
    /// instance is equivalent to a reset one.
    void reset() {
        dc_.reset();
        resonance_.reset();
        inductance_lp_.reset();
        semi_shelf_.reset();
        for (auto& mode : breakup_) mode.reset();
        baffle_.reset();
        ripple_[0].reset();
        ripple_[1].reset();
        dipole_hp_.reset();
        dipole_notch_.reset();
        proximity_.reset();
        air_loss_.reset();
        presence_.reset();
        offaxis_lp_.reset();
        excursion_ = SampleType{0};
        excursion_peak_ = SampleType{0};
        fc_dynamic_ = resonance_fc_hz();
        control_counter_ = 0;
    }

    /// The chain is a cascade of minimum-phase filters with no block buffering
    /// and no oversampler, so nothing is held back.
    int latency_samples() const { return 0; }

    /// Sum of per-stage peak magnitudes across the parameter space. Asserted,
    /// not estimated — see the worst-case-gain test.
    double worst_case_gain() const { return kWorstCaseGain; }

    // ── Derived physics (tests compute their expected values from these) ─────

    /// Compliance ratio alpha = Vas / Vb. Zero in open-back: the rear wave
    /// escapes, so there is no trapped-air stiffness and the driver runs near
    /// free air. That is the real physical difference between the two cabinet
    /// types, exposed as one enum.
    double compliance_ratio() const {
        if (box_type_ == SpeakerBoxType::open_back) return 0.0;
        return archetype(archetype_index_).vas_litres / box_volume_l_;
    }

    /// Box resonance fc = fs * sqrt(1 + alpha), shifted by the voicing trim.
    double resonance_fc_hz() const {
        const double base = archetype(archetype_index_).fs_hz *
                            std::sqrt(1.0 + compliance_ratio());
        return base * std::pow(2.0, resonance_trim_ / 12.0);
    }

    /// Box Qtc = Qts * sqrt(1 + alpha), unless overridden.
    double resonance_q() const {
        if (q_override_ > 0.0) return q_override_;
        return archetype(archetype_index_).qts * std::sqrt(1.0 + compliance_ratio());
    }

    /// Peak magnitude of the second-order high-pass at the resonance, in dB:
    /// `Q / sqrt(1 - 1/(4 Q^2))`. Only defined for Q > 1/sqrt(2); below that
    /// the response is monotone and there is no peak.
    double resonance_peak_db() const {
        const double q = resonance_q();
        const double inner = 1.0 - 1.0 / (4.0 * q * q);
        if (inner <= 0.0) return 0.0;
        return units::linear_to_db(q / std::sqrt(inner));
    }

    /// Frequency of that peak: `fc / sqrt(1 - 1/(2 Q^2))`. NOT fc — a
    /// second-order high-pass peaks ABOVE its corner, unlike a bandpass.
    double resonance_peak_hz() const {
        const double q = resonance_q();
        const double inner = 1.0 - 1.0 / (2.0 * q * q);
        if (inner <= 0.0) return resonance_fc_hz();
        return resonance_fc_hz() / std::sqrt(inner);
    }

    /// Baffle-step transition c / (pi * W).
    double baffle_step_hz() const {
        return kSpeedOfSoundMs / (3.14159265358979323846 * archetype(archetype_index_).baffle_width_m);
    }

    /// First edge-diffraction ripple c / (2 * d_edge).
    double ripple_hz() const { return kSpeedOfSoundMs / (2.0 * kEdgeDistanceM); }

    /// Open-back dipole corner c / (2 * D).
    double dipole_hz() const { return kSpeedOfSoundMs / (2.0 * kDipolePathM); }

    /// Centre of breakup mode `index`, anchored on the archetype.
    double breakup_mode_hz(int index) const {
        return archetype(archetype_index_).f_breakup_hz * breakup_mode(index).ratio;
    }

    /// Off-axis corner `f_on * (1 - k_ax * sin(angle))`.
    double offaxis_corner_hz() const {
        constexpr double deg_to_rad = 3.14159265358979323846 / 180.0;
        const double reduction = kOffAxisFactor * std::sin(mic_axis_deg_ * deg_to_rad);
        return kOffAxisOnAxisHz * std::max(0.05, 1.0 - reduction);
    }

    /// Proximity shelf gain, boost-only by construction and clamped at the
    /// ceiling: `k_prox * (1/d - 1/d_ref)` in dB.
    double proximity_gain_db() const {
        const double raw = kProximityGainK * (1.0 / mic_distance_cm_ - 1.0 / kProximityReferenceCm);
        return std::clamp(raw, 0.0, kProximityCeilingDb);
    }

    /// Cap-to-cone traverse shelf, lerped bright-to-dark.
    double presence_shelf_db() const {
        return kPresenceCapDb + mic_position_ * (kPresenceEdgeDb - kPresenceCapDb);
    }

    /// Air/distance HF trim, in dB (negative), scaling with distance.
    double air_loss_db() const { return -kAirLossDbPerCm * mic_distance_cm_; }

    /// Exact magnitude of the voice-coil inductance stage ALONE, in dB, from
    /// the coefficients currently in force.
    ///
    /// The stage is a parallel blend of a first-order lowpass and a high shelf,
    /// so it cannot be read off the chain's response: every other stage sits on
    /// top of it, and the off-axis lowpass in particular is a second HF rolloff
    /// that is always present. Exposing the stage's own response lets its
    /// corner and slope be asserted directly — and lets a UI draw the rolloff
    /// the "treble" control is actually applying.
    double inductance_magnitude_db(double hz) const {
        constexpr double pi = 3.14159265358979323846;
        const double w = 2.0 * pi * hz / sample_rate_;
        const std::complex<double> z_inv = std::polar(1.0, -w);

        // TPT one-pole lowpass: H(z) = g(1 + z^-1) / (1 - (1 - 2g) z^-1),
        // with the same bilinear g the filter itself computes from its corner.
        const double fc = static_cast<double>(inductance_lp_.cutoff());
        const double wa = 2.0 * sample_rate_ * std::tan(pi * fc / sample_rate_);
        const double g = wa / (2.0 * sample_rate_ + wa);
        const std::complex<double> lp =
            g * (1.0 + z_inv) / (1.0 - (1.0 - 2.0 * g) * z_inv);

        // The shelf's live coefficients, evaluated as H(e^jw).
        const auto c = semi_shelf_.coefficients();
        const std::complex<double> z2 = z_inv * z_inv;
        const std::complex<double> shelf =
            (static_cast<double>(c.b0) + static_cast<double>(c.b1) * z_inv +
             static_cast<double>(c.b2) * z2) /
            (1.0 + static_cast<double>(c.a1) * z_inv + static_cast<double>(c.a2) * z2);

        const double s = static_cast<double>(semi_);
        return units::linear_to_db(std::abs((1.0 - s) * lp + s * shelf));
    }

    /// BL(x) falloff coefficient beta actually in force.
    double bl_beta() const { return compression_amount_ * kBlFalloff; }

    /// Cms(x) stiffening coefficient gamma actually in force.
    double cms_gamma() const { return compression_amount_ * kCmsStiffening; }

    /// Normalized excursion produced by the most recent sample.
    double excursion() const { return static_cast<double>(excursion_); }

    /// The resonance cutoff currently in force, including Cms stiffening.
    double dynamic_fc_hz() const { return fc_dynamic_; }

    int archetype_index() const { return archetype_index_; }
    SpeakerBoxType box_type() const { return box_type_; }
    double sample_rate() const { return sample_rate_; }

private:
    // ── Voice-coil inductance: first-order rolloff blended with a shelf ──────
    // Leach's semi-inductive motor measures a gentler slope than a clean
    // -6 dB/oct, because eddy-current losses make the coil's impedance rise
    // sub-linearly. Blending a fraction of a high-shelved copy of the input
    // back in floors the decay instead of letting it fall forever.
    SampleType inductance(SampleType x) {
        const SampleType rolled = inductance_lp_.process_lowpass(x);
        const SampleType shelved = semi_shelf_.process(x);
        return (SampleType{1} - semi_) * rolled + semi_ * shelved;
    }

    void retarget_smoothed() {
        q_smoothed_.set_target(static_cast<SampleType>(resonance_q()));
        presence_db_.set_target(static_cast<SampleType>(presence_shelf_db()));
        proximity_db_.set_target(static_cast<SampleType>(proximity_gain_db()));
        offaxis_hz_.set_target(static_cast<SampleType>(offaxis_corner_hz()));
    }

    /// Control-cadence coefficient recompute. Everything transcendental lives
    /// here; the hot loop is arithmetic plus the resonator tick.
    void update_coefficients() {
        const auto fs = static_cast<SampleType>(sample_rate_);

        // Advance the smoothers by one control block so their ramps track
        // real time even though coefficients only move at this cadence.
        const double q = static_cast<double>(q_smoothed_.current());
        const double treble = static_cast<double>(treble_hz_.current());
        const double presence = static_cast<double>(presence_db_.current());
        const double proximity = static_cast<double>(proximity_db_.current());
        const double offaxis = static_cast<double>(offaxis_hz_.current());
        q_smoothed_.skip(kControlBlock);
        treble_hz_.skip(kControlBlock);
        presence_db_.skip(kControlBlock);
        proximity_db_.skip(kControlBlock);
        offaxis_hz_.skip(kControlBlock);

        // Cms(x) stiffening from the PREVIOUS block's peak excursion: raising
        // fc at high excursion reduces the displacement that produced it, so
        // the loop is compressive negative feedback and cannot run away.
        const double peak = static_cast<double>(excursion_peak_);
        fc_dynamic_ = resonance_fc_hz() * (1.0 + cms_gamma() * peak * peak);
        excursion_peak_ = SampleType{0};
        resonance_.set_coefficients(fc_dynamic_, q, sample_rate_);

        beta_ = static_cast<SampleType>(bl_beta());
        excursion_scale_ = static_cast<SampleType>(1.0 / kExcursionReference);

        inductance_lp_.set_cutoff(static_cast<SampleType>(treble));
        semi_ = static_cast<SampleType>(semi_inductance_);
        semi_shelf_.set_coefficients(Biquad::Type::high_shelf, static_cast<SampleType>(treble),
                                     kShelfQ, fs,
                                     static_cast<SampleType>(kSemiInductanceShelfDb));

        for (int i = 0; i < kBreakupModeCount; ++i) {
            const auto& mode = breakup_mode(i);
            breakup_[i].set_coefficients(
                Biquad::Type::peaking, static_cast<SampleType>(breakup_mode_hz(i)),
                static_cast<SampleType>(mode.q), fs,
                static_cast<SampleType>(mode.gain_db * breakup_amount_));
        }

        baffle_.set_coefficients(Biquad::Type::high_shelf,
                                 static_cast<SampleType>(baffle_step_hz()), kShelfQ, fs,
                                 static_cast<SampleType>(kBaffleStepDb * diffraction_amount_));
        const double ripple1 = ripple_hz();
        ripple_[0].set_coefficients(Biquad::Type::peaking, static_cast<SampleType>(ripple1),
                                    static_cast<SampleType>(kRipple1Q), fs,
                                    static_cast<SampleType>(kRipple1Db * diffraction_amount_));
        ripple_[1].set_coefficients(Biquad::Type::peaking, static_cast<SampleType>(2.0 * ripple1),
                                    static_cast<SampleType>(kRipple2Q), fs,
                                    static_cast<SampleType>(kRipple2Db * diffraction_amount_));

        dipole_hp_.set_cutoff(static_cast<SampleType>(dipole_hz()));
        dipole_notch_.set_coefficients(
            Biquad::Type::peaking, static_cast<SampleType>(dipole_hz() * kDipoleNotchRatio),
            static_cast<SampleType>(kDipoleNotchQ), fs,
            static_cast<SampleType>(kDipoleNotchDb));

        proximity_.set_coefficients(Biquad::Type::low_shelf,
                                    static_cast<SampleType>(kProximityCornerHz), kShelfQ, fs,
                                    static_cast<SampleType>(proximity));
        air_loss_.set_coefficients(Biquad::Type::high_shelf,
                                   static_cast<SampleType>(kAirLossCornerHz), kShelfQ, fs,
                                   static_cast<SampleType>(air_loss_db()));
        presence_.set_coefficients(Biquad::Type::high_shelf,
                                   static_cast<SampleType>(kPresenceCornerHz), kShelfQ, fs,
                                   static_cast<SampleType>(presence));
        offaxis_lp_.set_cutoff(static_cast<SampleType>(offaxis));
    }

    /// Shelf Q for every first-order-ish shelving section. 1/sqrt(2) is the
    /// maximally flat (Butterworth) shelf slope — no overshoot at the corner,
    /// which is what "first-order shelf" means in the specification's sense.
    static constexpr SampleType kShelfQ = static_cast<SampleType>(0.70710678118654752440);

    using Biquad = BiquadT<SampleType>;

    // ── Configuration ────────────────────────────────────────────────────────
    double sample_rate_ = 48000.0;
    int archetype_index_ = 0;
    SpeakerBoxType box_type_ = SpeakerBoxType::open_back;
    double box_volume_l_ = kBoxVolumeLDefault;
    double resonance_trim_ = 0.0;
    double q_override_ = 0.0;
    double breakup_amount_ = kConeBreakupAmountDefault / 100.0;
    double treble_hz_target_ = kTrebleRolloffHzDefault;
    double semi_inductance_ = kSemiInductanceDefault;
    double drive_db_ = kDriveDbDefault;
    double compression_amount_ = kCompressionAmountDefault / 100.0;
    double mic_distance_cm_ = kMicDistanceCmDefault;
    double mic_position_ = kMicPositionPctDefault / 100.0;
    double mic_axis_deg_ = 0.0;
    double diffraction_amount_ = kDiffractionAmountDefault / 100.0;
    double out_trim_db_ = 0.0;

    // ── Smoothed control values ──────────────────────────────────────────────
    SmoothedValue<SampleType> drive_lin_{SampleType{1}};
    SmoothedValue<SampleType> out_trim_lin_{SampleType{1}};
    SmoothedValue<SampleType> treble_hz_{static_cast<SampleType>(kTrebleRolloffHzDefault)};
    SmoothedValue<SampleType> q_smoothed_{SampleType{1}};
    SmoothedValue<SampleType> presence_db_{SampleType{0}};
    SmoothedValue<SampleType> proximity_db_{SampleType{0}};
    SmoothedValue<SampleType> offaxis_hz_{static_cast<SampleType>(kOffAxisOnAxisHz)};

    // ── Audio state ──────────────────────────────────────────────────────────
    DcBlocker<SampleType> dc_;
    detail::DualTapResonatorT<SampleType> resonance_;
    TptFilterT<SampleType> inductance_lp_;
    Biquad semi_shelf_;
    Biquad breakup_[kBreakupModeCount];
    Biquad baffle_;
    Biquad ripple_[2];
    TptFilterT<SampleType> dipole_hp_;
    Biquad dipole_notch_;
    Biquad proximity_;
    Biquad air_loss_;
    Biquad presence_;
    TptFilterT<SampleType> offaxis_lp_;

    SampleType beta_ = SampleType{0};
    SampleType semi_ = static_cast<SampleType>(kSemiInductanceDefault);
    SampleType excursion_scale_ = static_cast<SampleType>(1.0 / kExcursionReference);
    SampleType excursion_ = SampleType{0};
    SampleType excursion_peak_ = SampleType{0};
    double fc_dynamic_ = 0.0;
    int control_counter_ = 0;
};

using SpeakerModel = SpeakerModelT<float>;
using SpeakerModel64 = SpeakerModelT<double>;

/// ## Voicing cookbook
///
/// * **V1 Cranked British combo** — archetype 0, sealed, 28 L, `q 1.4`,
///   `drive +14 dB`, `compression 80 %`, mic 25 % / 3 cm / on-axis. The
///   resonance bark plus excursion give under hard input.
/// * **V2 Open-back clean chime** — archetype 2, open-back, `q 0.7`,
///   `drive 0 dB`, `breakup 100 %`, mic 10 % / 8 cm. Loose dipole low end plus
///   full cone cry.
/// * **V3 Dark rhythm bed** — archetype 0, mic 80 % / 45 degrees,
///   `treble_rolloff 3000 Hz`. Both HF moves stack.
/// * **V4 Proximity thump** — any 12", `mic_distance 1 cm`, `q 1.6`. The
///   clamped proximity shelf over the resonance hump.
/// * **V5 Bass cab** — archetype 4, sealed, 90 L, `drive +6 dB`,
///   `compression 100 %`. Low fc and heavy excursion compression: the sag of a
///   driven bass cab.

}  // namespace pulp::signal
