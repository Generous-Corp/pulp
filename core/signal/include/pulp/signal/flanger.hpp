#pragma once

/// @file flanger.hpp
/// The flanger — one comb, three ways of moving it.
///
/// A flanger is a comb filter: a short delay (0.1–10 ms, short enough that the
/// comb's teeth sit at audible, widely-spaced frequencies rather than the dense
/// comb of a chorus's 10–35 ms line) summed with the dry signal, its delay time
/// swept so the whole notch pattern sweeps with it, plus feedback that turns
/// the comb's shallow notches into sharp resonant peaks. The single fact that
/// governs everything here is
///
/// ```
///   notch spacing  Δf = 1/D          notches at  f(k) = (2k+1)/(2D)
/// ```
///
/// (Dattorro 1997 Part 2). Every notch is an artifact of the SAME delay, so
/// they all move in lockstep — that is why a flanger sweep reads as one moving
/// comb, and why a phaser (independent allpass stages, independently placed
/// notches) sounds different even though both are swept notch effects.
///
/// ## Three modes, one engine
///
///   * **Classic** — one modulated delay, feedback, dry + wet. The jet-plane
///     whoosh. `D(t) = center ± depth·LFO(t)`, dry undelayed, latency 0.
///   * **Through-zero** — the tape-studio original. The DRY path carries a
///     fixed delay the swept wet path can cross: `D_wet` sweeps
///     `0 .. 2·offset` and passes exactly through the dry path's delay twice
///     per LFO cycle. At the crossing the two paths hold the identical signal,
///     and the difference of them is silence — a full-band null no
///     single-delay topology can produce, because a delay line cannot run
///     backwards. The fixed dry delay, not a negative wet delay, is what makes
///     "through zero" representable.
///   * **Barberpole** — a comb whose notches climb (or fall) forever without
///     resetting. The delay is FIXED and the wet path is frequency-SHIFTED by
///     a few Hz instead of being swept (Esqueda, Välimäki & Parker, DAFx-15).
///     Each pass through the feedback loop adds another `Δf`, so the pattern
///     drifts monotonically and wraps — a Shepard-tone illusion in the
///     frequency domain rather than a sweep that turns around.
///
/// ## The one polarity convention, applied everywhere
///
/// `polarity` is the SIGN OF THE WET CONTRIBUTION, and it is the same sign in
/// every mode and in both the mix and the feedback tap:
///
/// ```
///   positive:  y = dry + wet   → notches at (2k+1)/(2D); TZ crossing DOUBLES
///   negative:  y = dry − wet   → notches at k/D, including DC; TZ crossing NULLS
/// ```
///
/// Both halves fall out of the same algebra: `|1 ± e^{−jωD}|²` is
/// `2 ± 2cos(ωD)`, whose zeros are the two interleaved series above. It also
/// settles which polarity nulls a through-zero crossing, which is not a matter
/// of taste: at the crossing the two paths carry the identical signal, so their
/// SUM is `2x` (+6.02 dB, an exact identity) and only their DIFFERENCE is zero.
/// A convention that called the summing case "the one that nulls" would have to
/// mean something different in each mode.
///
/// ## Where the numbers come from
///
/// **Feedback bound** (series laws 1 and 8). There is no saturator in this loop
/// — deliberately: a flanger's resonance is meant to ring, and a waveshaper
/// would blunt exactly the character the control is for — so the entire
/// stability burden sits on the coefficient clamp and must be measured, not
/// assumed. The loop's elements were measured rather than estimated:
///
///   * the 4-point Lagrange interpolator's magnitude response is **exactly
///     1.000** at DC and rolls off monotonically from there — swept over all
///     fractional offsets and the whole band it never exceeds unity, which is
///     the maximal-flatness property Laakso et al. 1996 describes;
///   * the feedback `DcBlocker` is the only element that can exceed unity, at
///     `2/(1 + p)` = **1.000327** at the shipped corner and 48 kHz;
///   * the optional BBD engine peaks at **1.0038** small-signal and is steeply
///     compressive above that (0.85 at −20 dBFS, 0.28 at full scale) — its
///     compander and bandwidth limit make it the *least* dangerous engine in
///     the loop, not the most.
///
/// `kLoopElementGainBound` = 1.012 covers all three with headroom, so per-pass
/// loop gain is at most `kFbClamp · kLoopElementGainBound` = 0.9816 and the
/// steady-state envelope `worst_case_gain()` is `1/(1 − 0.9816)` ≈ 54.5×. That
/// envelope, not the per-pass figure, is what a host needs: a resonant comb at
/// `kFbClamp` genuinely presents ~30 dB of gain at its peaks.
///
/// **Aliasing policy** (series law 4). **N/A for the clean engine**: delay
/// lines, LFO-driven fractional interpolation, linear feedback and an
/// equal-power mix are all linear, so there is no nonlinearity generating
/// harmonics for anything to alias. The frequency shifter behind barberpole
/// mode is also linear (it translates the spectrum, it does not add to it) and
/// states its own N/A. The **BBD engine is the exception** and does not inherit
/// this: it carries a waveshaper and a compander, and its aliasing policy is
/// the one its own module states — internal oversampling by `kBbdOversample`
/// with the clock-rate bandwidth limit doing the rest.
///
/// **Determinism** (series law 2). The clean and barberpole paths contain no
/// generator at all — a flanger LFO is periodic by definition of the effect, so
/// there is nothing to seed. The BBD engine is the exception again: its clock
/// jitter is stochastic, seeded, and rewound by `reset()`, so a render from
/// `reset()` is bit-identical on every engine.
///
/// RT contract: `prepare(sample_rate)` sizes the delay buffers and MAY
/// allocate. `set_*`, `process()`, `process_stereo()` and `reset()` never
/// allocate, never lock, never perform I/O. State is POD; a zero-initialised
/// instance is a valid silent one. The feedback return is DC-blocked and then
/// denormal-snapped, house convention for any recirculating state that decays
/// toward zero without reaching it.
///
/// References: J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and
/// Chorus", JAES 45(10), pp. 764–788, 1997 (the comb transfer function, the
/// `Δf = 1/D` relation, and flanging/chorus/doubling as one delay-modulation
/// family). T. I. Laakso, V. Välimäki, M. Karjalainen & U. K. Laine,
/// "Splitting the Unit Delay", IEEE Signal Processing Magazine 13(1),
/// pp. 30–60, 1996 (the fractional-delay interpolator, composed from
/// `chardelay::FractionalDelayLine`, not reimplemented). F. Esqueda,
/// V. Välimäki & J. Parker, "Barberpole Phasing and Flanging Illusions",
/// Proc. DAFx-15, Trondheim, 2015 (the frequency-shift route to a
/// non-reversing sweep). The positive/negative flanging vocabulary is
/// documented studio practice with no peer-reviewed source; the behaviour it
/// names is derived here from the comb algebra above rather than borrowed.

#include <pulp/signal/character_delay/bbd.hpp>
#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/frequency_shifter_ssb.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Which of the three sweep topologies the engine runs.
enum class FlangerMode : std::uint8_t {
    classic,       ///< One modulated delay against an undelayed dry. Latency 0.
    through_zero,  ///< Swept wet crossing a fixed-delay dry. Latency = offset.
    barberpole,    ///< Fixed delay, frequency-shifted wet. Latency 0.
};

/// The sign of the wet contribution, in the mix and in the feedback tap alike.
enum class FlangerPolarity : std::uint8_t {
    positive,  ///< `y = dry + wet`. Notches at `(2k+1)/(2D)`.
    negative,  ///< `y = dry − wet`. Notches at `k/D`, including one at DC.
};

/// Which delay engine fills the modulated slot.
enum class FlangerDelayEngine : std::uint8_t {
    clean,  ///< Lagrange-interpolated line. Linear, transparent, the default.
    bbd,    ///< The bucket-brigade character model: dark, companded, alive.
};

/// The flanger.
template <typename SampleType = float>
class FlangerT {
public:
    using Mode = FlangerMode;
    using Polarity = FlangerPolarity;
    using Engine = FlangerDelayEngine;
    using Wave = LfoWave;

    // ── Design parameters (the complete roster) ───────────────────────────

    /// Feedback ceiling. The clamp carries the entire stability burden here,
    /// because the loop deliberately has no saturator to self-limit.
    /// [design parameter] default 0.97, range 0.90 .. 0.97.
    static constexpr double kFbClamp = 0.97;

    /// Supremum of every non-coefficient element in the feedback loop, with
    /// headroom over the measured values quoted in the file doc block.
    ///
    /// The spec's calibration table calls this `kInterpGainBound` and
    /// attributes it to the interpolator. It is renamed because that
    /// attribution is measurably wrong: the 4-point Lagrange kernel's
    /// magnitude never exceeds 1.000 anywhere in the band, at any fractional
    /// offset. The element that can exceed unity is the feedback DC blocker,
    /// and the BBD engine's small-signal peak sits just above it. The shipped
    /// default is unchanged so the registry figure does not move.
    /// [design parameter] default 1.012, range 1.000 .. 1.020.
    static constexpr double kLoopElementGainBound = 1.012;

    /// Corner of the DC blocker in the feedback return. Without it, rounding
    /// in the fractional read accumulates a DC component over a long
    /// high-feedback render, and there is no saturator here to bound it.
    /// [design parameter] default 5 Hz, range 2 .. 10 Hz.
    static constexpr double kDcBlockHz = 5.0;

    /// Modulation excursion bounds. [design parameter] 0.02 .. 4.0 ms.
    static constexpr double kDepthMinMs = 0.02;
    static constexpr double kDepthMaxMs = 4.0;

    /// Classic-mode comb base delay — the flanging end of the delay-modulation
    /// family, deliberately stopping short of chorus territory.
    /// [design parameter] 0.1 .. 10.0 ms.
    static constexpr double kCenterMinMs = 0.1;
    static constexpr double kCenterMaxMs = 10.0;

    /// Through-zero fixed dry delay — the crossing point.
    /// [design parameter] 0.5 .. 10.0 ms.
    static constexpr double kOffsetMinMs = 0.5;
    static constexpr double kOffsetMaxMs = 10.0;

    /// Sweep rate. [design parameter] 0.02 .. 10.0 Hz.
    static constexpr double kRateMinHz = 0.02;
    static constexpr double kRateMaxHz = 10.0;

    /// Floor the classic-mode delay may never go below. The excursion is
    /// clamped against the centre rather than the caller's request, so
    /// `D(t) ≥ kMinClassicDelayMs` holds by construction for every setting the
    /// catalog can produce. [design parameter] default 0.05 ms,
    /// range 0.01 .. 0.5 ms.
    static constexpr double kMinClassicDelayMs = 0.05;

    /// Fixed extra history the delay buffer reserves so the 4-point kernel's
    /// taps never reach past the valid region, however small the delay gets.
    /// [design parameter] default 2 samples, range 1 .. 4 samples.
    static constexpr int kInterpGuardSamples = 2;

    /// De-zippering time for the delay-time target. The interpolator smooths a
    /// FRACTIONAL position; it does nothing about a jump in the target itself,
    /// which is what a host automating `center_delay_ms` produces.
    /// [design parameter] default 3 ms, range 1 .. 10 ms.
    static constexpr double kControlSmoothMs = 3.0;

    /// Mode-change transition window. Short enough to feel immediate, long
    /// enough that the per-sample gain step stays far below anything audible
    /// as a click. [design parameter] default 5 ms, range 1 .. 20 ms.
    static constexpr double kModeSwitchMs = 5.0;

    /// Barberpole shift ceiling. A few Hz is the musical setting; the ceiling
    /// exists so the control cannot be driven into audible ring modulation.
    /// [design parameter] default 20 Hz, range 1 .. 50 Hz.
    static constexpr double kBarberpoleShiftMaxHz = 20.0;

    /// The BBD engine's character amount — how hard its clock jitter and
    /// compander are driven. Fixed here rather than exposed: the flanger's
    /// parameter surface is about the comb, and a second character axis would
    /// duplicate the delay module's own.
    /// [design parameter] default 0.5, range 0 .. 1.
    static constexpr double kBbdCharacter = 0.5;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Sizes both delay buffers for the widest sweep any mode can ask for and
    /// prepares the composed engines. May allocate; nothing else here does.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;

        // The modulated line has to hold the widest delay any mode can reach:
        // through-zero's `2 · kOffsetMaxMs` beats classic's centre + depth.
        const double widest_ms = std::max(2.0 * kOffsetMaxMs, kCenterMaxMs + kDepthMaxMs);
        modulated_capacity_ =
            static_cast<std::size_t>(std::ceil(units::ms_to_samples(widest_ms, sample_rate_))) +
            static_cast<std::size_t>(kInterpGuardSamples) + 4u;
        fixed_capacity_ =
            static_cast<std::size_t>(std::ceil(units::ms_to_samples(kOffsetMaxMs, sample_rate_))) +
            2u;

        const auto pole = static_cast<double>(std::exp(-kTwoPi * kDcBlockHz / sample_rate_));
        for (auto& ch : channels_) {
            ch.line.prepare(modulated_capacity_);
            ch.fixed.assign(fixed_capacity_, 0.0);
            ch.fixed_valid = 0;
            ch.blocker.set_pole(pole);
            ch.bbd.prepare(sample_rate_);
            // The shifter runs as a pure translator inside this loop: the
            // flanger owns the feedback and the mix, so the shifter must own
            // neither, or the spiral would be shaped twice.
            ch.shifter.prepare(sample_rate_);
            ch.shifter.set_feedback(0.0);
            ch.shifter.set_mix(1.0);
            ch.shifter.set_shift_hz(barberpole_hz_);
            // One smoother per channel: the two rails read the modulated line
            // at different delays, so a shared smoother would hand each
            // channel the other's ramp.
            ch.delay_smoother.set_ramp_time(kControlSmoothMs * 0.001, sample_rate_);
        }

        for (auto& lfo : lfos_) lfo.prepare(sample_rate_);
        mode_switch_samples_ =
            std::max(2, static_cast<int>(std::lround(units::ms_to_samples(kModeSwitchMs,
                                                                         sample_rate_))));
        apply_lfo_settings();
        reset();
    }

    /// Rewinds every buffer, filter, oscillator and generator. Allocates
    /// nothing; a render from here is reproducible to the bit.
    void reset() {
        for (auto& ch : channels_) {
            ch.line.reset();
            std::fill(ch.fixed.begin(), ch.fixed.end(), 0.0);
            ch.fixed_write = 0;
            ch.fixed_valid = 0;
            ch.blocker.reset();
            ch.bbd.reset();
            ch.shifter.reset();
            ch.feedback = 0.0;
            ch.delay_ms = 0.0;
            ch.delay_smoother.set_immediate(delay_samples_for(mode_, 0.0));
        }
        for (auto& lfo : lfos_) lfo.reset();
        mode_switch_remaining_ = 0;
        previous_mode_ = mode_;
    }

    /// Constant-time audio fault recovery. Dynamic delay storage is invalidated
    /// logically; fixed-capacity DSP state is reset normally.
    void discard_history() noexcept {
        for (auto& ch : channels_) {
            ch.line.discard_history();
            ch.fixed_write = 0;
            ch.fixed_valid = 0;
            ch.blocker.reset();
            ch.bbd.reset();
            ch.shifter.discard_history();
            ch.feedback = 0.0;
            ch.delay_ms = 0.0;
            ch.delay_smoother.set_immediate(delay_samples_for(mode_, 0.0));
        }
        for (auto& lfo : lfos_) lfo.reset();
        mode_switch_remaining_ = 0;
        previous_mode_ = mode_;
    }

    // ── Parameters (real units throughout) ────────────────────────────────

    /// Takes effect through a `kModeSwitchMs` transition rather than instantly.
    /// Mode is a preset-time control, not an automation lane: it changes the
    /// dry path's latency and the wet path's whole delay law at once.
    void set_mode(Mode mode) {
        if (mode == mode_) return;
        previous_mode_ = mode_;
        mode_ = mode;
        mode_switch_remaining_ = mode_switch_samples_;
    }

    Mode mode() const { return mode_; }

    void set_polarity(Polarity polarity) { polarity_ = polarity; }
    Polarity polarity() const { return polarity_; }

    void set_delay_engine(Engine engine) { engine_ = engine; }
    Engine delay_engine() const { return engine_; }

    void set_rate_hz(double hz) {
        if (!std::isfinite(hz)) return;
        rate_hz_ = std::clamp(hz, kRateMinHz, kRateMaxHz);
        apply_lfo_settings();
    }

    void set_waveform(Wave wave) {
        wave_ = wave;
        apply_lfo_settings();
    }

    /// Stereo spread in CYCLES of the sweep, in the range [0, 0.5]. The house default is a
    /// quarter cycle: far enough apart that the two channels decorrelate into
    /// real width. This is a PHASE control, not a waveform-normalized width
    /// control: a half-cycle is exact inversion for sine, triangle and square,
    /// but not for either saw. Saw still responds to every offset; its stereo
    /// width simply does not vary monotonically by the same law as the
    /// odd-symmetric shapes because its wrap discontinuity moves between rails.
    void set_stereo_spread(double cycles01) {
        if (!std::isfinite(cycles01)) return;
        stereo_spread_ = std::clamp(cycles01, 0.0, 0.5);
        apply_lfo_settings();
    }

    /// Modulation excursion. The value the engine uses is
    /// `effective_depth_ms()`, which is this clamped against the mode's fixed
    /// reference — never the raw request.
    void set_depth_ms(double ms) {
        if (!std::isfinite(ms)) return;
        depth_ms_ = std::clamp(ms, 0.0, kDepthMaxMs);
    }
    double depth_ms() const { return depth_ms_; }

    void set_center_delay_ms(double ms) {
        if (!std::isfinite(ms)) return;
        center_ms_ = std::clamp(ms, kCenterMinMs, kCenterMaxMs);
    }
    double center_delay_ms() const { return center_ms_; }

    void set_offset_ms(double ms) {
        if (!std::isfinite(ms)) return;
        offset_ms_ = std::clamp(ms, kOffsetMinMs, kOffsetMaxMs);
    }
    double offset_ms() const { return offset_ms_; }

    /// Resonance, in the range [-kFbClamp, +kFbClamp]. A negative coefficient is a
    /// different resonance colour, not a polarity control — `set_polarity`
    /// owns that.
    void set_feedback(double fb) {
        if (!std::isfinite(fb)) return;
        feedback_ = std::clamp(fb, -kFbClamp, kFbClamp);
    }
    double feedback() const { return feedback_; }

    /// Equal-power dry/wet, in the range [0, 1]. At 0.5 both gains are `1/√2`, which is
    /// the setting the comb algebra is written for: equal dry and wet weights
    /// give exact nulls.
    void set_mix(double wet01) {
        if (!std::isfinite(wet01)) return;
        mix_ = std::clamp(wet01, 0.0, 1.0);
    }
    double mix() const { return mix_; }

    /// Barberpole shift in Hz. Positive climbs, negative descends.
    void set_barberpole_shift_hz(double hz) {
        if (!std::isfinite(hz)) return;
        barberpole_hz_ = std::clamp(hz, -kBarberpoleShiftMaxHz, kBarberpoleShiftMaxHz);
        for (auto& ch : channels_) ch.shifter.set_shift_hz(barberpole_hz_);
    }
    double barberpole_shift_hz() const { return barberpole_hz_; }

    // ── Processing ────────────────────────────────────────────────────────

    void process(const SampleType* in, SampleType* out, int n) {
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(static_cast<double>(in[i]))) {
                discard_history();
                out[i] = SampleType{0};
                continue;
            }
            const Sweep sweep = advance_sweep();
            out[i] = static_cast<SampleType>(
                run_channel(channels_[0], static_cast<double>(in[i]), sweep));
        }
    }

    void process_stereo(const SampleType* in_left, const SampleType* in_right,
                        SampleType* out_left, SampleType* out_right, int n) {
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(static_cast<double>(in_left[i])) ||
                !std::isfinite(static_cast<double>(in_right[i]))) {
                discard_history();
                out_left[i] = out_right[i] = SampleType{0};
                continue;
            }
            const Sweep sweep = advance_sweep();
            out_left[i] = static_cast<SampleType>(
                run_channel(channels_[0], static_cast<double>(in_left[i]), sweep));
            out_right[i] = static_cast<SampleType>(
                run_channel(channels_[1], static_cast<double>(in_right[i]), sweep, 1));
        }
    }

    /// Zero in classic and barberpole modes — the dry path is a direct
    /// passthrough and the mix is a gain, not a resampling. In through-zero
    /// mode the dry path genuinely carries the fixed offset, so the latency is
    /// real and is reported exactly: the offset line is an INTEGER-sample
    /// buffer (nothing sweeps it, so it needs no fractional accuracy), which
    /// is what keeps this an exact integer rather than a rounded
    /// approximation of a fractional read.
    ///
    /// Reported for the TARGET mode the instant `set_mode` is called, not at
    /// the end of the transition window. A host reads latency on the control
    /// thread and re-aligns on its own schedule; handing it the outgoing mode's
    /// figure for five more milliseconds would give it a number that is already
    /// wrong by the time it acts on it.
    int latency_samples() const noexcept {
        return mode_ == Mode::through_zero ? fixed_delay_samples() : 0;
    }

    // ── Derived quantities (pure functions of the shipped constants) ───────

    /// The excursion actually in force, after the mode's clamp.
    ///
    /// Classic: the sweep is subtracted from the centre at the LFO's trough,
    /// so an unclamped `depth` can drive the delay negative — the catalog's
    /// own extremes do it (`centre` 0.1 ms against `depth` 4.0 ms asks for
    /// −3.9 ms). The excursion is clamped against the centre so
    /// `D(t) ≥ kMinClassicDelayMs` holds by construction.
    ///
    /// Through-zero: the sweep is about the offset and must not go negative
    /// either, so the excursion is capped at the offset — which is exactly the
    /// full `0 .. 2·offset` sweep, the maximum musical setting.
    double effective_depth_ms() const { return effective_depth_for(mode_); }

    /// The delay the modulated line was last read at, in ms. The instrument
    /// the sweep tests measure with — the delay-time law is the thing under
    /// test, and inferring it from the output spectrum would be measuring the
    /// comb and the law at once.
    double instantaneous_delay_ms(int channel = 0) const {
        return channels_[static_cast<std::size_t>(channel)].delay_ms;
    }

    /// The fixed dry delay in samples, exact and integer. Zero outside
    /// through-zero mode, where the dry path is not delayed at all.
    int fixed_delay_samples() const noexcept {
        return static_cast<int>(std::lround(units::ms_to_samples(offset_ms_, sample_rate_)));
    }

    /// Notch frequencies of the comb at a given delay, from `Δf = 1/D`. The
    /// series depends on the polarity: summing puts zeros at the odd
    /// half-multiples, differencing at the whole multiples starting at DC.
    static double notch_hz(int k, double delay_ms, Polarity polarity) {
        const double d = delay_ms * 0.001;
        return polarity == Polarity::positive ? (2.0 * k + 1.0) / (2.0 * d)
                                              : static_cast<double>(k) / d;
    }

    /// Spacing between adjacent notches — the reciprocal of the delay, for
    /// either polarity.
    static double notch_spacing_hz(double delay_ms) { return 1.0 / (delay_ms * 0.001); }

    /// The bound the Forge registry cites (series law 8): the steady-state
    /// envelope of a recursive comb at the feedback ceiling. Not the per-pass
    /// gain — a host needs to know the module can present ~35× at resonance,
    /// and a registry entry of 0.98 would say it can never exceed unity.
    static constexpr double worst_case_gain() {
        return 1.0 / (1.0 - kFbClamp * kLoopElementGainBound);
    }

    /// The dry and wet weights the shipped equal-power law produces at a given
    /// mix. Exposed so a test computes its expectation from the law rather
    /// than restating a number the law might later disagree with.
    static void mix_gains(double wet01, double& dry_gain, double& wet_gain) {
        const double theta = std::clamp(wet01, 0.0, 1.0) * 0.5 * kPi;
        dry_gain = std::cos(theta);
        wet_gain = std::sin(theta);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    /// Everything the two channels share for one frame, sampled once so mono
    /// and stereo advance the modulators and the transition identically.
    struct Sweep {
        double lfo[2] = {0.0, 0.0};
        double wet_scale = 1.0;
        double old_dry = 0.0;
        double new_dry = 1.0;
        bool switching = false;
        bool past_midpoint = true;
        bool snap_delay = false;
    };

    struct Channel {
        chardelay::FractionalDelayLine line{};
        std::vector<double> fixed{};
        std::size_t fixed_write = 0;
        std::size_t fixed_valid = 0;
        DcBlocker<double> blocker{};
        chardelay::BbdChannel bbd{};
        SsbFrequencyShifterT<double> shifter{};
        SmoothedValue<double> delay_smoother{};
        double feedback = 0.0;
        double delay_ms = 0.0;
    };

    Sweep advance_sweep() {
        Sweep s;
        // Both modulators advance every frame, in mono as in stereo. They are
        // two `LfoT` instances rather than one read at two offsets, and they
        // cannot drift apart: identical increments accumulated in `double`
        // from an identical reset produce identical phase forever, so the
        // spread is a fixed angular relationship rather than two clocks that
        // might disagree over a long render.
        s.lfo[0] = static_cast<double>(lfos_[0].next());
        s.lfo[1] = static_cast<double>(lfos_[1].next());

        const bool was_past = past_midpoint();
        if (mode_switch_remaining_ > 0) --mode_switch_remaining_;
        s.switching = mode_switch_remaining_ > 0;
        s.past_midpoint = past_midpoint();
        s.snap_delay = !was_past && s.past_midpoint;

        if (s.switching) {
            const double progress =
                1.0 - static_cast<double>(mode_switch_remaining_) /
                          static_cast<double>(mode_switch_samples_);
            crossfade_gains(crossfade_smoothstep(progress), CrossfadeGainLaw::EqualPower,
                            s.old_dry, s.new_dry);
            // A raised-cosine notch: 1 at both ends, exactly 0 at the midpoint
            // where the delay law changes hands.
            s.wet_scale = std::abs(std::cos(kPi * progress));
        }
        return s;
    }

    bool past_midpoint() const {
        return mode_switch_remaining_ <= 0 || mode_switch_remaining_ * 2 <= mode_switch_samples_;
    }

    /// Target delay for the modulated line, in samples, at an LFO value.
    double delay_samples_for(Mode mode, double lfo) const {
        double ms = 0.0;
        switch (mode) {
            case Mode::classic:
                ms = center_ms_ + effective_depth_for(mode) * lfo;
                break;
            case Mode::through_zero:
                // Anchored to the dry path's REALISED integer delay rather
                // than to `offset_ms` in the abstract. The spec's law is
                // written in milliseconds, but the dry line is an integer
                // buffer, so an offset that does not land on a whole sample
                // would put the crossing between two samples and the null
                // would never be exact. Sweeping about the integer the dry
                // path actually uses makes the crossing exact at every offset.
                return static_cast<double>(fixed_delay_samples()) +
                       units::ms_to_samples(effective_depth_for(mode), sample_rate_) * lfo;
            case Mode::barberpole:
                ms = center_ms_;  // fixed; the shift, not the delay, moves
                break;
        }
        return units::ms_to_samples(ms, sample_rate_);
    }

    double effective_depth_for(Mode mode) const {
        switch (mode) {
            case Mode::classic:
                return std::clamp(depth_ms_, 0.0, center_ms_ - kMinClassicDelayMs);
            case Mode::through_zero:
                return std::min(depth_ms_, offset_ms_);
            case Mode::barberpole:
                return 0.0;
        }
        return 0.0;
    }

    /// One channel of the engine.
    ///
    /// The mode transition here is a WET-NOTCH crossfade, not a dual-engine
    /// one, and the choice is deliberate. Running two complete engines through
    /// the window would need a second BBD line whose memory a preset-time
    /// control does not justify. Instead the wet gain is taken smoothly down to
    /// zero and back up while the dry path crossfades between its two forms —
    /// so the delay law changes hands at the instant the wet contributes
    /// nothing, and every gain moves by at most one window-step per sample. The
    /// delay smoother is SNAPPED at that instant rather than slewed, because
    /// slewing a delay across a mode change is a pitch glide, not a fade.
    double run_channel(Channel& ch, double input, const Sweep& sweep, int index = 0) {
        const Mode active = sweep.past_midpoint ? mode_ : previous_mode_;

        // Delay-time target. Smoothed so a host automating the centre does not
        // step the read pointer.
        const double target = delay_samples_for(active, sweep.lfo[index]);
        if (sweep.snap_delay) ch.delay_smoother.set_immediate(target);
        ch.delay_smoother.set_target(target);
        const double delay = std::clamp(ch.delay_smoother.next(), 1.0, ch.line.max_delay());
        ch.delay_ms = units::samples_to_ms(delay, sample_rate_);

        // The dry reference. Both forms are maintained at all times — the
        // fixed line is written every sample regardless of mode — so a mode
        // change crossfades between two dry paths that are both already
        // correct, with no buffer to fill.
        const double dry_direct = input;
        const double dry_fixed = read_fixed(ch);
        push_fixed(ch, input);
        const double dry =
            sweep.switching
                ? sweep.old_dry * dry_for(previous_mode_, dry_direct, dry_fixed) +
                      sweep.new_dry * dry_for(mode_, dry_direct, dry_fixed)
                : dry_for(active, dry_direct, dry_fixed);

        // The transition window gates the wet path AT ITS SOURCE, not just on
        // the way to the mix, and that placement is the whole point. Silencing
        // only the mix contribution still writes a discontinuous feedback value
        // into the delay line at the instant the delay law changes hands — and
        // that step re-emerges one loop delay later, at full gain, long after
        // the crossfade has finished. Gating the tap instead means the line,
        // the DC blocker and the shifter all see a signal that goes smoothly to
        // zero and back, so there is nothing discontinuous anywhere to come
        // back around.
        const double sign = polarity_ == Polarity::negative ? -1.0 : 1.0;
        double wet = 0.0;

        if (engine_ == Engine::bbd) {
            // The bucket line owns its own delay, so its tap cannot be read
            // ahead of the write the way the clean line's can. Its loop
            // therefore carries one extra sample, which is inaudible against a
            // clocked line whose own delay quantises to its bucket count
            // anyway — but it is a real difference from the clean engine and
            // is stated rather than left to be discovered.
            const double recirculated = snap_to_zero(ch.blocker.process(ch.feedback));
            ch.bbd.update(kBbdCharacter, delay / sample_rate_);
            wet = ch.bbd.process(input + feedback_ * sign * recirculated) * sweep.wet_scale;
        } else {
            // The tap is read at `delay − 1` BEFORE the write, which is
            // bit-for-bit the same sample a read at `delay` after the write
            // would return — the same four taps at the same fractional offset,
            // with the buffer shifted by one instead of the request. Doing it
            // in this order is what makes the loop exactly `delay` samples
            // long rather than `delay + 1`.
            //
            // That one sample is not a rounding detail. The resonance sits
            // where the round-trip phase is zero, so a loop of `delay + 1`
            // puts the peaks at `1/(D+1)` while the feedforward comb's teeth
            // stay at `1/D` — 0.7 % apart at the default 3 ms delay, and 42 %
            // apart at the 0.05 ms floor, where the comb and its own resonance
            // would be audibly detuned from each other.
            wet = ch.line.read(std::max(1.0, delay - 1.0)) * sweep.wet_scale;
            // Barberpole puts the shifter INSIDE the loop, which is what makes
            // the pattern climb without end: every recirculation adds another
            // Δf, so the comb never lands back where it started the way an LFO
            // sweep does. The shifter is a pure translator here — its own
            // feedback and mix are off, because this module owns both.
            if (active == Mode::barberpole) wet = ch.shifter.process(wet);
            // The recirculating tap is DC-blocked so rounding cannot
            // accumulate a drift the (deliberately absent) saturator would
            // otherwise bound, then denormal-snapped because a decaying loop
            // tail approaches zero without reaching it. The FORWARD wet tap is
            // neither, which is what keeps the through-zero crossing an exact
            // cancellation rather than one filtered path against another.
            const double recirculated = snap_to_zero(ch.blocker.process(wet));
            ch.line.push(input + feedback_ * sign * recirculated);
        }
        ch.feedback = wet;

        double dry_gain = 0.0, wet_gain = 0.0;
        mix_gains(mix_, dry_gain, wet_gain);
        // `wet` already carries the transition gate — it was applied at the tap.
        return snap_to_zero(dry_gain * dry + wet_gain * sign * wet);
    }

    static double dry_for(Mode mode, double direct, double fixed) {
        return mode == Mode::through_zero ? fixed : direct;
    }

    double read_fixed(const Channel& ch) const {
        const auto delay = static_cast<std::size_t>(std::max(0, fixed_delay_samples()));
        const std::size_t capacity = ch.fixed.size();
        if (capacity == 0) return 0.0;
        if (ch.fixed_valid < capacity && delay >= ch.fixed_valid) return 0.0;
        const std::size_t index = (ch.fixed_write + capacity - (delay % capacity)) % capacity;
        return ch.fixed[index];
    }

    void push_fixed(Channel& ch, double x) {
        if (ch.fixed.empty()) return;
        ch.fixed[ch.fixed_write] = x;
        ch.fixed_write = (ch.fixed_write + 1u) % ch.fixed.size();
        ch.fixed_valid = std::min(ch.fixed_valid + 1u, ch.fixed.size());
    }

    void apply_lfo_settings() {
        for (std::size_t i = 0; i < lfos_.size(); ++i) {
            lfos_[i].set_rate_hz(rate_hz_);
            lfos_[i].set_wave(wave_);
            lfos_[i].set_phase_offset(i == 0 ? 0.0 : stereo_spread_);
        }
    }

    double sample_rate_ = 44100.0;
    std::size_t modulated_capacity_ = 8;
    std::size_t fixed_capacity_ = 8;

    Mode mode_ = Mode::classic;
    Mode previous_mode_ = Mode::classic;
    Polarity polarity_ = Polarity::positive;
    Engine engine_ = Engine::clean;
    Wave wave_ = LfoWave::sine;

    double rate_hz_ = 0.5;
    double depth_ms_ = 1.5;
    double center_ms_ = 3.0;
    double offset_ms_ = 4.0;
    double feedback_ = 0.6;
    double mix_ = 0.5;
    double stereo_spread_ = 0.25;
    double barberpole_hz_ = 3.0;

    int mode_switch_samples_ = 2;
    int mode_switch_remaining_ = 0;

    std::array<Channel, 2> channels_{};
    std::array<EffectLfoT<double>, 2> lfos_{};
};

using Flanger = FlangerT<float>;
using Flanger64 = FlangerT<double>;

}  // namespace pulp::signal
