#pragma once

/// @file frequency_shifter_ssb.hpp
/// The single-sideband (Bode/Moog) frequency shifter: a Hilbert
/// phase-difference network, an exact quadrature carrier, an SSB combine, and a
/// barberpole feedback loop.
///
/// A frequency shifter ADDS a constant number of hertz to every component of a
/// signal — `f → f + Δf` — where a pitch shifter MULTIPLIES by a ratio. That is
/// the entire point of the effect and it is asserted, not asserted-about: a
/// 440 Hz fundamental with an 880 Hz octave partial, shifted by +5 Hz, becomes
/// 445 and 885 Hz, whose ratio is 1.989 rather than 2. The harmonic series is
/// destroyed on purpose; that inharmonicity is the sound, and it is why a bank
/// of tuned resonators becomes a cymbal under a few tens of hertz of shift.
///
/// ## Why a second header rather than an edit to `frequency_shifter.hpp`
///
/// `frequency_shifter.hpp` already carried a 159-line prototype of this idea,
/// and this file does not delete it, because `drum/cymbal.hpp` composes its
/// `FrequencyShifterT` by name. What this file does instead is take ownership
/// of the *math*: the prototype's private Hilbert network and its private
/// coefficient table are gone, and it now composes
/// `HilbertQuadratureNetworkT` and the combine from here. There is exactly one
/// quadrature network and one coefficient table in the tree, and it is this
/// one. Two things were wrong in the prototype's own copy, both of which the
/// acceptance suite here now covers:
///
///   - Its `reset()` assigned a value-initialised section over each live
///     section, which zeroed the section COEFFICIENTS along with the state.
///     Every caller resets, so the shipped network was in practice a chain of
///     two-sample delays and the "shifter" was a ring modulator emitting BOTH
///     sidebands. Four behavioural tests passed over it, because suppressing
///     the carrier — which a ring modulator also does — was as far as they
///     looked. The lesson is structural, not clerical: the coefficients here
///     live in a `static constexpr` table and never in per-section state, so
///     no `reset()` can lose them.
///   - Its in-phase branch was the DELAYED one, which puts the quadrature
///     branch a quarter cycle AHEAD rather than behind, and inverts the
///     sideband the combine keeps. `set_shift_hz(+250)` moved a 1 kHz tone to
///     750 Hz. The assignment below is the other one; see the network's own
///     doc block for why only two of the four I/Q assignments are even
///     constant-phase.
///
/// ## Architecture
///
/// ```
///          ┌───────────────────────── barberpole feedback ──────────────┐
///          │                                                            │
///  x ─▶ (+) ─▶ DcBlocker ─▶ Hilbert network ─┬─▶ I ─▶(×cos φ)─┐         │
///        ▲                                   └─▶ Q ─▶(×sin φ)─┤         │
///        │                     carrier φ += 2π·Δf/fs ─────────┤         │
///        │                                          up:  I·c − Q·s      │
///        │                                        down:  I·c + Q·s      │
///        └──────── ×g ◀── delay_line(fb_delay_ms) ◀────────── wet ───────┘
///                                                             │
///                                        dry/wet mix ◀────────┴──▶ y
/// ```
///
/// The DC blocker sits between the feedback sum and the network so ONE
/// instance protects both paths: a DC component reaching the combine would be
/// "shifted from 0 Hz" and emerge as a bare tone at the shift frequency, and
/// anything the recirculation path introduces gets swept up on the next pass
/// rather than accumulating. The dry/wet mix sits after the feedback tap, so
/// turning `mix` down attenuates the barberpole without disabling it.
///
/// ## The four contracts this block keeps
///
/// **The carrier is exact.** `cos φ` and `sin φ` are evaluated from the SAME
/// accumulated phase every sample. A recursive resonator is forbidden here:
/// its slow amplitude and phase drift is inaudible in a tremolo and fatal in a
/// carrier, because every microradian of carrier phase error subtracts
/// directly from image rejection. This is the discipline
/// `LfoT::next_quadrature` documents, and this module would compose it
/// literally except that `LfoT` clamps its rate to `LfoT::kMaxRateHz` = 200 Hz
/// — two orders of magnitude below this module's ±5 kHz. So the accumulator
/// underneath it (`osc::PhaseAccumulator`) is composed directly and the
/// exactness contract is kept identically: advance, then evaluate both
/// trigonometric functions at the advanced phase.
///
/// **Image rejection is a measured property of the shipped table, not a
/// claim.** For an exactly-allpass quadrature pair with phase error `θ` from
/// the ideal quarter cycle, the retained sideband has amplitude `|cos(θ/2)|`
/// and the unwanted one `|sin(θ/2)|`, so rejection is `−20·log10|tan(θ/2)|`:
/// 40 dB needs `θ ≤ 0.0200 rad`, 60 dB needs `θ ≤ 0.0020 rad`. The shipped
/// four-section table holds `θ ≤ 0.0200 rad` — hence `kImageRejectDb` = 40 dB —
/// over the NORMALISED band `[kBandEdgeNormalized, 0.5 − kBandEdgeNormalized]`
/// of the sample rate; the measured floor across that band is 44.2 dB, at
/// `f/fs` = 0.00128.
///
/// **The band is normalised, and that is not a dodge.** A fixed allpass table
/// is a function of `ω = 2π f / fs` alone, so its band edges are fixed
/// FRACTIONS of the sample rate and its edge in hertz necessarily rises with
/// `fs`: 20.3 Hz at 44.1 kHz, 22.1 Hz at 48 kHz, 44.2 Hz at 96 kHz, 88.3 Hz at
/// 192 kHz. There is no coefficient set that pins an absolute 20 Hz edge at
/// every rate, and quoting one would be quoting a number true only at the rate
/// it was measured at. Below the edge the rejection degrades gracefully rather
/// than failing — 36.9 dB at 20 Hz / 48 kHz — and above the upper edge it is
/// the mirror of the same curve.
///
/// **Small-signal gain and the loop bound** (series laws 1 and 8). Every
/// element in the feedback loop is either exactly allpass (both Hilbert
/// branches, whose magnitude is 1 identically, not approximately), a convex
/// interpolation (the delay line), or the combine, whose retained-sideband
/// gain `|cos(θ/2)|` is bounded ABOVE by 1. The single element that can exceed
/// unity is the DC blocker, whose supremum is `2/(1 + p)` at Nyquist —
/// 1.00033 at the shipped corner and sample rate. `kGshiftBudget` = 1.02 is
/// the headroom the registry pays for that, so per-pass loop gain is at most
/// `kMaxFeedback · kGshiftBudget` = 0.918 and the steady-state envelope is
/// `1/(1 − 0.918)` ≈ 12.2×. The acceptance suite measures the actual retained
/// gain and the actual loop-impulse peak against that envelope rather than
/// trusting the arithmetic.
///
/// ## Anti-aliasing policy: NOT APPLICABLE, and stated so deliberately
///
/// Series law 4 asks every module to state one. Frequency shifting is a LINEAR
/// time-varying operation: it generates no harmonics, so there is nothing to
/// oversample. Content shifted past `fs/2` folds back, and content shifted
/// below 0 Hz reflects to positive frequency with its sideband inverted — but
/// that is the shifted spectrum itself crossing a band edge, not a distortion
/// product, and oversampling would only move the edge, not remove it. The
/// Bode and Moog hardware did the same thing. It is documented, bounded by
/// `kMaxShiftHz`, and left alone.
///
/// ## Determinism
///
/// There is no randomness anywhere in this module — no generator to seed, no
/// dither, no noise floor. A render from `reset()` is bit-identical for
/// identical (parameters, input), per series law 2.
///
/// RT contract: `prepare(sample_rate)` sizes the feedback delay lines and MAY
/// allocate. `set_*`, `process()`, `process_stereo()` and `reset()` never
/// allocate, never lock, never perform I/O, and are safe per sample on the
/// audio thread. State is POD; a zero-initialised instance is a valid silent
/// one at `shift = 0`, `feedback = 0`, fully wet. `process()` costs eight
/// second-order allpass sections at two multiplies each, one `sin`/`cos` pair,
/// and one interpolated delay read; `process_stereo()` costs a second set of
/// sections and delay read but reuses the one carrier.
///
/// References: H. Bode and R. A. Moog, "A High-Accuracy Frequency Shifter for
/// Professional Audio Applications", JAES 20(6), pp. 453–458, 1972 (the
/// professional SSB shifter concept). D. K. Weaver, "A Third Method of
/// Generation and Detection of Single-Sideband Signals", Proc. IRE 44(12),
/// 1956 (the low-IF alternative, cited for lineage; not built — the
/// Hartley/Hilbert path needs one wide-band network and no baseband filter
/// bank). R. Ansari, "IIR Discrete-Time Hilbert Transformers", IEEE Trans.
/// ASSP 35(8), pp. 1116–1119, 1987 (the polyphase-halfband → IIR Hilbert
/// design procedure). O. Niemitalo, "Hilbert transform", yehar.com (the
/// public two-cascade eight-multiply allpass form and the
/// differential-evolution minimax coefficient set this table follows).

#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// A pair of allpass cascades whose outputs stand a quarter cycle apart across
/// nearly the whole band — the analytic-signal splitter an SSB shifter needs.
///
/// Neither output is the input. Both branches are allpass, so both preserve
/// the input's magnitude spectrum exactly and move only its phase; what
/// carries information is the DIFFERENCE between them. A single filter cannot
/// produce that, which is why the structure is a pair.
///
/// Each section is second-order with one coefficient:
///
/// ```
///            c − z⁻²                                        c = a²
/// A(z) = ─────────────── ,  poles at z = ±√c ,  |A(e^{jω})| = 1
///          1 − c·z⁻²
/// ```
///
/// **This is not the section form a polyphase half-band decimator uses**, and
/// the difference is one sign, so it is worth stating before someone
/// "corrects" it. The half-band form is `(c + z⁻²)/(1 + c·z⁻²)`, with poles at
/// `±j√c`. Fed the SAME coefficient table, that form produces a branch pair
/// whose phase difference is 0 below `fs/4` and π above it — a half-band
/// filter, not a quadrature pair. The two forms are related by `z → jz`, which
/// is the half-band-to-Hilbert rotation, and that identity is precisely why
/// one published table serves both uses. Ship the wrong sign and the module
/// still runs, still sounds like something, and rejects no image at all.
///
/// Both branches are cascades of `kSections` such sections and the QUADRATURE
/// branch carries an additional `z⁻¹`. Of the four ways to assign
/// (delayed, undelayed) × (table A, table B) to (I, Q), only two give a phase
/// difference that is constant across the band rather than one that drifts
/// with `ω`; of those two, this one leaves the IN-PHASE branch undelayed, so
/// the module's impulse response is non-zero at sample 0 and
/// `latency_samples()` is honestly 0 (the other choice puts a bulk sample of
/// delay on `I` and would have to report 1).
///
/// Stability is structural, not a property of the fitted values: `|z| = √c < 1`
/// for any `0 < c < 1`, so any coefficient search can be re-run without ever
/// risking an unstable filter. What a coefficient search CAN get wrong is the
/// image rejection, which is why that — and not the digits — is what the
/// acceptance suite asserts.
///
/// RT contract: every member allocates nothing, takes no locks, and is safe
/// per sample on the audio thread.
template <typename SampleType = double>
class HilbertQuadratureNetworkT {
public:
    /// Sections per branch.
    /// [design parameter] default 4, range 3 .. 6. Four is Niemitalo's
    /// published operating point and measures ≈ 44 dB of broadband rejection
    /// here; the range exists so the table can be regenerated deeper without a
    /// structural change if a future spec wants a tighter floor.
    static constexpr std::size_t kSections = 4;

    /// Pole magnitudes of the in-phase (undelayed) branch.
    /// [design parameter] — a calibration table, validated by the acceptance
    /// suite's measured image rejection rather than by inspecting digits.
    /// Reached from the published minimax design cited in the file doc block;
    /// the section coefficient is the SQUARE of each entry.
    static constexpr std::array<double, kSections> kInPhasePoles = {
        0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};

    /// Pole magnitudes of the quadrature (one-sample-delayed) branch.
    /// [design parameter] — same provenance and same validation as above.
    static constexpr std::array<double, kSections> kQuadraturePoles = {
        0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};

    /// The lower edge of the design band, as a fraction of the sample rate.
    /// A fixed allpass table is a normalised-frequency design, so this is the
    /// only form in which the edge is a constant at all (see the file doc
    /// block). Measured: `kImageRejectDb` is met on
    /// `[kBandEdgeNormalized·fs, (0.5 − kBandEdgeNormalized)·fs]`, which is
    /// 20.3 Hz .. 22.08 kHz at 44.1 kHz and 22.1 Hz .. 23.98 kHz at 48 kHz.
    /// The 40 dB crossover itself is at 4.34e-4; this sits a little above it so
    /// the claim carries margin rather than sitting on the knife edge.
    static constexpr double kBandEdgeNormalized = 4.6e-4;

    /// The rejection floor the shipped table holds across the design band.
    /// [design parameter] default 40 dB, range 30 .. 60 dB — the range is the
    /// span a regenerated table at `kSections` 3 .. 6 can reach; 40 dB is what
    /// four sections deliver.
    static constexpr double kImageRejectDb = 40.0;

    struct Outputs {
        SampleType in_phase = 0;    ///< the reference branch, no bulk delay
        SampleType quadrature = 0;  ///< a quarter cycle behind it
    };

    /// Clears state. Cannot clear coefficients — there are none to clear;
    /// they are compile-time constants read directly by `process()`.
    void reset() {
        in_phase_state_ = {};
        quadrature_state_ = {};
        delayed_ = 0;
    }

    Outputs process(SampleType x) {
        const SampleType in_phase = cascade(x, kInPhasePoles, in_phase_state_);
        const SampleType quadrature = cascade(x, kQuadraturePoles, quadrature_state_);
        const Outputs out{in_phase, delayed_};
        delayed_ = quadrature;
        return out;
    }

    /// Lower edge of the design band in Hz at a given rate. Computed from the
    /// shipped constant, never restated.
    static constexpr double band_low_hz(double sample_rate) {
        return kBandEdgeNormalized * sample_rate;
    }

    /// Upper edge of the design band in Hz at a given rate.
    static constexpr double band_high_hz(double sample_rate) {
        return (0.5 - kBandEdgeNormalized) * sample_rate;
    }

private:
    /// Two state words per section, which is all a second-order allpass needs.
    /// Transposed direct form II for `(c − z⁻²)/(1 − c·z⁻²)` reduces to
    /// `y = c·x + s1;  s1 ← s2;  s2 ← c·y − x`, and expanding it two samples
    /// back recovers `y[n] = c·(x[n] + y[n−2]) − x[n−2]` exactly.
    struct SectionState {
        SampleType s1 = 0;
        SampleType s2 = 0;
    };

    using BranchState = std::array<SectionState, kSections>;

    static SampleType cascade(SampleType x, const std::array<double, kSections>& poles,
                              BranchState& state) {
        SampleType v = x;
        for (std::size_t k = 0; k < kSections; ++k) {
            const auto c = static_cast<SampleType>(poles[k] * poles[k]);
            const SampleType y = c * v + state[k].s1;
            state[k].s1 = state[k].s2;
            // Snap the recursive state: the top sections' poles sit at
            // |z| = 0.9994, so a decaying tail stalls in denormals for a long
            // time with no flush-to-zero guard. No-op above 1e-15.
            state[k].s2 = snap_to_zero(c * y - v);
            v = y;
        }
        return v;
    }

    BranchState in_phase_state_{};
    BranchState quadrature_state_{};
    SampleType delayed_ = 0;
};

/// Which sideband a `SsbFrequencyShifterT` keeps, and how it spreads it.
enum class FrequencyShiftMode : std::uint8_t {
    up,            ///< Upper sideband on both channels: `f → f + Δf`.
    down,          ///< Lower sideband on both channels: `f → f − Δf`.
    dual_mono,     ///< Two independent identical mono shifters. See the note below.
    stereo_split,  ///< Up on the left, down on the right, scaled by the spread.
};

/// The single-sideband frequency shifter.
///
/// `up` and `down` are mirror images of one signed control: because `Δf` is
/// signed, `down` at `Δf` is `up` at `−Δf`, and both are reachable from either.
/// They are both present because a host-facing enum that names the sideband
/// reads better than one that asks a user to negate a knob, and `dual_mono` is
/// present for the same reason — it is `up`'s behaviour under a name that says
/// "deliberately no stereo differentiation", so a preset can distinguish "I
/// want both channels the same" from "I have not thought about stereo yet".
/// Only `stereo_split` is a different topology.
template <typename SampleType = float,
          FastTrigProfile TrigProfile = FastTrigProfile::reference>
class SsbFrequencyShifterT {
public:
    using Mode = FrequencyShiftMode;
    using Network = HilbertQuadratureNetworkT<double>;
    static_assert(TrigProfile == FastTrigProfile::reference ||
                  TrigProfile == FastTrigProfile::realtime_precise);
    static constexpr FastTrigProfile kTrigProfile = TrigProfile;

    // ── Design parameters (the complete roster; see §11 of the spec) ───────

    /// Shift magnitude ceiling, in Hz.
    /// [design parameter] default 5000 Hz, range 1000 .. 20000 Hz. 5 kHz
    /// covers musical clangor without inviting whole-band fold-over.
    static constexpr double kMaxShiftHz = 5000.0;

    /// Top of the taper's linear fine zone, in Hz — the region where detune
    /// and barberpole live and where a 1 Hz taper error is audible as pitch
    /// instability. [design parameter] default 20 Hz, range 5 .. 50 Hz.
    static constexpr double kLinZoneHz = 20.0;

    /// Knob position where the taper crosses from linear to logarithmic.
    /// [design parameter] default 0.5, range 0.3 .. 0.7.
    static constexpr double kSplit = 0.5;

    /// Feedback ceiling. [design parameter] default 0.90, range 0.5 .. 0.95 —
    /// the cap the loop-gain arithmetic in the file doc block is stated at.
    static constexpr double kMaxFeedback = 0.90;

    /// Feedback-loop delay floor, in ms. Numerical, not musical: the loop's
    /// log taper divides by it, and `log(0)` is undefined, so the knob's
    /// bottom position resolves here. Any value far below the shortest
    /// comb-relevant delay works. [design parameter] default 0.1 ms,
    /// range 0.01 .. 1 ms.
    static constexpr double kMinDelayMs = 0.1;

    /// Feedback-loop delay ceiling, in ms. Bounds the barberpole comb spacing
    /// without letting the loop stray into audible slapback.
    /// [design parameter] default 50 ms, range 10 .. 100 ms.
    static constexpr double kMaxLoopMs = 50.0;

    /// Allowed headroom above ideal unity for the retained sideband, which the
    /// registry's `worst_case_gain` is computed from. The combine itself
    /// cannot exceed 1 (see the file doc block); what this actually pays for
    /// is the DC blocker's `2/(1 + p)` supremum, plus margin.
    /// [design parameter] default 1.02, range 1.00 .. 1.05.
    static constexpr double kGshiftBudget = 1.02;

    /// DC blocker corner. High enough to settle a DC step in a fraction of a
    /// second, low enough to leave the bottom of the audio band alone.
    /// [design parameter] default 5 Hz, range 1 .. 20 Hz.
    static constexpr double kDcCornerHz = 5.0;

    /// De-zippering time for `shift_hz`. Long enough that a jumped automation
    /// value glides rather than clicks, short enough that a deliberate sweep
    /// still tracks. [design parameter] default 20 ms, range 1 .. 100 ms.
    static constexpr double kShiftSmoothingMs = 20.0;

    /// De-zippering time for `feedback`, `mix` and `stereo_spread` — the gain
    /// controls. Smoothing feedback also keeps a fast automation move from
    /// stepping the loop into a transient overshoot.
    /// [design parameter] default 20 ms, range 1 .. 100 ms.
    static constexpr double kGainSmoothingMs = 20.0;

    /// De-zippering time for the loop delay length. Longer than the gains
    /// because moving a delay tap resamples what is already in the line, and a
    /// slower move turns that from a chirp into a glide.
    /// [design parameter] default 50 ms, range 10 .. 500 ms.
    static constexpr double kDelaySmoothingMs = 50.0;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Sizes the two feedback delay lines for `kMaxLoopMs` at this rate and
    /// recomputes the DC-blocker pole and every smoothing increment. May
    /// allocate; nothing else here does.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;

        const int max_delay = std::max(
            2, static_cast<int>(std::ceil(units::ms_to_samples(kMaxLoopMs, sample_rate_))) + 2);
        for (auto& line : delay_) line.prepare(max_delay);
        max_delay_samples_ = static_cast<double>(max_delay);

        // H(z) = (1 − z⁻¹)/(1 − p·z⁻¹) has its corner where the pole sits;
        // `exp(−2π·fc/fs)` places it at `kDcCornerHz` at any sample rate,
        // which the fixed 0.995 default cannot.
        const auto pole = static_cast<double>(std::exp(-2.0 * kPi * kDcCornerHz / sample_rate_));
        for (auto& dc : dc_blocker_) dc.set_pole(pole);

        const auto sr = static_cast<double>(sample_rate_);
        shift_smoother_.set_ramp_time(kShiftSmoothingMs * 0.001, sr);
        feedback_smoother_.set_ramp_time(kGainSmoothingMs * 0.001, sr);
        mix_smoother_.set_ramp_time(kGainSmoothingMs * 0.001, sr);
        spread_smoother_.set_ramp_time(kGainSmoothingMs * 0.001, sr);
        delay_smoother_.set_ramp_time(kDelaySmoothingMs * 0.001, sr);

        update_delay_target();
        reset();
    }

    /// Clears every filter, line, accumulator and smoother, and snaps each
    /// smoother onto its target so a render from here is reproducible to the
    /// bit. Allocates nothing.
    void reset() {
        for (auto& n : network_) n.reset();
        for (auto& dc : dc_blocker_) dc.reset();
        for (auto& line : delay_) line.reset();
        carrier_.reset(0.0);
        shift_smoother_.set_immediate(shift_hz_);
        feedback_smoother_.set_immediate(feedback_);
        mix_smoother_.set_immediate(mix_);
        spread_smoother_.set_immediate(spread_);
        delay_smoother_.set_immediate(delay_target_samples_);
    }

    /// Audio-thread fault recovery: identical running state to `reset()` but
    /// logically invalidates the delay history instead of clearing its storage.
    void discard_history() noexcept {
        for (auto& n : network_) n.reset();
        for (auto& dc : dc_blocker_) dc.reset();
        for (auto& line : delay_) line.discard_history();
        carrier_.reset(0.0);
        shift_smoother_.set_immediate(shift_hz_);
        feedback_smoother_.set_immediate(feedback_);
        mix_smoother_.set_immediate(mix_);
        spread_smoother_.set_immediate(spread_);
        delay_smoother_.set_immediate(delay_target_samples_);
    }

    // ── Parameters (real units throughout, series law 3) ──────────────────

    /// Signed shift in Hz: positive shifts the retained sideband up, negative
    /// down. Clamped to ±`kMaxShiftHz`.
    ///
    /// Smoothed LINEARLY, not geometrically, and that is the correct law here
    /// rather than a shortcut. `LogRampedValueT` is the house smoother for
    /// pitch and frequency because those are heard as ratios — but it requires
    /// both endpoints strictly positive, and `Δf` is signed and passes through
    /// zero every time a sweep reverses direction. It is also the wrong shape:
    /// a frequency SHIFT is additive by definition, so equal time should give
    /// equal hertz.
    void set_shift_hz(double hz) {
        if (!std::isfinite(hz)) return;
        shift_hz_ = std::clamp(hz, -kMaxShiftHz, kMaxShiftHz);
        shift_smoother_.set_target(shift_hz_);
    }

    double shift_hz() const { return shift_hz_; }

    /// Barberpole feedback depth, in the range [0, kMaxFeedback].
    void set_feedback(double g) {
        if (!std::isfinite(g)) return;
        feedback_ = std::clamp(g, 0.0, kMaxFeedback);
        feedback_smoother_.set_target(feedback_);
    }

    double feedback() const { return feedback_; }

    /// Feedback-loop delay in ms, in the range [kMinDelayMs, kMaxLoopMs]. Sets the comb
    /// spacing that colours the spiral: short reads as a continuous glide,
    /// long as individually audible passes.
    void set_feedback_delay_ms(double ms) {
        if (!std::isfinite(ms)) return;
        feedback_delay_ms_ = std::clamp(ms, kMinDelayMs, kMaxLoopMs);
        update_delay_target();
        delay_smoother_.set_target(delay_target_samples_);
    }

    double feedback_delay_ms() const { return feedback_delay_ms_; }

    void set_mode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }

    /// Dry/wet, `0 .. 1`.
    void set_mix(double wet01) {
        if (!std::isfinite(wet01)) return;
        mix_ = std::clamp(wet01, 0.0, 1.0);
        mix_smoother_.set_target(mix_);
    }

    /// `stereo_split` only: scales how far apart the two channels are driven.
    /// At 1 the channels move by `+|Δf|` and `−|Δf|`; at 0.5 by `±|Δf|/2`; at 0
    /// neither moves and both carry the (allpass, full-magnitude) input.
    ///
    /// It scales the CARRIER FREQUENCY, not the quadrature term's depth, and
    /// the difference is not cosmetic. Scaling the quadrature term — `L = I·c
    /// − s·Q·s`, which is how the spec's §7 formula reads — is not a partial
    /// shift at all: at `s = 0.5` it produces 75 % of the upper sideband plus
    /// 25 % of the lower, two tones 10 Hz apart, and at `s = 0` it produces an
    /// even split of both, which is ring modulation. Neither has any energy at
    /// the frequency the spec's own worked table says that setting should
    /// produce. Scaling the carrier gives exactly that table: one tone per
    /// channel at `f ± spread·Δf`, and at spread 0 the carrier stops and both
    /// channels pass the input at full magnitude.
    void set_stereo_spread(double spread01) {
        if (!std::isfinite(spread01)) return;
        spread_ = std::clamp(spread01, 0.0, 1.0);
        spread_smoother_.set_target(spread_);
    }

    // ── Processing ────────────────────────────────────────────────────────

    /// One mono sample. In `stereo_split` this returns the left (upper
    /// sideband) leg, spread and all — there is no second channel to put the
    /// other one on.
    SampleType process(SampleType x) {
        if (!std::isfinite(static_cast<double>(x))) {
            discard_history();
            return SampleType{0};
        }
        const Controls c = advance_controls();
        const double dry = static_cast<double>(x);
        const double wet = shift_channel(0, dry, c, /*left=*/true);
        return static_cast<SampleType>(snap_to_zero(dry + c.mix * (wet - dry)));
    }

    /// One stereo frame. Both channels share the carrier — one accumulator,
    /// one `sin`/`cos` pair — and each has its own Hilbert network, DC blocker
    /// and feedback line, so stereo content is preserved rather than summed.
    void process_stereo(SampleType& left, SampleType& right) {
        if (!std::isfinite(static_cast<double>(left)) ||
            !std::isfinite(static_cast<double>(right))) {
            discard_history();
            left = right = SampleType{0};
            return;
        }
        const Controls c = advance_controls();
        const double dry_l = static_cast<double>(left);
        const double dry_r = static_cast<double>(right);
        const double wet_l = shift_channel(0, dry_l, c, /*left=*/true);
        const double wet_r = shift_channel(1, dry_r, c, /*left=*/false);
        left = static_cast<SampleType>(snap_to_zero(dry_l + c.mix * (wet_l - dry_l)));
        right = static_cast<SampleType>(snap_to_zero(dry_r + c.mix * (wet_r - dry_r)));
    }

    /// Zero, and reported exactly rather than estimated (series law 5). The
    /// network is IIR allpass: there is no bulk delay on the in-phase branch,
    /// and the impulse response is non-zero at sample 0. Its group delay IS
    /// frequency-dependent and a couple of samples in places — that is
    /// dispersion, not latency, and a host cannot compensate for it with a
    /// delay, so reporting it would misinform rather than inform. This is also
    /// why the network is IIR at all: a linear-phase FIR Hilbert would cost
    /// `(taps−1)/2` samples and inflate the feedback loop by the same amount.
    int latency_samples() const noexcept { return 0; }

    // ── Registry and taper helpers (pure functions of shipped constants) ───

    /// The bound Forge's registry cites (series law 8): the steady-state
    /// envelope of a comb-like loop at the feedback ceiling,
    /// `1/(1 − kMaxFeedback·kGshiftBudget)`. The acceptance suite measures
    /// both factors and the resulting peak rather than trusting this.
    ///
    /// It is deliberately the SAFE envelope, not the observed peak: a shift
    /// decorrelates recirculated energy (nothing lands twice at the same
    /// frequency, unlike a comb filter), so the measured peak sits well under
    /// it. Registering the observed number would be registering a bound that
    /// happens to hold for the programme material it was measured on.
    static constexpr double worst_case_gain() {
        return 1.0 / (1.0 - kMaxFeedback * kGshiftBudget);
    }

    /// The signed piecewise taper: a linear fine zone out to `kLinZoneHz`,
    /// geometric beyond it to `kMaxShiftHz`, mirrored about zero. Both
    /// branches meet exactly at `kSplit` (the log branch's exponent is 0
    /// there, and `x⁰ = 1` needs no `pow`), and both are strictly increasing,
    /// so an automation sweep finds no plateau or reversal to snag on.
    static double shift_hz_from_knob(double knob) {
        const double clamped = std::clamp(knob, -1.0, 1.0);
        const double m = std::abs(clamped);
        const double magnitude =
            m < kSplit ? (m / kSplit) * kLinZoneHz
                       : units::taper_log((m - kSplit) / (1.0 - kSplit), kLinZoneHz, kMaxShiftHz);
        return clamped < 0.0 ? -magnitude : magnitude;
    }

    /// Inverse of `shift_hz_from_knob`.
    static double knob_from_shift_hz(double hz) {
        const double clamped = std::clamp(hz, -kMaxShiftHz, kMaxShiftHz);
        const double m = std::abs(clamped);
        const double position =
            m < kLinZoneHz
                ? (m / kLinZoneHz) * kSplit
                : kSplit + (1.0 - kSplit) * units::untaper_log(m, kLinZoneHz, kMaxShiftHz);
        return clamped < 0.0 ? -position : position;
    }

    /// The loop-delay taper. Geometric, because comb pitch is heard
    /// geometrically: halving the delay raises the comb's fundamental by an
    /// octave, so equal knob travel should buy equal intervals.
    static double feedback_delay_ms_from_knob(double u) {
        return units::taper_log(std::clamp(u, 0.0, 1.0), kMinDelayMs, kMaxLoopMs);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    /// Everything the two channels share for one frame, sampled once so a
    /// stereo frame advances each smoother and the carrier exactly once.
    struct Controls {
        double cosine = 1.0;
        double sine = 0.0;
        double feedback = 0.0;
        double delay_samples = 0.0;
        double mix = 1.0;
        double spread = 1.0;
    };

    Controls advance_controls() {
        Controls c;
        c.feedback = feedback_smoother_.next();
        c.delay_samples = delay_smoother_.next();
        c.mix = mix_smoother_.next();
        c.spread = spread_smoother_.next();

        // In `stereo_split` the spread scales the carrier itself, so the two
        // legs land at `f ± spread·Δf`. Both legs read the SAME accumulator:
        // the right channel's carrier is the left's conjugate, which is the
        // same cosine with the sine negated, so one phase serves both.
        const double shift = shift_smoother_.next() *
                             (mode_ == Mode::stereo_split ? c.spread : 1.0);

        // Advance FIRST, then evaluate both functions at the advanced phase —
        // `LfoT::next_quadrature`'s order, and the reason an impulse produces a
        // non-zero sample 0: at `n = 0` the carrier is already off zero, so the
        // quadrature term contributes immediately.
        carrier_.advance(shift / sample_rate_);
        if constexpr (TrigProfile == FastTrigProfile::realtime_precise) {
            const auto pair = FastMath::sincos_cycles_precise(carrier_.phase());
            c.cosine = pair.cosine;
            c.sine = pair.sine;
        } else {
            const double radians = kTwoPi * carrier_.phase();
            c.cosine = std::cos(radians);
            c.sine = std::sin(radians);
        }
        return c;
    }

    /// One channel of the shifter: read the loop, block DC, split, combine,
    /// write the loop back.
    double shift_channel(std::size_t channel, double input, const Controls& c, bool left) {
        // Read BEFORE the write, so the shortest realisable round trip is one
        // sample and the loop is never algebraic. `read()` looks back
        // `delay + 1` samples from the write cursor, so asking for
        // `delay_samples − 1` makes the total round trip exactly
        // `delay_samples` — with a floor of one sample.
        const double read_position = std::clamp(c.delay_samples - 1.0, 0.0, max_delay_samples_);
        const double recirculated = delay_[channel].read(read_position);

        const double driven = dc_blocker_[channel].process(input + c.feedback * recirculated);
        const auto pair = network_[channel].process(driven);

        // `up` keeps `f + Δf`, `down` keeps `f − Δf`. In `stereo_split` the
        // left leg is the up combine and the right the down one — exactly the
        // up/down pair run in parallel, over a carrier the spread has already
        // scaled.
        double quadrature_gain = 0.0;
        switch (mode_) {
            case Mode::up:
            case Mode::dual_mono:
                quadrature_gain = -1.0;
                break;
            case Mode::down:
                quadrature_gain = 1.0;
                break;
            case Mode::stereo_split:
                quadrature_gain = left ? -1.0 : 1.0;
                break;
        }

        const double wet = snap_to_zero(pair.in_phase * c.cosine +
                                        quadrature_gain * pair.quadrature * c.sine);
        delay_[channel].push(wet);
        return wet;
    }

    void update_delay_target() {
        const double samples = units::ms_to_samples(feedback_delay_ms_, sample_rate_);
        delay_target_samples_ = std::clamp(samples, 1.0, max_delay_samples_);
    }

    double sample_rate_ = 44100.0;
    double max_delay_samples_ = 1.0;

    double shift_hz_ = 0.0;
    double feedback_ = 0.0;
    double feedback_delay_ms_ = 8.0;
    double mix_ = 1.0;
    double spread_ = 1.0;
    double delay_target_samples_ = 1.0;
    Mode mode_ = Mode::up;
    std::array<Network, 2> network_{};
    std::array<DcBlocker<double>, 2> dc_blocker_{};
    std::array<DelayLineT<double>, 2> delay_{};

    osc::PhaseAccumulator carrier_{};

    SmoothedValue<double> shift_smoother_{};
    SmoothedValue<double> feedback_smoother_{};
    SmoothedValue<double> mix_smoother_{1.0};
    SmoothedValue<double> spread_smoother_{1.0};
    SmoothedValue<double> delay_smoother_{1.0};
};

using HilbertQuadratureNetwork = HilbertQuadratureNetworkT<double>;
using SsbFrequencyShifter = SsbFrequencyShifterT<float>;
using SsbFrequencyShifter64 = SsbFrequencyShifterT<double>;
using PreciseSsbFrequencyShifter =
    SsbFrequencyShifterT<float, FastTrigProfile::realtime_precise>;
using PreciseSsbFrequencyShifter64 =
    SsbFrequencyShifterT<double, FastTrigProfile::realtime_precise>;

}  // namespace pulp::signal
