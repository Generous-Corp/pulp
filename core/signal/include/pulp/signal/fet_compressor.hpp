#pragma once

/// @file fet_compressor.hpp
/// The FET compressor: a FEEDBACK-topology design in the documented 1176
/// lineage, with a FET operated as a voltage-controlled resistor inside the
/// loop, microsecond attack times, discrete ratio buttons including the
/// documented "all buttons in" state, no threshold control, and an output
/// transformer's low-frequency tilt as a colour stage.
///
/// "Feedback" is the whole topology in one word, and it is the only thing that
/// really separates this from `feedforward_compressor.hpp`: the level detector
/// senses the compressor's OWN OUTPUT, not its input. Everything downstream of
/// that one wire — the stability story, the static curve a user actually
/// measures, the knee they actually hear, and how fast the thing really
/// attacks — changes, and each of those changes is quantified below and
/// asserted by the suite rather than described.
///
/// The gain computer, soft knee and branching attack/release detector come from
/// Giannoulis, Massberg & Reiss, "Digital Dynamic Range Compressor Design — A
/// Tutorial and Analysis", JAES 60(6), pp. 399–408, 2012. The divider law is
/// the Shichman–Hodges triode approximation (Shichman & Hodges, IEEE JSSC
/// SC-3(3), pp. 285–289, 1968): a FET in its ohmic region has drain CONDUCTANCE
/// approximately linear in gate overdrive, so conductance — not resistance — is
/// what interpolates linearly with the control voltage. The "all buttons in"
/// state is documented qualitatively by Moore, A., "All Buttons In: An
/// Investigation Into the Use of the 1176 FET Compressor in Popular Music
/// Production", Journal on the Art of Record Production, Issue 6, 2012.
/// Everything not from those sources is tagged `[design parameter]` with a
/// default and a range.
///
/// ## The sign convention, stated because it is easy to get backwards
///
/// `gain_computer_db()` returns gain reduction as a POSITIVE magnitude in dB —
/// the paper's convention, and the same one `feedforward_compressor.hpp` uses.
/// The detector runs in positives throughout, so the attack branch is taken
/// when MORE reduction is wanted. Running the branch on the negative
/// gain-computer output instead would make the detector snap toward less
/// reduction and ease into more, i.e. swap attack and release — a version that
/// still sounds like a working compressor and fails a step-response test
/// immediately, which is why there is one.
///
/// ## The loop, and why it is stable (series law 1)
///
/// The detector reads `y[n−1]`, the previous OVERSAMPLED divider output, so the
/// loop is closed explicitly with a one-sample delay. There is no implicit
/// solve anywhere in `process()`: the divider law is evaluated in closed form
/// and its inverse is closed form too. **The iteration count is zero, at every
/// sample, unconditionally** — which is the only version of "bounded iteration
/// count" that is actually RT-safe.
///
/// Linearising the loop in the dB domain around any operating point:
///
/// ```
/// δy_L[n] = (α − (1−α)·A·B) · δy_L[n−1] + (1−α)·B · δx̂[n−1]
///
///   A = dΓ/dD   how many dB the divider actually applies per dB the
///               detector asks for  — the loop's gain-carrying element
///   B = dD/dŷ   the static curve's slope, = 1 − 1/R above the knee,
///               ramping 0 → (1 − 1/R) across it
/// ```
///
/// **`A` is where the naive design fails.** The spec this module is built from
/// normalises the control voltage as `c = y_L / GR_max_db`, i.e. it assumes the
/// divider's dB attenuation is linear in `c`. It is not: `Γ(c) = 20·log10(1 +
/// R_s·g_fet(c)) − 20·log10(1 + R_s·g_min)` with `g_fet` linear in `c`, which
/// at the shipped constants is `Γ(c) ≈ 20·log10(1 + 99·c)`. That maps `c = 0.5`
/// onto 34 dB of attenuation when the detector asked for 20, and gives
/// `A = dΓ/dD ≈ 21.5` near `c = 0`. At the documented 20 µs attack
/// (`α = 0.770` at 192 kHz) and `R = 8` that puts the closed-loop pole at
/// `α − (1−α)·A·B = 0.770 − 0.230·21.5·0.875 = −3.55` — **an unstable loop**,
/// arrived at by arithmetic, not by taste.
///
/// So the control voltage is instead the CLOSED-FORM INVERSE of the divider law
/// (`control_for_reduction_db()`), which makes `A ≡ 1` by construction — the
/// loop is unity-compensated, exactly as series law 1 asks. That is also the
/// same calibration intent the divider already carries: `R_s` is solved at
/// `prepare()` so the endpoints land on 0 dB and −`kGainReductionMaxDb`
/// exactly, and inverting the law simply extends that calibration to every
/// point between them rather than only the two ends.
///
/// With `A = 1` the pole is `p = α − (1−α)·B` with `α ∈ [0,1)` and
/// `B ∈ [0, 1 − 1/R]`, so
///
/// ```
/// |p| ≤ max(α, 1 − 1/R_max) = max(α, 0.95) < 1
/// ```
///
/// unconditionally, at every attack time, ratio, knee and sample rate. That is
/// the bound the suite asserts — `loop_slope()` returns `B`, and a test drives
/// the fastest attack against the hardest ratio and checks the settling is
/// monotone rather than ringing.
///
/// ## What the feedback loop does to the numbers on the panel
///
/// A feedback detector is not a flavour of a feedforward one; it re-maps the
/// static curve, and this module exposes the re-mapping instead of describing
/// it. With `m = 1 − 1/R` the closed-loop fixed point `ŷ = x̂ − D(ŷ)` solves in
/// closed form (`measured_static_curve_db()`), and the consequences are:
///
/// - **Ratio.** The measured input/output ratio is `1 + m = 2 − 1/R`, NOT `R`.
///   The 4:1 button measures 1.75:1, the 20:1 button measures 1.95:1. A
///   feedback compressor of this shape simply cannot exceed 2:1 measured — that
///   gentleness under a nominally brutal ratio setting IS the lineage's
///   documented character, not a defect. `measured_ratio()` reports it.
/// - **Knee.** The knee the user sets is `knee_db` wide in the DETECTOR's
///   domain, which is the output. Referred back to the input it is
///   `knee_db·(2 + m)/2` wide — 47 % wider at the 20:1 button.
///   `measured_knee_db()` reports it.
/// - **Attack.** The loop accelerates it. The closed-loop pole
///   `p = α − (1−α)·B` gives an effective time constant of roughly
///   `τ_A / (1 + B)` — at the 200 µs default and 8:1 that is 107 µs, nearly
///   twice as fast as the knob says. Release toward silence is NOT accelerated:
///   once the output falls below the knee, `B = 0`, the loop opens, and the
///   release runs at its own `τ_R`. That asymmetry is a direct, measurable
///   consequence of the topology.
///
/// ## Oversampling policy (series law 4) — 4×, and it is not optional
///
/// `g_fet_inst` is a memoryless nonlinearity evaluated inside the loop, so it
/// generates harmonics and aliases. The stage runs at 4× on a linear-phase
/// Kaiser (β = 8) 65-tap polyphase pair. 4× is chosen rather than merely
/// defaulted: it is also what keeps the one-sample loop delay at `1/(4·fs)` =
/// 5.2 µs at 48 kHz, comfortably under the fastest documented 20 µs attack.
/// At 1× the same delay would be 20.8 µs — the same order as the fastest time
/// constant the circuit is documented to have, which is why 1× is rejected.
///
/// The source spec calls this a "half-band pair", which it is not, and the two
/// readings do not cost the same. A genuine half-band pair is two cascaded 2×
/// stages: 65 taps at 2× contributes 16 base-rate samples and 65 taps at 4×
/// contributes 8, so a two-stage pair would report 48 samples of latency, not
/// 16. The spec's own §3.8 arithmetic — "(65−1)/2 = 32 at the oversampled rate
/// for each of the up/down filters ... 16 total" — is only consistent with a
/// SINGLE-stage 4× interpolation/decimation pair, which is what ships, because
/// that is the reading the tested latency figure depends on. "Pair" here means
/// interpolator + decimator, not two half-band stages.
///
/// The 65-tap length is a hard constraint from the latency contract below, and
/// it is short for the job. Measured on the shipped taps at
/// `kResamplerCutoffFraction`: the round trip is flat to 15 kHz at 48 kHz,
/// −0.44 dB at 18 kHz, −1.34 dB at 19.2 kHz, and rejects 30 kHz by 57.6 dB.
/// The honest limitation, stated rather than buried: a fundamental near
/// `fs/4`, whose second harmonic lands in the pair's transition band around
/// `fs/2`, is the worst case for aliasing — a longer filter would fix it and
/// would change `latency_samples()`.
///
/// ## Latency (series law 5)
///
/// Exactly `kLatencySamples` = 16 samples at the base rate, independent of
/// every parameter. The linear-phase prototype's group delay is `(65−1)/2 = 32`
/// samples at the oversampled rate for each of the interpolation and decimation
/// filters; 32 is divisible by the 4× factor, so each contributes exactly 8
/// base-rate samples and the pair contributes 16 — an exact integer, computed
/// from the shipped constants, with no fractional residue to round away. The
/// one-sample feedback delay is internal to the oversampled loop and delays the
/// control path, not the audio path, so it adds nothing to the reported figure.
/// The dry path of `set_mix()` is delayed by the same 16 samples, or the
/// parallel blend would be a comb filter.
///
/// ## Worst-case gain (series law 8)
///
/// The divider is a series-resistor / shunt-FET attenuator. Its supremum gain
/// is **exactly 1**, attained only at rest (`c = 0`), and the coloration term
/// cannot lift it above 1: raising `c` raises the shunt conductance far faster
/// than the coloration term can lower it (`Δg·(1 − α·v_clamp) ≫ α·v_clamp·g_min`
/// by a factor of ~5×10⁴ at the shipped constants). `worst_case_gain()`
/// therefore returns a closed-form ℓ∞ bound on the whole block:
///
/// ```
/// (1 − mix) + mix · input_gain · 1 · resampler_peak_gain_bound() ·
///             output_gain · 1
/// ```
///
/// where the middle `1`s are the divider and the cut-only transformer shelf,
/// and `resampler_peak_gain_bound()` is `‖h_up‖₁·‖h_down‖₁` computed from the
/// shipped taps (≈ 3.06 — a true worst-case-input bound; the measured overshoot
/// on a full-scale step is 1.045). The suite asserts realised gain never
/// exceeds it, asserts the divider supremum is exactly 1 over the whole
/// `(c, v)` grid, and separately asserts the spec's own conductance-multiplier
/// bound `1 + α_max·v_clamp`.
///
/// **A note on that last one, because it is stated backwards in the source
/// spec.** The spec's registry entry cites `worst_case_gain ≤ 1 + α_max_ABI ·
/// v_clamp = 1.1344`. That quantity is the bound on the CONDUCTANCE multiplier
/// `1 + α(c)·v`, and gain is `1/(1 + R_s·g)` — so a higher conductance means a
/// LOWER gain. The 1.1344 figure bounds the divider's gain from below, not
/// above, and cannot serve as a `worst_case_gain`. `coloration_multiplier_bound()`
/// returns it under its correct name; `worst_case_gain()` returns the bound that
/// actually bounds the gain.
///
/// ## No threshold, on purpose
///
/// There is no `set_threshold_db()`. `kThresholdDbfs` is a fixed internal
/// reference and `input_gain_db` is the only lever into gain reduction, which
/// is the documented hardware's arrangement: the circuit has no threshold knob,
/// and the input control is what drives program material into the fixed
/// reference.
///
/// ## Use-case starting points
///
/// - **Drum-room crush** — all buttons in, input +18 dB, attack at the 20 µs
///   minimum, release ~100 ms. The mode exists for exactly this.
/// - **Vocal 4:1** — `r4_1`, input +8 dB, attack ~400 µs, release ~300 ms,
///   knee 2 dB. Measured ratio 1.75:1 — levelling, not squashing.
/// - **Bass 8:1** — `r8_1`, input +12 dB, attack ~800 µs (slow enough to let
///   the fundamental's first cycle through), release ~150 ms.
/// - **Parallel** — any ratio, `mix` 0.3–0.5, input hot. The dry path is taken
///   PRE input gain and delay-aligned, so the blend is a true parallel
///   compression, not a wet/dry of an already-driven signal.
///
/// ## Out of scope, and why
///
/// Power-supply sag is a second-order effect with no citable public analysis
/// for this lineage. Meter ballistics belong with a UI component. Stereo link
/// composes as two instances driven from a caller-computed shared detector
/// signal; a convenience wrapper is future work. A SPICE-accurate device model
/// would add cost with no citable audible target to validate against.
///
/// RT contract: `prepare()` designs the resampler taps and the transformer
/// shelf and is the only call permitted to be expensive; in this implementation
/// every buffer is a fixed-size `std::array`, so `prepare()` performs **no heap
/// allocation at all** — strictly stronger than the contract requires.
/// `set_*()`, `process()` and `reset()` never allocate, never lock, never
/// throw, and never perform I/O. `process()` is a pure function of (state,
/// input) with no hidden globals; all recursive state is denormal-snapped.
/// Control changes are consumed immediately by the setters (a clamp, a store,
/// and at most one `exp`/`pow` for a coefficient) rather than through a
/// `SmoothedValue` stage — matching `feedforward_compressor.hpp`, because the
/// catalog node drives every knob per sample and a smoother there would be a
/// second, redundant smoothing stage with its own state to keep deterministic.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dynamics_core.hpp>
#include <pulp/signal/dynamics_contract.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/windowed_sinc_design.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// The ratio buttons. `all_buttons_in` is not a fifth ratio — it is the
/// documented distinct circuit state produced by engaging all four buttons at
/// once (Moore 2012), modelled here as four simultaneous deviations from the
/// 20:1 baseline.
enum class FetRatio : std::uint8_t {
    r4_1,
    r8_1,
    r12_1,
    r20_1,
    all_buttons_in,
};

/// Feedback-topology FET compressor.
template <typename SampleType = float>
class FetCompressorT {
public:
    using Ratio = FetRatio;

    // ── Resampler geometry: the latency contract's inputs ──────────────────

    /// Oversampling factor. Chosen, not defaulted — see the file doc block.
    /// Fixed at build time because changing it changes `latency_samples()`.
    /// [design parameter] default 4, range {2, 4, 8}.
    static constexpr int kOversamplingFactor = 4;

    /// Prototype length. Odd, and `(kResamplerTaps − 1)/2` is divisible by
    /// `kOversamplingFactor`, which is what makes the base-rate group delay an
    /// exact integer rather than something to round.
    /// [design parameter] default 65, range 33 .. 257 (of the form 8k+1).
    static constexpr int kResamplerTaps = 65;

    /// Kaiser shape parameter of the prototype. House policy value.
    /// [design parameter] default 8.0, range 6.0 .. 12.0.
    static constexpr double kResamplerKaiserBeta = 8.0;

    /// Prototype cutoff, as a fraction of the BASE rate's Nyquist. Lower buys
    /// alias rejection and costs passband; 0.96 is the balance measured in the
    /// file doc block. [design parameter] default 0.96, range 0.85 .. 1.0.
    static constexpr double kResamplerCutoffFraction = 0.96;

    /// The whole module's latency, in base-rate samples. Exact, parameter
    /// independent, computed from the geometry above.
    static constexpr int kLatencySamples =
        2 * ((kResamplerTaps - 1) / 2) / kOversamplingFactor;

    // ── Design parameters: the divider ────────────────────────────────────

    /// FET fully on — the maximum gain reduction the divider can express.
    /// [design parameter] default 120 Ω, range 50 .. 500 Ω.
    static constexpr double kFetOnResistance = 120.0;

    /// FET fully off — negligible gain reduction at rest.
    /// [design parameter] default 1 MΩ, range 200 kΩ .. 5 MΩ.
    static constexpr double kFetOffResistance = 1.0e6;

    /// The control-voltage span the divider law is normalised against: the
    /// attenuation the divider hits exactly at `c = 1`.
    /// [design parameter] default 40 dB, range 20 .. 60 dB.
    static constexpr double kGainReductionMaxDb = 40.0;

    // ── Design parameters: the coloration term ────────────────────────────

    /// Coloration depth ceiling. `α(c) = kColorationAlphaMax · c`, so the
    /// distortion scales with how hard the FET is being driven.
    /// **Honest gap:** no citable literature gives a coloration-depth constant
    /// for any historical unit — those numbers are measured and proprietary,
    /// not published. Original engineering, sized to sit inside the documented
    /// qualitative envelope. [design parameter] default 0.08, range 0.0 .. 0.2.
    static constexpr double kColorationAlphaMax = 0.08;

    /// Hard ceiling on the effective coloration depth, whatever the ABI
    /// multiplier is re-tuned to. [design parameter] default 0.16, fixed.
    static constexpr double kColorationAlphaCeiling = 0.16;

    /// Headroom clamp on the instantaneous sample fed to the coloration term.
    /// Bounds the correction so it can neither invert nor blow up the divider
    /// law. [design parameter] default 1.2, range 1.0 .. 1.5.
    static constexpr double kColorationHeadroomClamp = 1.2;

    // ── Design parameters: all-buttons-in ─────────────────────────────────
    //
    // **Honest gap, stated plainly.** Moore (2012) documents the QUALITATIVE
    // phenomenon — heavier, more distorted, less predictable compression — from
    // listening and usage analysis, not circuit-level component values. No
    // citable source gives numbers for any of the five constants below. They
    // are original engineering choices; re-tuning by ear must stay inside the
    // stated ranges and must preserve the DIRECTION of each deviation.

    /// ABI reuses the 20:1 static curve rather than inventing a fifth ratio.
    static constexpr double kAbiRatio = 20.0;

    /// Widened knee — the documented "curve chaos" softness.
    /// [design parameter] default 2.5, range 1.5 .. 4.0.
    static constexpr double kAbiKneeWidenMult = 2.5;

    /// Detector bias-point shift, added directly to the gain-reduction
    /// magnitude before smoothing. Note this is added unconditionally, so ABI
    /// carries a small standing reduction even below the reference — which is
    /// what a bias shift means and what the mode is documented to do.
    /// [design parameter] default 1.5 dB, range 0.5 .. 3.0 dB.
    static constexpr double kAbiBiasShiftDb = 1.5;

    /// Documented faster reaction. [design parameter] default 0.6, range
    /// 0.4 .. 0.8.
    static constexpr double kAbiAttackScale = 0.6;

    /// [design parameter] default 0.75, range 0.5 .. 0.9.
    static constexpr double kAbiReleaseScale = 0.75;

    /// Raises the coloration ceiling — the documented increased distortion.
    /// [design parameter] default 1.4, range 1.2 .. 2.0.
    static constexpr double kAbiAlphaExtraMult = 1.4;

    // ── Design parameters: the transformer tilt ───────────────────────────

    /// Corner of the output transformer's low-frequency shelf. A transformer
    /// loses level below its primary-inductance corner, so the shelf CUTS —
    /// which is also what keeps it out of the worst-case gain bound.
    /// [design parameter] default 60 Hz, range 20 .. 200 Hz.
    static constexpr double kTransformerCornerHz = 60.0;

    /// Tilt depth at `transformer_amount = 1`.
    /// [design parameter] default 3 dB, range 0 .. 6 dB.
    static constexpr double kTransformerTiltDbMax = 3.0;

    /// Butterworth-flat, the shape with no shelf overshoot — so the stage's
    /// supremum magnitude is exactly 1 and the gain bound stays clean.
    /// [design parameter] default 0.70710678, range 0.3 .. 0.7071.
    static constexpr double kTransformerQ = 0.70710678118654752;

    // ── Design parameters: everything else ────────────────────────────────

    /// The fixed internal reference. Configurable only here, never at runtime:
    /// the absence of a threshold control is the point of the design.
    /// [design parameter] default −18 dBFS, range −24 .. −12 dBFS.
    static constexpr double kThresholdDbfs = -18.0;

    /// Level-conversion floor, guarding `log10(0)`. Declared in
    /// `dynamics_core.hpp` alongside the conversions that consume it, and
    /// re-exported here so the call sites and the catalog keep reading one name.
    static constexpr double kLevelEpsilon = dynamics::kLevelEpsilon;

    // Control ranges. These mirror the catalog node's parameter table; the
    // table is the canonical declaration and these are the same numbers at the
    // call site, not a second independent one.
    static constexpr double kInputGainDbMin = -20.0;
    static constexpr double kInputGainDbMax = 40.0;
    static constexpr double kOutputGainDbMin = -20.0;
    static constexpr double kOutputGainDbMax = 20.0;
    static constexpr double kAttackUsMin = 20.0;
    static constexpr double kAttackUsMax = 800.0;
    static constexpr double kReleaseMsMin = 50.0;
    static constexpr double kReleaseMsMax = 1100.0;
    static constexpr double kKneeDbMin = 0.0;
    static constexpr double kKneeDbMax = 6.0;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    FetCompressorT() { prepare(44100.0); }

    /// Designs the resampler prototype, solves the divider's series resistance,
    /// and configures the transformer shelf. Allocates nothing (every buffer is
    /// a fixed-size array), but is the expensive call and belongs off the audio
    /// thread.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        oversampled_rate_ = sample_rate_ * kOversamplingFactor;

        design_resampler();
        solve_divider();
        update_ratio_dependents();
        update_coefficients();
        update_transformer();
        reset();
    }

    /// Zeroes every state buffer. Never allocates; a zero-initialised instance
    /// after `prepare()` is already in this state.
    void reset() {
        base_history_.fill(0.0);
        os_history_.fill(0.0);
        dry_delay_.fill(0.0);
        base_write_ = 0;
        os_write_ = 0;
        dry_write_ = 0;
        detector_db_ = 0.0;
        control_ = 0.0;
        feedback_ = 0.0;
        transformer_.reset();
    }

    // ── Controls (real units throughout) ──────────────────────────────────
    //
    // Taken as `double` rather than `SampleType`. A `double` parameter accepts
    // everything a `SampleType` one would and rounds nothing on the way in, and
    // it matches `feedforward_compressor.hpp` so the two members of the family
    // present the same call shape.

    /// The only lever into gain reduction — there is no threshold control.
    void set_input_gain_db(double db) {
        input_gain_db_ = std::clamp(dynamics::retain_finite(db, input_gain_db_),
                                    kInputGainDbMin, kInputGainDbMax);
        input_gain_ = units::db_to_linear(input_gain_db_);
    }

    /// Makeup gain, applied after the divider and before the transformer.
    void set_output_gain_db(double db) {
        output_gain_db_ = std::clamp(dynamics::retain_finite(db, output_gain_db_),
                                     kOutputGainDbMin, kOutputGainDbMax);
        output_gain_ = units::db_to_linear(output_gain_db_);
    }

    void set_ratio(Ratio r) {
        ratio_button_ = r;
        update_ratio_dependents();
        update_coefficients();
    }

    Ratio ratio() const { return ratio_button_; }

    void set_attack_us(double us) {
        attack_us_ = std::clamp(dynamics::retain_finite(us, attack_us_),
                                kAttackUsMin, kAttackUsMax);
        update_coefficients();
    }

    void set_release_ms(double ms) {
        release_ms_ = std::clamp(dynamics::retain_finite(ms, release_ms_),
                                 kReleaseMsMin, kReleaseMsMax);
        update_coefficients();
    }

    /// Soft-knee width around the fixed reference, in the DETECTOR's domain.
    /// The width a user measures at the input is wider — `measured_knee_db()`.
    void set_knee_db(double db) {
        knee_request_db_ = std::clamp(dynamics::retain_finite(db, knee_request_db_),
                                      kKneeDbMin, kKneeDbMax);
        update_ratio_dependents();
    }

    /// 0..1 depth of the output transformer's low-frequency tilt.
    void set_transformer_amount(double pct01) {
        transformer_amount_ = std::clamp(
            dynamics::retain_finite(pct01, transformer_amount_), 0.0, 1.0);
        update_transformer();
    }

    /// 0..1 parallel blend. The dry path is taken PRE input gain and delayed by
    /// `latency_samples()`, so the blend is not a comb filter.
    void set_mix(double pct01) {
        mix_ = std::clamp(dynamics::retain_finite(pct01, mix_), 0.0, 1.0);
    }

    // ── Reported quantities ───────────────────────────────────────────────

    /// Exact, parameter independent. See the file doc block.
    int latency_samples() const noexcept { return kLatencySamples; }

    double sample_rate() const noexcept { return sample_rate_; }
    double oversampled_rate() const noexcept { return oversampled_rate_; }

    /// The static characteristic's output for a level in dB, per Giannoulis et
    /// al. — evaluated in the DETECTOR's domain, which for this topology is the
    /// compressor's own OUTPUT. This is NOT the input/output curve a user
    /// measures; `measured_static_curve_db()` is.
    double static_curve_db(double level_db) const {
        return dynamics::soft_knee_output_db(level_db, kThresholdDbfs, knee_db_, ratio_value_);
    }

    /// Gain reduction the static curve asks for, as a POSITIVE magnitude in dB.
    double gain_computer_db(double level_db) const {
        return level_db - static_curve_db(level_db);
    }

    /// The static curve a user actually measures, input dB → output dB: the
    /// closed-loop fixed point `ŷ = x̂ − bias − D(ŷ)`, solved in closed form on
    /// each of the three branches. No iteration.
    double measured_static_curve_db(double input_db) const {
        const double m = loop_slope();
        const double w = knee_db_;
        const double over = input_db - bias_shift_db_ - kThresholdDbfs;
        if (2.0 * over < -w) return kThresholdDbfs + over;
        if (w > 0.0 && m > 0.0 && 2.0 * over <= w * (1.0 + m)) {
            // Quadratic branch: (m/2W)·s² + s − (over + W/2) = 0, s = u + W/2.
            const double s =
                (w / m) * (std::sqrt(1.0 + (2.0 * m / w) * (over + 0.5 * w)) - 1.0);
            return kThresholdDbfs + s - 0.5 * w;
        }
        return kThresholdDbfs + over / (1.0 + m);
    }

    /// Measured gain reduction in dB (≥ 0) at a steady input level.
    double measured_gain_reduction_db(double input_db) const {
        return input_db - measured_static_curve_db(input_db);
    }

    /// The static curve's slope in the detector's domain, `B = 1 − 1/R` — the
    /// loop's only remaining gain term once the divider law is inverted.
    double loop_slope() const { return 1.0 - 1.0 / ratio_value_; }

    /// The measured input/output ratio: `1 + B = 2 − 1/R`, never `R`.
    double measured_ratio() const { return 1.0 + loop_slope(); }

    /// The knee width a user measures at the INPUT, in dB. Wider than the one
    /// they set, by the loop's own re-mapping.
    double measured_knee_db() const { return knee_db_ * (2.0 + loop_slope()) * 0.5; }

    /// The nominal ratio feeding the gain computer (20 in all-buttons-in).
    double nominal_ratio() const { return ratio_value_; }

    /// Knee width in force, after the all-buttons-in widening.
    double effective_knee_db() const { return knee_db_; }

    /// Detector bias shift in force (non-zero only in all-buttons-in).
    double bias_shift_db() const { return bias_shift_db_; }

    /// Coloration depth ceiling in force: `α_max`, or `α_max·kAbiAlphaExtraMult`
    /// clamped to `kColorationAlphaCeiling` in all-buttons-in.
    double coloration_depth() const { return alpha_max_; }

    /// Detector retain coefficients, `exp(−1/(τ·fs_os))`. Exposed so a test can
    /// predict the closed-loop pole from the SHIPPED values rather than
    /// restating them.
    double attack_coefficient() const { return attack_coef_; }
    double release_coefficient() const { return release_coef_; }

    /// Smoothed gain reduction in dB (≥ 0, a magnitude) as of the last
    /// oversampled step — the detector itself, exposed for metering and so
    /// tests can measure it directly instead of inferring it from audio that
    /// crosses zero every half cycle.
    double gain_reduction_db() const { return detector_db_; }
    GainReduction gain_reduction() const noexcept {
        return GainReduction::from_magnitude_db(detector_db_);
    }

    /// The control voltage currently driving the divider, 0..1.
    double control_voltage() const { return control_; }

    // ── The divider law ───────────────────────────────────────────────────

    /// Shunt conductance at control voltage `c` — the quantity that is linear
    /// in control voltage, per Shichman & Hodges.
    double divider_conductance(double c) const {
        return g_min_ + std::clamp(c, 0.0, 1.0) * (g_max_ - g_min_);
    }

    /// Small-signal divider gain at fixed `c`: `R_fet/(R_s + R_fet)` with the
    /// rest-state trim folded in, so `c = 0` is exactly unity and `c = 1` is
    /// exactly `−kGainReductionMaxDb`. The coloration term is a DEVIATION from
    /// this, never folded into it, so the documented small-signal gain stays
    /// exact and auditable independent of drive level (series law 1).
    double divider_small_signal_gain(double c) const {
        return trim_ / (1.0 + series_resistance_ * divider_conductance(c));
    }

    /// Instantaneous divider gain, including the coloration term. `v` is the
    /// divider-referred audio sample; it is clamped to
    /// `±kColorationHeadroomClamp` before use.
    double divider_gain(double c, double v) const {
        return trim_ / (1.0 + series_resistance_ *
                                  divider_conductance(c) * coloration_multiplier(c, v));
    }

    /// `1 + α(c)·v`, the conductance multiplier. Bounded by
    /// `coloration_multiplier_bound()`.
    double coloration_multiplier(double c, double v) const {
        const double clamped = std::clamp(v, -kColorationHeadroomClamp, kColorationHeadroomClamp);
        return 1.0 + alpha_max_ * std::clamp(c, 0.0, 1.0) * clamped;
    }

    /// The bound on `|1 + α(c)·v|`. This is the quantity the source spec's
    /// registry row calls `worst_case_gain`; it is NOT one — see the file doc
    /// block. Bounds the divider's gain from BELOW.
    double coloration_multiplier_bound() const {
        return 1.0 + alpha_max_ * kColorationHeadroomClamp;
    }

    /// The exact inverse of `divider_small_signal_gain` in the dB domain: the
    /// control voltage that makes the divider apply `db` dB of attenuation.
    /// Closed form — one `pow`, no iteration, which is what makes the loop
    /// unity-compensated without an implicit solve in `process()`.
    double control_for_reduction_db(double db) const {
        if (!(db > 0.0)) return 0.0;
        const double rest = 1.0 + series_resistance_ * g_min_;
        const double target = rest * std::pow(10.0, db / 20.0);
        const double c = (target - rest) / (series_resistance_ * (g_max_ - g_min_));
        return std::clamp(c, 0.0, 1.0);
    }

    /// The divider's supremum gain over the whole `(c, v)` grid. Exactly 1: the
    /// stage is a passive attenuator and the coloration term cannot lift it,
    /// because `Δg·(1 − α·v_clamp) ≫ α·v_clamp·g_min` at the shipped constants.
    /// `divider_supremum_is_provable()` checks that inequality numerically so a
    /// re-tune of the design parameters cannot silently break the claim.
    static constexpr double kDividerGainSupremum = 1.0;

    bool divider_supremum_is_provable() const {
        const double a = kColorationAlphaCeiling * kColorationHeadroomClamp;
        return (g_max_ - g_min_) * (1.0 - a) > a * g_min_;
    }

    /// `‖h_up‖₁ · ‖h_down‖₁` for the shipped taps — a true ℓ∞ bound on the
    /// resampling pair's peak gain for ANY bounded input, not a step-response
    /// measurement. Conservative by design: the measured overshoot on a
    /// full-scale step is 1.045.
    double resampler_peak_gain_bound() const { return resampler_peak_bound_; }

    /// Closed-form ℓ∞ bound on `|y/x|` for the whole block at the current
    /// settings (series law 8). Every factor is either exactly 1 with a proof
    /// (the divider, the cut-only transformer shelf) or a shipped gain.
    double worst_case_gain() const {
        const double wet = input_gain_ * kDividerGainSupremum * resampler_peak_bound_ *
                           output_gain_ * kTransformerGainSupremum;
        return (1.0 - mix_) + mix_ * wet;
    }

    /// The transformer stage's supremum magnitude. A low shelf with a
    /// non-positive gain and `Q ≤ 1/√2` is monotone, so its peak is its
    /// high-frequency asymptote, exactly 1.
    static constexpr double kTransformerGainSupremum = 1.0;

    // ── Processing ────────────────────────────────────────────────────────

    /// One mono sample. Stereo composes two instances driven from a
    /// caller-computed shared detector signal.
    SampleType process(SampleType input) {
        // The feedback tap, both resampler histories, the dry delay, and the
        // transformer are recursive. Reject a non-finite sample before any of
        // them sees it, then restart from the class's documented fresh state.
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }
        const double x = static_cast<double>(input);
        const double dry = push_dry(x);
        const double driven = x * input_gain_;

        // Interpolate: one base sample in, `kOversamplingFactor` out.
        base_history_[base_write_] = driven;

        double decimated = 0.0;
        for (int phase = 0; phase < kOversamplingFactor; ++phase) {
            const double v = interpolate_phase(phase);

            // ── the loop ──────────────────────────────────────────────────
            // The detector reads the PREVIOUS oversampled divider output. That
            // one line is the entire topological difference from the
            // feedforward design.
            const double level_db = dynamics::amplitude_db(feedback_, kLevelEpsilon);
            const double requested = gain_computer_db(level_db) + bias_shift_db_;
            detector_db_ = snap_to_zero(
                requested > detector_db_
                    ? attack_coef_ * detector_db_ + (1.0 - attack_coef_) * requested
                    : release_coef_ * detector_db_ + (1.0 - release_coef_) * requested);
            control_ = control_for_reduction_db(detector_db_);

            const double y = v * divider_gain(control_, v);
            feedback_ = y;

            os_history_[os_write_] = y;
            // The first oversample is the decimator's output grid point; taking
            // it here keeps the composite delay an exact integer.
            if (phase == 0) decimated = decimate();
            os_write_ = os_write_ + 1 < kResamplerTaps ? os_write_ + 1 : 0;
        }
        base_write_ = base_write_ + 1 < kBaseHistory ? base_write_ + 1 : 0;

        const double wet = transformer_.process(decimated * output_gain_);
        return static_cast<SampleType>(snap_to_zero(dry + mix_ * (wet - dry)));
    }

    void process_block(SampleType* io, int n) {
        for (int i = 0; i < n; ++i) io[i] = process(io[i]);
    }

private:
    static constexpr int kBaseHistory =
        (kResamplerTaps + kOversamplingFactor - 1) / kOversamplingFactor;

    // ── Resampler ─────────────────────────────────────────────────────────

    void design_resampler() {
        // Cutoff expressed in cycles per OVERSAMPLED sample: base Nyquist is
        // `0.5/kOversamplingFactor` there.
        const double cutoff =
            kResamplerCutoffFraction * 0.5 / static_cast<double>(kOversamplingFactor);
        const auto prototype = design_windowed_sinc(static_cast<std::size_t>(kResamplerTaps),
                                                    cutoff, kResamplerKaiserBeta);
        double decimation_l1 = 0.0;
        for (int i = 0; i < kResamplerTaps; ++i) {
            decimation_[static_cast<std::size_t>(i)] = prototype[static_cast<std::size_t>(i)];
            decimation_l1 += std::abs(prototype[static_cast<std::size_t>(i)]);
        }

        // Polyphase decomposition of the interpolator. Zero-stuffing by L drops
        // the DC gain to 1/L, so each branch carries the compensating factor L.
        double interpolation_l1 = 0.0;
        for (int phase = 0; phase < kOversamplingFactor; ++phase) {
            int count = 0;
            double branch_l1 = 0.0;
            for (int tap = phase; tap < kResamplerTaps; tap += kOversamplingFactor) {
                const double coefficient =
                    kOversamplingFactor * prototype[static_cast<std::size_t>(tap)];
                interpolation_[static_cast<std::size_t>(phase)][static_cast<std::size_t>(count)] =
                    coefficient;
                branch_l1 += std::abs(coefficient);
                ++count;
            }
            interpolation_length_[static_cast<std::size_t>(phase)] = count;
            interpolation_l1 = std::max(interpolation_l1, branch_l1);
        }
        resampler_peak_bound_ = interpolation_l1 * decimation_l1;
    }

    /// One oversampled sample from the base-rate history, at `phase`.
    double interpolate_phase(int phase) const {
        const auto p = static_cast<std::size_t>(phase);
        const int count = interpolation_length_[p];
        double sum = 0.0;
        int index = base_write_;
        for (int k = 0; k < count; ++k) {
            sum += interpolation_[p][static_cast<std::size_t>(k)] *
                   base_history_[static_cast<std::size_t>(index)];
            if (--index < 0) index = kBaseHistory - 1;
        }
        return sum;
    }

    /// The decimation filter evaluated at the just-written oversampled sample.
    double decimate() const {
        double sum = 0.0;
        int index = os_write_;
        for (int k = 0; k < kResamplerTaps; ++k) {
            sum += decimation_[static_cast<std::size_t>(k)] *
                   os_history_[static_cast<std::size_t>(index)];
            if (--index < 0) index = kResamplerTaps - 1;
        }
        return sum;
    }

    /// Pushes the raw input into the dry ring and returns it delayed by exactly
    /// `kLatencySamples`, so the parallel blend is not a comb filter.
    double push_dry(double x) {
        constexpr std::size_t size = kLatencySamples + 1;
        const std::size_t read = (dry_write_ + size - kLatencySamples) % size;
        const double out = dry_delay_[read];
        dry_delay_[dry_write_] = x;
        dry_write_ = (dry_write_ + 1) % size;
        return out;
    }

    // ── Derived quantities ────────────────────────────────────────────────

    /// Solves the one free quantity in the divider — the series resistance —
    /// so `c = 0` lands on unity and `c = 1` lands on exactly
    /// `−kGainReductionMaxDb`. Closed form; `R_s` is derived, not a second
    /// independent design parameter.
    void solve_divider() {
        g_min_ = 1.0 / kFetOffResistance;
        g_max_ = 1.0 / kFetOnResistance;
        const double k = std::pow(10.0, -kGainReductionMaxDb / 20.0);
        series_resistance_ = kFetOffResistance * kFetOnResistance * (k - 1.0) /
                             (kFetOnResistance - k * kFetOffResistance);
        trim_ = 1.0 + series_resistance_ * g_min_;
    }

    void update_ratio_dependents() {
        const bool abi = ratio_button_ == Ratio::all_buttons_in;
        switch (ratio_button_) {
            case Ratio::r4_1: ratio_value_ = 4.0; break;
            case Ratio::r8_1: ratio_value_ = 8.0; break;
            case Ratio::r12_1: ratio_value_ = 12.0; break;
            case Ratio::r20_1: ratio_value_ = 20.0; break;
            case Ratio::all_buttons_in: ratio_value_ = kAbiRatio; break;
        }
        knee_db_ = abi ? knee_request_db_ * kAbiKneeWidenMult : knee_request_db_;
        bias_shift_db_ = abi ? kAbiBiasShiftDb : 0.0;
        alpha_max_ = abi ? std::min(kColorationAlphaMax * kAbiAlphaExtraMult,
                                    kColorationAlphaCeiling)
                         : kColorationAlphaMax;
    }

    void update_coefficients() {
        const bool abi = ratio_button_ == Ratio::all_buttons_in;
        const double attack_s =
            attack_us_ * 1e-6 * (abi ? kAbiAttackScale : 1.0);
        const double release_s =
            release_ms_ * 1e-3 * (abi ? kAbiReleaseScale : 1.0);
        attack_coef_ = retain(attack_s);
        release_coef_ = retain(release_s);
    }

    /// The paper's coefficient mapping at the OVERSAMPLED rate, which is where
    /// the ballistics run. RETAIN convention — see `dynamics_core.hpp`.
    double retain(double tau_seconds) const {
        return dynamics::one_pole_retain(tau_seconds, oversampled_rate_);
    }

    void update_transformer() {
        transformer_.set_coefficients(
            BiquadT<double>::Type::low_shelf, static_cast<double>(kTransformerCornerHz),
            kTransformerQ, sample_rate_, -transformer_amount_ * kTransformerTiltDbMax);
    }

    // ── State ─────────────────────────────────────────────────────────────

    double sample_rate_ = 44100.0;
    double oversampled_rate_ = 44100.0 * kOversamplingFactor;

    // Controls.
    double input_gain_db_ = 0.0;
    double output_gain_db_ = 0.0;
    double attack_us_ = 200.0;
    double release_ms_ = 300.0;
    double knee_request_db_ = 1.0;
    double transformer_amount_ = 0.6;
    double mix_ = 1.0;
    Ratio ratio_button_ = Ratio::r4_1;

    // Derived from controls.
    double input_gain_ = 1.0;
    double output_gain_ = 1.0;
    double ratio_value_ = 4.0;
    double knee_db_ = 1.0;
    double bias_shift_db_ = 0.0;
    double alpha_max_ = kColorationAlphaMax;
    double attack_coef_ = 0.0;
    double release_coef_ = 0.0;

    // Derived from the divider's design parameters.
    double g_min_ = 0.0;
    double g_max_ = 0.0;
    double series_resistance_ = 0.0;
    double trim_ = 1.0;

    // Resampler.
    std::array<double, kResamplerTaps> decimation_{};
    std::array<std::array<double, kBaseHistory>, kOversamplingFactor> interpolation_{};
    std::array<int, kOversamplingFactor> interpolation_length_{};
    double resampler_peak_bound_ = 1.0;

    // Running state.
    std::array<double, kBaseHistory> base_history_{};
    std::array<double, kResamplerTaps> os_history_{};
    std::array<double, kLatencySamples + 1> dry_delay_{};
    int base_write_ = 0;
    int os_write_ = 0;
    std::size_t dry_write_ = 0;

    double detector_db_ = 0.0;
    double control_ = 0.0;
    double feedback_ = 0.0;
    BiquadT<double> transformer_{};
};

using FetCompressor = FetCompressorT<float>;
using FetCompressor64 = FetCompressorT<double>;

}  // namespace pulp::signal
