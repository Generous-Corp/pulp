#pragma once

/// @file diode_bridge_compressor.hpp
/// The diode-bridge compressor lineage (Neve 2254 / 33609): gain reduction by
/// pushing control current through a bridge of four diodes, bracketed by two
/// coloured transformers.
///
/// The thing that separates this lineage from the VCA and FET ones is WHERE THE
/// COLOUR COMES FROM. In a VCA or FET design the gain element is asked to be
/// clean and the character is argued about in the ballistics; here the audio
/// physically passes THROUGH a nonlinear variable resistor, so the gain element
/// itself generates harmonics, and it generates MORE of them the harder it is
/// compressing. Everything in this header exists to make that mechanism
/// measurable rather than merely described.
///
/// ## Three classes, one header
///
/// `DiodeBridgeGainT` is the variable attenuator, `TransformerBracketT` is one
/// input/output transformer, and `DiodeBridgeCompressorT` is the complete
/// device: two brackets around one bridge, with a feedback sidechain composed
/// from `BallisticsFilterT`. They ship together because the compressor is the
/// only sensible consumer of the other two and all three share one calibration
/// story — splitting them would scatter one set of decisions across three
/// files, exactly as `distortion.hpp` argues for its clipper family.
///
/// ## The physics, in one paragraph
///
/// A diode's small-signal dynamic resistance is `r_d = n·V_T/(I + Is)` — the
/// reciprocal of the slope of Shockley's law, so resistance falls as bias
/// current rises. Put that variable resistance in a divider against a fixed
/// series resistance `Rs` and the audio gain is
///
/// ```
/// g(I) = r_d/(r_d + Rs) = 1/(1 + x),      x ≡ Rs·(I + Is)/(n·V_T)
/// ```
///
/// `x` is the whole control law, and it is DIMENSIONLESS (series law 7): `Rs`,
/// `n` and `V_T` fold into it and never appear separately, which is why the
/// worked table in the spec is independent of all three. `x ≥ 0` always, so
/// `g ∈ (0, 1]` — **the gain element can never boost**, and that is the
/// structural fact the whole worst-case-gain bound rests on.
///
/// **The exponential is not written here.** `junction.hpp` owns the thermal
/// voltage, the overflow-safe exponent, and the closed-form conductance; this
/// header calls `JunctionPair::conductance(knee_voltage(I))` to get `1/r_d` and
/// therefore `x`. There is exactly one junction implementation in this library
/// and this module is a consumer of it, not a second author of it.
///
/// ## Why four diodes (and what that buys the model)
///
/// One diode would rectify the control current straight into the audio path as
/// a thump. Four in a bridge, two legs of two, driven differentially, put the
/// control current at the audio tap as a balanced COMMON-MODE term that cancels
/// to first order, while the audio — injected differentially — sees a matched
/// pair of variable resistances. Matching sets the residual control
/// feedthrough; this model assumes ideal matching, so feedthrough is
/// zero and the only surviving artefact is the intended audio-path curvature.
/// The audio therefore sees an ANTIPARALLEL MATCHED PAIR, which is why the
/// curvature below is odd-symmetric (third harmonic) rather than rectifying.
///
/// ## The noise-for-colour trade, made a knob
///
/// Neve kept the signal tiny across the bridge — attenuate going in, recover
/// with a large makeup afterwards — so the diodes sit in their near-linear
/// region and the gain law stays accurate. The price is noise: that makeup
/// stage amplifies the bridge's own thermal noise along with its colour. Here
/// the trade is explicit and tunable instead of fixed: `character` sets how
/// hard the bridge is driven internally, DECOUPLED from the level path, so
/// colour and loudness are independent controls (series law 3 — the macro is
/// the surface, not an internal level).
///
/// ## Anti-aliasing policy (series law 4)
///
/// First-order antiderivative antialiasing (ADAA), not oversampling, on both
/// memoryless shapers. The curvature is mild by construction (the operating
/// amplitude is small by design), and ADAA reports zero integer latency, which
/// is what lets `latency_samples()` be 0. First-order ADAA carries a half-sample
/// group delay; that is sub-sample colouration, not integer latency, and is not
/// reported. `set_adaa(false)` exists so the suite can measure the alias
/// reduction against the naive evaluation, not as a shipped mode.
/// References: Parker, Zavalishin & Le Bivic, DAFx-16; Bilbao, Esqueda, Parker
/// & Välimäki, IEEE Signal Processing Letters 24(7):1049–1053, 2017.
///
/// ## Determinism (series law 2)
///
/// No randomness anywhere in this file — no dither, no analog drift — so
/// renders are bit-identical per (params, input) by construction. Stated rather
/// than assumed, because it is easy to suppose a "vintage" model needs drift.
/// If a later revision wants it, it must use seeded `Xorshift32`/`OuWalkT` from
/// `rng.hpp`, seeded at `reset()`, never macro-exposed.
///
/// ## Model scope
///
/// The model implements the publicly documented topology and behaviour:
/// four-diode bridge, common-mode cancellation of the control current, large
/// input attenuation ahead of the bridge with makeup after, feedback detection,
/// transformer brackets, slow-attack low-frequency de-sensitisation, and a
/// smooth-at-rest to overdriven-when-driven slope. Every time constant, curvature,
/// and saturation coefficient below is a `[design parameter]` with a default and
/// range chosen for that behavior.
///
/// RT contract: `prepare()` sets sample rate and coefficients; nothing in this
/// header allocates, at any point, including `prepare()` — all state is POD and
/// fixed-size, and zero-init is a valid fresh instance. `set_*`, `process()`
/// and `reset()` never allocate, never lock, never perform I/O, and are safe
/// per sample on the audio thread. `process()` costs a bounded handful of
/// multiplies plus, on a control change, at most one `exp`/`pow`; there is no
/// iteration and no data-dependent work.
///
/// References: Shockley, "The Theory of p-n Junctions in Semiconductors and p-n
/// Junction Transistors", Bell System Technical Journal 28(3):435–489, 1949 —
/// the exponential law and the dynamic resistance `r_d = n·V_T/I`. Giannoulis,
/// Massberg & Reiss, "Digital Dynamic Range Compressor Design — A Tutorial and
/// Analysis", JAES 60(6):399–408, 2012 — the soft-knee gain computer, used as
/// the standard formulation it is. Parker/Zavalishin/Le Bivic and
/// Bilbao/Esqueda/Parker/Välimäki as above for ADAA.

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dynamics_core.hpp>
#include <pulp/signal/junction.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

// ── The current-controlled variable attenuator ────────────────────────────

/// The diode bridge as a gain element: an exact `1/(1 + x)` divider plus the
/// audio-path curvature that divider's diodes actually impose.
///
/// **The gain magnitude is not where the colour lives.** That is deliberate and
/// it is the point of the topology: the divider realises the requested gain
/// reduction to within floating-point, and the harmonics come from a separate,
/// separately-scaled shaper. A caller that wants gain reduction with no colour
/// sets `character` low; a caller that wants colour at unity gain drives the
/// shaper with `control_drive = 0`.
///
/// **Small-signal gain is exactly `g`** (series law 1). The shaper is applied to
/// `s = drive·u` and divided back by `drive`, so its contribution to the
/// input→output slope at the origin is `shape'(0) = 1` identically, at every
/// `character` setting. Without that normalisation the character knob would be
/// a hidden level control of up to −24 dB, which is precisely the defect series
/// law 1 exists to prevent.
template <typename SampleType = float>
class DiodeBridgeGainT {
public:
    // ── Design parameters (the complete roster) ───────────────────────────

    /// Emission coefficient of the bridge diodes. Sets how many volts of bias
    /// buy a decade of current, and therefore the current scale of the control
    /// law — but it folds entirely into the dimensionless `x`, so no test can
    /// see it except through `control_drive_for_current()`.
    /// [design parameter] default 1.9, range 1.0 .. 2.0 (silicon signal-diode).
    static constexpr double kIdeality = 1.9;

    /// Reverse saturation current of one bridge diode. Order-of-magnitude for
    /// the device CLASS, never a measured part; like `kIdeality` it is visible
    /// only through the current→drive conversion.
    /// [design parameter] default 1e-14 A, range 1e-15 .. 1e-12 A.
    static constexpr double kSaturationCurrent = 1e-14;

    /// Series reference resistance the bridge's variable resistance divides
    /// against. Fixes the control CURRENT that produces a given `x`; the gain
    /// law itself is independent of it.
    /// [design parameter] default 1e3 Ω, range 100 Ω .. 100 kΩ.
    static constexpr double kSeriesResistance = 1000.0;

    /// Base audio-path curvature, at rest. Tuned to the documented "low
    /// distortion by design" regime — see `third_harmonic_ratio()` for what it
    /// buys in measurable terms.
    /// **Honest gap:** no public measured THD-vs-drive curve for this bridge
    /// exists at citable precision. This, `kDriveCurvature` and
    /// `kDriveBridgeMax` are original engineering tuned to the documented
    /// QUALITATIVE behaviour, not lifted from hardware.
    /// [design parameter] default 0.5, range 0.0 .. 1.5.
    static constexpr double kBaseCurvature = 0.5;

    /// How fast curvature grows with control drive — the smooth-at-rest →
    /// overdriven-when-driven slope, as a slope rather than an adjective:
    /// `β(x) = β0·(1 + κ·x)`.
    /// [design parameter] default 0.6, range 0.0 .. 2.0.
    static constexpr double kDriveCurvature = 0.6;

    /// Internal operating amplitude at `character = 1`: full-scale audio maps
    /// to `|s| ≤ kDriveBridgeMax`, comfortably inside the monotonic region.
    /// [design parameter] default 0.20, range 0.05 .. 0.40.
    static constexpr double kDriveBridgeMax = 0.20;

    /// Fraction of `kDriveBridgeMax` still applied at `character = 0`. Nonzero
    /// so the block never becomes a bit-exact pass-through — the bridge is
    /// always in the audio path in hardware, and a character knob that reaches
    /// literal zero invites callers to use it as a bypass.
    /// [design parameter] default 0.3, range 0.0 .. 1.0.
    static constexpr double kDriveBridgeFloor = 0.3;

    /// Assumed worst-case control drive, used only to size the monotonicity
    /// guard's headroom claim. `x = 30.62` is 30 dB of gain reduction.
    /// [design parameter] default 30.62, range 14.85 .. 99.0 (24 .. 40 dB).
    static constexpr double kMaxControlDrive = 30.62;

    /// Fraction of the `1/√β` monotonicity limit the input clamp uses. The
    /// cubic folds back beyond `1/√β`; this keeps a margin so the fold is
    /// unreachable rather than merely unlikely.
    /// [design parameter] default 0.98, range 0.90 .. 0.99.
    static constexpr double kMonotonicMargin = 0.98;

    /// ADAA ill-conditioning guard on `|s[n] − s[n−1]|`. Must sit far below a
    /// typical sample-to-sample increment of the drive-scaled signal (at 1 kHz,
    /// full scale, `character = 1`, that increment is ~2.6e-2) and far above
    /// float epsilon.
    /// [design parameter] default 1e-5, range 1e-7 .. 1e-4.
    static constexpr double kAdaaEpsilon = 1e-5;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    DiodeBridgeGainT() { configure_junction(); }

    /// Present for lifecycle symmetry with the rest of the catalog. The shaper
    /// is memoryless, so there is no rate-dependent coefficient to compute;
    /// this only clears state. Never allocates.
    void prepare(double /*sample_rate*/) { reset(); }

    void reset() { previous_shaped_ = 0.0; }

    // ── Controls ──────────────────────────────────────────────────────────

    /// 0..1 colour depth. Scales the internal operating amplitude only — it is
    /// not a level control and does not touch the gain law.
    void set_character(double character01) {
        if (!std::isfinite(character01)) return;
        const double c = std::clamp(character01, 0.0, 1.0);
        drive_ = kDriveBridgeMax * (kDriveBridgeFloor + (1.0 - kDriveBridgeFloor) * c);
    }

    /// Off is for measurement only — it is what the alias-reduction test
    /// compares against.
    void set_adaa(bool on) { adaa_ = on; }

    /// The internal operating amplitude full-scale audio is mapped to.
    double drive() const { return drive_; }

    // ── The control law ───────────────────────────────────────────────────

    /// `g = 1/(1 + x)`. The exact inverse of `control_drive_for_gain_db()`, so a
    /// sidechain that computes gain reduction in dB and inverts it gets that
    /// reduction back to within floating-point.
    static double gain_for_control_drive(double control_drive) {
        return 1.0 / (1.0 + std::max(0.0, control_drive));
    }

    /// `x = 10^(|GR|/20) − 1`, the drive that realises a given (negative) gain
    /// reduction in dB.
    static double control_drive_for_gain_db(double gain_reduction_db) {
        return units::db_to_linear(-std::min(0.0, gain_reduction_db)) - 1.0;
    }

    /// The control drive produced by a physical bias current, in amps.
    ///
    /// This is the one place `(Is, n, V_T, Rs)` are visible, and it is computed
    /// THROUGH the shared junction law rather than by restating Shockley's
    /// equation: the bias point is the junction's own conduction knee at that
    /// current, and `1/r_d` is the junction's own incremental conductance
    /// there. `x = Rs/r_d` follows. Exposed because "resistance is inversely
    /// proportional to bias current" is the claim the whole topology rests on,
    /// and a claim a test can check is worth more than one it cannot.
    double control_drive_for_current(double amperes) const {
        const double bias = network_.knee_voltage(std::max(amperes, 0.0));
        return kSeriesResistance * network_.conductance(bias) - series_offset_;
    }

    /// The bridge's small-signal dynamic resistance at a bias current, in ohms.
    /// `n·V_T/(I + Is)` — inverted from the same junction, not restated.
    double dynamic_resistance(double amperes) const {
        return 1.0 / network_.conductance(network_.knee_voltage(std::max(amperes, 0.0)));
    }

    // ── The colour ────────────────────────────────────────────────────────

    /// `β(x) = β0·(1 + κ·x)` — the curvature in force at a given control drive.
    static double curvature(double control_drive) {
        return kBaseCurvature * (1.0 + kDriveCurvature * std::max(0.0, control_drive));
    }

    /// The odd cubic the balanced bridge imposes, with slope exactly 1 at the
    /// origin: `shape(s) = s − β·s³/3`.
    static double shape(double s, double beta) { return s - beta * s * s * s / 3.0; }

    /// `∫shape ds = s²/2 − β·s⁴/12`. Even, as the antiderivative of an odd
    /// function must be — a sign error here would produce a waveform that is
    /// wrong on half of each cycle and still reads as "the distortion working".
    static double shape_antiderivative(double s, double beta) {
        const double s2 = s * s;
        return 0.5 * s2 - beta * s2 * s2 / 12.0;
    }

    /// Where the cubic stops being monotonic, less the safety margin.
    static double max_operating_amplitude(double beta) {
        return beta > 0.0 ? kMonotonicMargin / std::sqrt(beta) : 1e30;
    }

    /// Closed-form third-harmonic ratio of the shaper for a sine of amplitude
    /// `s` at curvature `β`, from the identity `sin³θ = (3sinθ − sin3θ)/4`:
    ///
    /// ```
    /// shape(s·sinθ) = s(1 − β·s²/4)·sinθ + (β·s³/12)·sin3θ
    /// THD3 = (β·s³/12) / (s(1 − β·s²/4)) = β·s²/12 / (1 − β·s²/4)
    /// ```
    ///
    /// Stated as a shipped function, not as a test-side literal, so the
    /// acceptance suite compares the measurement against the model rather than
    /// against a number someone typed. (The spec's worked example gives
    /// `β·s²/3`, which drops the factor of 4 from the `sin³` expansion — see
    /// the module's defect log.)
    static double third_harmonic_ratio(double s, double beta) {
        const double fundamental = 1.0 - beta * s * s / 4.0;
        if (!(fundamental > 0.0)) return 0.0;
        return (beta * s * s / 12.0) / fundamental;
    }

    // ── Processing ────────────────────────────────────────────────────────

    /// One sample. `control_drive` is `x`; the gain `1/(1 + x)` is DERIVED from
    /// it rather than passed alongside it.
    ///
    /// The spec's signature takes a linear gain target AND a control drive.
    /// They are exact inverses of one another by §3.2, so accepting both admits
    /// an inconsistent pair — a caller that smooths one and not the other gets
    /// a gain law that silently stops matching its own gain reduction. One
    /// argument cannot disagree with itself.
    SampleType process(SampleType input, double control_drive) {
        if (!std::isfinite(static_cast<double>(input)) || !std::isfinite(control_drive)) {
            reset();
            return SampleType{0};
        }
        const double x = std::max(0.0, control_drive);
        const double beta = curvature(x);
        const double limit = max_operating_amplitude(beta);
        const double s = std::clamp(static_cast<double>(input) * drive_, -limit, limit);

        double shaped;
        if (adaa_) {
            const double delta = s - previous_shaped_;
            shaped = std::abs(delta) > kAdaaEpsilon
                         ? (shape_antiderivative(s, beta) -
                            shape_antiderivative(previous_shaped_, beta)) /
                               delta
                         : shape(0.5 * (s + previous_shaped_), beta);
        } else {
            shaped = shape(s, beta);
        }
        previous_shaped_ = s;

        // Divide the drive back out: the shaper contributes harmonics, never
        // level (series law 1). See the class doc block.
        const double output = snap_to_zero(gain_for_control_drive(x) * shaped / drive_);
        if (!std::isfinite(output)) {
            reset();
            return SampleType{0};
        }
        return static_cast<SampleType>(output);
    }

private:
    void configure_junction() {
        network_.saturation_current = kSaturationCurrent;
        network_.ideality = kIdeality;
        // The audio sees the balanced bridge as an antiparallel MATCHED pair —
        // symmetric, hence the odd-only curvature above. One diode per leg.
        network_.leg_a = 1.0;
        network_.leg_b = 1.0;
        // `conductance` at zero bias is not zero (it is `2·Is/θ`), so the drive
        // law is offset to make `x(0) = 0` exactly. Without this a zero control
        // current would ask for a gain of 1/(1 + 1.5e-10) rather than unity —
        // inaudible, but it would make "no current means no reduction" an
        // approximation instead of an identity.
        series_offset_ = kSeriesResistance * network_.conductance(0.0);
    }

    junction::JunctionPair network_{};
    double series_offset_ = 0.0;
    double drive_ = kDriveBridgeMax * (kDriveBridgeFloor + (1.0 - kDriveBridgeFloor) * 0.35);
    double previous_shaped_ = 0.0;
    bool adaa_ = true;
};

using DiodeBridgeGain = DiodeBridgeGainT<float>;
using DiodeBridgeGain64 = DiodeBridgeGainT<double>;

// ── The transformer bracket ───────────────────────────────────────────────

/// One input or output transformer: band-limit, mild saturation, DC-blocked.
///
/// **Peak gain is exactly 1.0, and that is load-bearing** — it is the `Tpeak`
/// the compressor's worst-case-gain bound multiplies by, so it has to be a
/// property of the construction rather than a measured approximation. Three
/// stages, each with magnitude ≤ 1 at every frequency and amplitude:
/// a one-pole high-pass, a compressive saturator whose gain is `1 − (a/2)|u|`
/// (≤ 1 by inspection, = 1 only at the origin), and a one-pole low-pass.
///
/// **Why not `DcBlocker`.** The spec composes `DcBlocker` for the LF corner.
/// Its transfer function `(1 − z⁻¹)/(1 − p·z⁻¹)` has magnitude `2/(1 + p)` at
/// Nyquist — 1.0025 at the default pole. Two brackets would therefore have a
/// peak gain of 1.005, and the `Tpeak = 1.0` the worst-case bound cites would
/// be false by construction. The house TPT one-pole's high-pass output is
/// `1 − LP`, whose magnitude is ≤ 1 everywhere with equality only at Nyquist,
/// and it is still an exact DC block (`H(0) = 0`). Same existing primitive
/// family, one that does not void the invariant it is used inside.
///
/// **Order: saturate, then high-pass.** The saturator's even-harmonic term
/// rectifies, so it generates a program-dependent DC offset. Placing the
/// high-pass after it makes one filter do both jobs the spec names — the
/// documented LF droop AND the DC block — and keeps that offset out of the
/// output sum and out of the feedback detector, which would otherwise read it
/// as signal and hold gain down on a silent passage.
///
/// **The even harmonics are real, and they need an asymmetry to exist.** The
/// spec prescribes `sat(u) = u − (a/2)·u·|u|` and describes it as adding a
/// second harmonic. It does not: `u·|u|` is an ODD function, so that shaper is
/// odd-symmetric and produces odd harmonics only — which is also the physically
/// right answer for a SYMMETRIC magnetic core. Even harmonics in a transformer
/// come from asymmetry. `kEvenAsymmetry` is the parameter that supplies it, and
/// at `kEvenAsymmetry = 0` this reduces to the spec's formula exactly:
///
/// ```
/// sat(u) = u − (a/2)·u·|u| − (a·ε/2)·u²        slope(0) = 1
/// F1(u)  = u²/2 − (a/6)·u²·|u| − (a·ε/6)·u³
/// ```
///
/// RT contract: as the file doc block. Nothing allocates.
template <typename SampleType = float>
class TransformerBracketT {
public:
    /// Low-frequency corner: the transformer's LF droop, and the stage's DC
    /// block.
    /// [design parameter] default 20 Hz, range 10 .. 40 Hz.
    static constexpr double kLowCornerHz = 20.0;

    /// High-frequency corner: winding bandwidth.
    /// [design parameter] default 28 kHz, range 18 .. 40 kHz.
    static constexpr double kHighCornerHz = 28000.0;

    /// Saturation depth at `character = 1`. Small: a line-level bracket runs at
    /// far lower flux than a tape head, which is also why hysteresis is out of
    /// scope here (see the module's scope notes).
    /// [design parameter] default 0.15, range 0.0 .. 0.5.
    static constexpr double kSaturationDepth = 0.15;

    /// How much of the saturation is asymmetric, i.e. how even-harmonic-forward
    /// the bracket is. 0 is the spec's odd-only formula; 1 leaves the negative
    /// half of the curve untouched entirely.
    /// [design parameter] default 0.5, range 0.0 .. 1.0.
    static constexpr double kEvenAsymmetry = 0.5;

    /// Fraction of the `1/(a(1+ε))` monotonicity limit the input clamp uses.
    /// At the default depth that limit is ~4.4 — far outside any real signal,
    /// so this is a guard against a caller feeding the bracket something wild,
    /// not a shaping stage.
    /// [design parameter] default 0.98, range 0.90 .. 0.99.
    static constexpr double kMonotonicMargin = 0.98;

    /// ADAA ill-conditioning guard, as `DiodeBridgeGainT::kAdaaEpsilon`.
    /// [design parameter] default 1e-5, range 1e-7 .. 1e-4.
    static constexpr double kAdaaEpsilon = 1e-5;

    /// The peak gain this stage can present, at any frequency and any
    /// amplitude. Exactly 1 by construction — see the class doc block. Named
    /// rather than written as a literal because the compressor's worst-case
    /// bound multiplies two of them.
    static constexpr double kPeakGain = 1.0;

    void prepare(double sample_rate) {
        highpass_.prepare(static_cast<SampleType>(sample_rate));
        lowpass_.prepare(static_cast<SampleType>(sample_rate));
        highpass_.set_cutoff(static_cast<SampleType>(kLowCornerHz));
        lowpass_.set_cutoff(static_cast<SampleType>(kHighCornerHz));
        reset();
    }

    void reset() {
        highpass_.reset();
        lowpass_.reset();
        previous_input_ = 0.0;
    }

    /// 0..1 colour depth, shared with the bridge.
    void set_character(double character01) {
        if (!std::isfinite(character01)) return;
        const double a = kSaturationDepth * std::clamp(character01, 0.0, 1.0);
        positive_ = a * (1.0 + kEvenAsymmetry);
        negative_ = a * (1.0 - kEvenAsymmetry);
        symmetric_ = a;
        asymmetric_ = a * kEvenAsymmetry;
        const double steepest = std::max(positive_, negative_);
        limit_ = steepest > 0.0 ? kMonotonicMargin / steepest : 1e30;
    }

    void set_adaa(bool on) { adaa_ = on; }

    /// `u − (a/2)·u·|u| − (a·ε/2)·u²`. Slope 1 at the origin; `|sat(u)/u| ≤ 1`
    /// everywhere inside the monotonic region.
    double saturate(double u) const {
        return u - 0.5 * symmetric_ * u * std::abs(u) - 0.5 * asymmetric_ * u * u;
    }

    /// `∫sat du`. Continuous and continuously differentiable at the origin,
    /// which is what makes the ADAA quotient valid across a zero crossing.
    double saturate_antiderivative(double u) const {
        const double u2 = u * u;
        return 0.5 * u2 - symmetric_ * u2 * std::abs(u) / 6.0 - asymmetric_ * u2 * u / 6.0;
    }

    SampleType process(SampleType input) {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const double u = std::clamp(static_cast<double>(input), -limit_, limit_);

        double shaped;
        if (adaa_) {
            const double delta = u - previous_input_;
            shaped = std::abs(delta) > kAdaaEpsilon
                         ? (saturate_antiderivative(u) - saturate_antiderivative(previous_input_)) /
                               delta
                         : saturate(0.5 * (u + previous_input_));
        } else {
            shaped = saturate(u);
        }
        previous_input_ = u;

        const SampleType blocked = highpass_.process_highpass(static_cast<SampleType>(shaped));
        const SampleType output = lowpass_.process_lowpass(blocked);
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

private:
    TptFilterT<SampleType> highpass_{};
    TptFilterT<SampleType> lowpass_{};
    double symmetric_ = kSaturationDepth * 0.35;
    double asymmetric_ = kSaturationDepth * 0.35 * kEvenAsymmetry;
    double positive_ = kSaturationDepth * 0.35 * (1.0 + kEvenAsymmetry);
    double negative_ = kSaturationDepth * 0.35 * (1.0 - kEvenAsymmetry);
    double limit_ = kMonotonicMargin / (kSaturationDepth * 0.35 * (1.0 + kEvenAsymmetry));
    double previous_input_ = 0.0;
    bool adaa_ = true;
};

using TransformerBracket = TransformerBracketT<float>;
using TransformerBracket64 = TransformerBracketT<double>;

// ── The complete device ───────────────────────────────────────────────────

/// Two transformer brackets around one diode bridge, with a feedback sidechain.
///
/// ## Feedback is the default, and it changes what the ratio knob means
///
/// With `feedback` on, the detector senses the OUTPUT, so the gain computer's
/// argument already contains the reduction it is about to apply. In the hard
/// region that is a fixed point:
///
/// ```
/// GR = (L_in + GR − thr)·(1/ρ − 1)   ⇒   GR = (L_in − thr)·(1/ρ − 1)/(2 − 1/ρ)
/// ```
///
/// At `L_in − thr = 6 dB` and `ρ = 4` the feed-forward answer is −4.5 dB and the
/// feedback answer is −2.57 dB: less reduction, and an EFFECTIVE ratio of
/// 1.75:1 rather than 4:1. That program-dependent softening is the documented
/// signature of this lineage, not a calibration error — the manual ratio
/// control reads "aggressive" only relative to a far gentler realised curve.
/// `static_curve_feedback_db()` states the closed form so a caller can see the
/// curve it is actually getting.
///
/// The loop closes through a ONE-SAMPLE delay in the control path only. The
/// audio path is strictly feed-forward — gain multiply, two memoryless shapers,
/// one-pole filters — so `latency_samples()` is 0 and the impulse response
/// starts at `n = 0`.
///
/// ## The worst-case gain bound (series law 8)
///
/// The bridge can only attenuate (`g ∈ (0, 1]`), each bracket has peak gain
/// exactly 1, and the only element that can boost is the makeup control. So
///
/// ```
/// worst_case_gain = 10^(24/20) · 1.0 · 1.0 = 15.849
/// ```
///
/// bounds the ratio of output peak to input peak at every setting and every
/// input. The suite asserts it directly; it is not an estimate.
///
/// ## Patch notes
///
/// - **Stereo-bus glue** — ratio 1.5–2, threshold for 2–4 dB of reduction,
///   attack 10–30 ms, auto release, character 0.3. Slow feedback ballistics
///   plus a touch of third-harmonic bridge colour thickens a mix without
///   pumping.
/// - **Drum-bus attitude** — ratio 4, attack 3 ms, release 400 ms, character
///   0.6. The fast attack grabs transients and `β` rising with `x` puts the
///   smack in as the reduction deepens.
/// - **Vocal levelling** — ratio 3, knee 9 dB, sidechain high-pass 120 Hz,
///   feedback on. The soft feedback knee flatters sustained sources.
/// - **Parallel colour** — mix 30–50 %, ratio 10, character 1. Heavy squash and
///   full colour blended under the dry signal: transformer weight as a tone
///   rather than as levelling.
/// - **Bass control** — the sidechain high-pass IS the "does it duck to the
///   kick?" knob. 30–50 Hz to make it respond to low end, 150 Hz to let the low
///   end through while riding the mids.
/// - **Limit mode** — ratio at or above `kLimitRatio` engages the brickwall
///   region; pair with a fast attack for a ceiling that still carries the
///   bridge and transformer signature.
///
/// ## Out of scope, and why
///
/// A true per-sample diode ODE solve (the `distortion.hpp` treatment) is
/// overkill for a bridge whose operating region is near-linear by design; the
/// closed-form divider plus a mild ADAA'd cubic captures the audible behaviour
/// at a fraction of the cost. Transformer hysteresis (Jiles-Atherton) belongs
/// to the tape tier — a line-level bracket runs far below a tape head's flux.
/// Vari-mu (Fairchild 670 lineage) is a different gain element entirely, grid
/// bias rather than diode current, and a separate catalog entry. Mid/side and
/// true stereo linking are graph concerns; this class is mono, and a linked
/// pair sums detectors at the graph layer.
template <typename SampleType = float>
class DiodeBridgeCompressorT {
public:
    // ── Control ranges (mirrored by the catalog node's table) ─────────────
    static constexpr double kThresholdDbMin = -40.0;
    static constexpr double kThresholdDbMax = 6.0;
    static constexpr double kRatioMin = 1.5;
    static constexpr double kRatioMax = 20.0;
    static constexpr double kAttackMsMin = 0.5;
    static constexpr double kAttackMsMax = 100.0;
    static constexpr double kReleaseMsMin = 50.0;
    static constexpr double kReleaseMsMax = 2000.0;
    static constexpr double kKneeDbMin = 0.0;
    static constexpr double kKneeDbMax = 18.0;
    static constexpr double kMakeupDbMax = 24.0;
    static constexpr double kScHpfHzMin = 20.0;
    static constexpr double kScHpfHzMax = 400.0;

    // ── Design parameters ─────────────────────────────────────────────────

    /// Ratio at or above which the limit (infinite-ratio) region engages. Must
    /// be at or below `kRatioMax` for the control's top position to reach it.
    /// [design parameter] default 20, range 15 .. 25.
    static constexpr double kLimitRatio = 20.0;

    /// Control-signal smoothing. Short: its job is to keep `β(x)` continuous
    /// per sample so the memoryless-ADAA assumption holds, not to shape the
    /// ballistics — those are the detector's.
    /// [design parameter] default 1.0 ms, range 0.2 .. 5.0 ms.
    static constexpr double kCtrlSmoothMs = 1.0;

    /// Slow/fast release ratio of the auto (program-dependent) mode.
    /// [design parameter] default 6.0, range 3.0 .. 10.0.
    static constexpr double kAutoSlowFactor = 6.0;

    /// Time constant of the "has compression been sustained" detector that
    /// blends the two auto-release slopes.
    /// [design parameter] default 200 ms, range 50 .. 1000 ms.
    static constexpr double kSustainTauMs = 200.0;

    /// How much gain reduction counts as sustained, for that blend.
    /// [design parameter] default 3 dB, range 1 .. 12 dB.
    static constexpr double kSustainThresholdDb = 3.0;

    /// Envelope-to-dB log guard. Far below the quietest envelope anyone
    /// measures (−180 dB) and far above float epsilon.
    /// [design parameter] default 1e-9, range 1e-12 .. 1e-6.
    static constexpr double kEnvelopeEpsilon = 1e-9;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Never allocates — every member is fixed-size POD. Kept as `prepare()`
    /// rather than folded into the setters because the filters and followers
    /// need the sample rate before any of them is meaningful.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        input_bracket_.prepare(sample_rate_);
        output_bracket_.prepare(sample_rate_);
        bridge_.prepare(sample_rate_);
        sidechain_hpf_.prepare(static_cast<SampleType>(sample_rate_));
        sidechain_hpf_.set_cutoff(static_cast<SampleType>(sc_hpf_hz_));
        fast_follower_.prepare(static_cast<SampleType>(sample_rate_));
        slow_follower_.prepare(static_cast<SampleType>(sample_rate_));
        fast_follower_.set_mode(BallisticsFilterT<SampleType>::Mode::peak);
        slow_follower_.set_mode(BallisticsFilterT<SampleType>::Mode::peak);
        update_ballistics();
        update_smoothing();
        reset();
    }

    void reset() {
        input_bracket_.reset();
        output_bracket_.reset();
        bridge_.reset();
        sidechain_dc_.reset();
        sidechain_hpf_.reset();
        fast_follower_.reset();
        slow_follower_.reset();
        smoothed_reduction_db_ = 0.0;
        sustain_ = 0.0;
        feedback_tap_ = 0.0;
    }

    // ── Controls (real units throughout) ──────────────────────────────────

    void set_threshold_db(double db) {
        threshold_db_ = std::clamp(dynamics::retain_finite(db, threshold_db_),
                                   kThresholdDbMin, kThresholdDbMax);
    }

    void set_ratio(double r) {
        ratio_ = std::clamp(dynamics::retain_finite(r, ratio_), kRatioMin, kRatioMax);
    }

    void set_knee_db(double w) {
        knee_db_ = std::clamp(dynamics::retain_finite(w, knee_db_),
                              kKneeDbMin, kKneeDbMax);
    }

    void set_attack_ms(double ms) {
        attack_ms_ = std::clamp(dynamics::retain_finite(ms, attack_ms_),
                                kAttackMsMin, kAttackMsMax);
        update_ballistics();
    }

    void set_release_ms(double ms) {
        release_ms_ = std::clamp(dynamics::retain_finite(ms, release_ms_),
                                 kReleaseMsMin, kReleaseMsMax);
        update_ballistics();
    }

    void set_makeup_db(double db) {
        makeup_db_ = std::clamp(dynamics::retain_finite(db, makeup_db_),
                                0.0, kMakeupDbMax);
        makeup_linear_ = units::db_to_linear(makeup_db_);
    }

    /// 0..1 colour depth — the one macro. Drives the bridge's internal
    /// operating amplitude and both brackets' saturation depth together, which
    /// is what makes it one knob rather than three.
    void set_character(double character01) {
        character_ = std::clamp(dynamics::retain_finite(character01, character_),
                                0.0, 1.0);
        bridge_.set_character(character_);
        input_bracket_.set_character(character_);
        output_bracket_.set_character(character_);
    }

    /// Parallel dry/wet as a percentage, matching the catalog table's units.
    void set_mix_percent(double percent) {
        const double current_percent = mix_ * 100.0;
        mix_ = std::clamp(dynamics::retain_finite(percent, current_percent),
                          0.0, 100.0) / 100.0;
    }

    void set_sc_hpf_hz(double hz) {
        sc_hpf_hz_ = std::clamp(dynamics::retain_finite(hz, sc_hpf_hz_),
                                kScHpfHzMin, kScHpfHzMax);
        sidechain_hpf_.set_cutoff(static_cast<SampleType>(sc_hpf_hz_));
    }

    /// Program-dependent dual-slope release.
    void set_auto_release(bool on) { auto_release_ = on; }

    /// On is the authentic topology: the detector senses the output.
    void set_feedback(bool on) { feedback_ = on; }

    void set_adaa(bool on) {
        bridge_.set_adaa(on);
        input_bracket_.set_adaa(on);
        output_bracket_.set_adaa(on);
    }

    // ── Queries ───────────────────────────────────────────────────────────

    /// Zero. The audio path is a gain multiply, two memoryless ADAA shapers and
    /// one-pole filters; ADAA's half-sample group delay is sub-sample
    /// colouration and the one-poles add phase, not delay (series law 5).
    int latency_samples() const noexcept { return 0; }

    /// Smoothed gain reduction in dB (≤ 0) as of the last processed sample.
    double gain_reduction_db() const { return smoothed_reduction_db_; }

    /// The control drive `x` currently applied to the bridge.
    double control_drive() const {
        return DiodeBridgeGainT<SampleType>::control_drive_for_gain_db(smoothed_reduction_db_);
    }

    /// The static characteristic's gain reduction (≤ 0) for an input level in
    /// dB, ignoring the loop and the detector. Exposed so a caller or a test can
    /// plot the curve without running audio through it.
    /// This lineage's ratio control maps its top of range onto a true limiter,
    /// so the resolved slope — not the ratio — is what goes to the shared
    /// equation: `1/ρ` normally, exactly 0 at and above `kLimitRatio`.
    double static_curve_db(double input_db) const {
        const double slope = ratio_ >= kLimitRatio ? 0.0 : 1.0 / ratio_;
        return dynamics::soft_knee_reduction_db(input_db - threshold_db_, knee_db_, slope);
    }

    /// The gain reduction the FEEDBACK loop settles at for a given input level
    /// — the fixed point of `GR = static_curve(L_in + GR)`, solved by iteration
    /// because the soft knee makes the closed form piecewise.
    ///
    /// The iteration is AVERAGED (`gr ← ½(gr + f(gr))`), and that is not a
    /// refinement — the plain iteration does not converge at the top of the
    /// ratio control. `f`'s slope is `1/ρ − 1`, which is −0.95 at ρ = 20 and
    /// exactly −1 in the limit region, so plain iteration either crawls or
    /// oscillates forever between two values without ever narrowing. Averaging
    /// maps that slope to `(1/ρ)/2 ∈ [0, ½]`, so the worst case is now the
    /// GENTLEST ratio rather than the harshest, and the limit region converges
    /// in a single step. Not an RT path — this is a curve query.
    double static_curve_feedback_db(double input_db) const {
        double gr = 0.0;
        for (int i = 0; i < 96; ++i) {
            const double next = 0.5 * (gr + static_curve_db(input_db + gr));
            if (next == gr) break;
            gr = next;
        }
        return gr;
    }

    /// The bound the Forge registry cites (series law 8): makeup ceiling times
    /// both brackets' peak gain. The bridge contributes at most 1.
    static double worst_case_gain() {
        return units::db_to_linear(kMakeupDbMax) * TransformerBracketT<SampleType>::kPeakGain *
               TransformerBracketT<SampleType>::kPeakGain;
    }

    // ── Processing ────────────────────────────────────────────────────────

    SampleType process(SampleType input) {
        // Every stage below owns recursive state, including the feedback tap.
        // A non-finite input is therefore a recovery boundary: do not let it
        // enter the loop, and make the next finite sample start validly.
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const double dry = static_cast<double>(input);

        // Detection. The feedback tap is last sample's pre-makeup output: the
        // loop's only delay, and it lives in the CONTROL path, not the audio.
        const double detector_input = feedback_ ? feedback_tap_ : dry;
        const double envelope = detect(detector_input);
        const double envelope_db =
            units::linear_to_db(std::abs(envelope) + kEnvelopeEpsilon);

        const double target_db = static_curve_db(envelope_db);
        smoothed_reduction_db_ = snap_to_zero(smoothed_reduction_db_ +
                                              smooth_coef_ * (target_db - smoothed_reduction_db_));

        // Gain and drive are derived from ONE smoothed quantity, so they cannot
        // disagree about how much reduction is being applied.
        const double gain = units::db_to_linear(smoothed_reduction_db_);
        const double drive = 1.0 / gain - 1.0;

        const SampleType bracketed = input_bracket_.process(input);
        const SampleType attenuated = bridge_.process(bracketed, drive);
        const SampleType wet = output_bracket_.process(attenuated);

        feedback_tap_ = static_cast<double>(wet);
        return static_cast<SampleType>(dry * (1.0 - mix_) +
                                       static_cast<double>(wet) * makeup_linear_ * mix_);
    }

    void process_block(SampleType* io, int n) {
        for (int i = 0; i < n; ++i) io[i] = process(io[i]);
    }

private:
    /// Sidechain: DC block, high-pass, rectify, follow.
    ///
    /// The high-pass at ~100 Hz is what produces the documented low-frequency
    /// de-sensitisation — the compressor stops chasing the kick. Note the size
    /// of the effect is the high-pass attenuation times `(1 − 1/ratio)`, NOT the
    /// attenuation itself: the gain computer only passes a fraction of a level
    /// change through to the reduction.
    double detect(double detector_input) {
        const SampleType blocked = sidechain_dc_.process(static_cast<SampleType>(detector_input));
        const SampleType filtered = sidechain_hpf_.process_highpass(blocked);

        const double fast = static_cast<double>(fast_follower_.process(filtered));
        // Keep the slow branch synchronized even while its readout is disabled.
        // Otherwise enabling Auto mid-release blends `fast` against a stale
        // zero state and briefly recovers gain faster than the manual follower.
        const double slow = static_cast<double>(slow_follower_.process(filtered));
        if (!auto_release_) return fast;

        // Program-dependent dual slope. The spec says `max(fast, slow)`, which
        // is identically the SLOW follower — during release both decay from the
        // same value and the slow one is always the larger, so the maximum
        // never selects the fast branch and the mode degenerates into a single
        // long release with nothing program-dependent about it. Blending on a
        // sustain indicator is what makes it dual-slope: fast recovery right
        // after a transient, slow hold once compression has been sustained.
        // Equals `max(fast, slow)` in the sustained limit.
        const double sustained = -smoothed_reduction_db_ > kSustainThresholdDb ? 1.0 : 0.0;
        sustain_ = snap_to_zero(sustain_ + sustain_coef_ * (sustained - sustain_));
        return fast + sustain_ * (slow - fast);
    }

    void update_ballistics() {
        const auto attack = static_cast<SampleType>(attack_ms_);
        fast_follower_.set_attack_ms(attack);
        slow_follower_.set_attack_ms(attack);
        fast_follower_.set_release_ms(static_cast<SampleType>(release_ms_));
        slow_follower_.set_release_ms(static_cast<SampleType>(release_ms_ * kAutoSlowFactor));
    }

    void update_smoothing() {
        smooth_coef_ = units::ms_to_onepole_coef(kCtrlSmoothMs, sample_rate_);
        sustain_coef_ = units::ms_to_onepole_coef(kSustainTauMs, sample_rate_);
    }

    double sample_rate_ = 48000.0;

    double threshold_db_ = -12.0;
    double ratio_ = 4.0;
    double knee_db_ = 6.0;
    double attack_ms_ = 3.0;
    double release_ms_ = 400.0;
    double makeup_db_ = 0.0;
    double character_ = 0.35;
    double mix_ = 1.0;
    double sc_hpf_hz_ = 100.0;
    bool auto_release_ = false;
    bool feedback_ = true;

    double makeup_linear_ = 1.0;
    double smooth_coef_ = 1.0;
    double sustain_coef_ = 1.0;

    double smoothed_reduction_db_ = 0.0;
    double sustain_ = 0.0;
    double feedback_tap_ = 0.0;

    TransformerBracketT<SampleType> input_bracket_{};
    TransformerBracketT<SampleType> output_bracket_{};
    DiodeBridgeGainT<SampleType> bridge_{};
    DcBlocker<SampleType> sidechain_dc_{};
    TptFilterT<SampleType> sidechain_hpf_{};
    BallisticsFilterT<SampleType> fast_follower_{};
    BallisticsFilterT<SampleType> slow_follower_{};
};

using DiodeBridgeCompressor = DiodeBridgeCompressorT<float>;
using DiodeBridgeCompressor64 = DiodeBridgeCompressorT<double>;

}  // namespace pulp::signal
