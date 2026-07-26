#pragma once

/// @file pitch_shifter.hpp
/// The time-domain ratio pitch shifter — the one that warbles.
///
/// RT contract: `prepare(sample_rate)` allocates the circular delay buffer once
/// and may be called off the audio thread; `set_*`, `process`, and `reset` never
/// allocate, never lock, and never throw. State is POD apart from the delay
/// storage that `prepare` owns (the same arrangement `DelayLineT` has); a
/// default-constructed instance is valid but must see `prepare` before
/// `process`. — USE: the time-domain, warble-forward pitch shifter —
/// expression-pedal whammy bends, dive-bombs, fixed-interval harmony, and
/// detune-thickening. It does **no** pitch tracking: it shifts the whole
/// spectrum by `r = 2^(semitones/12)`, so a single note tracks cleanly and a
/// chord intermodulates (that grain is the effect, not a defect). Reach for the
/// phase-vocoder shifter instead when you need warble-free, formant-preserved
/// shifting; reach for the MIDI-side harmonizer when you need scale-aware
/// diatonic harmony.
///
/// ## How it shifts
///
/// Lineage: Dattorro's delay-line-modulation pitch shifter (Jon Dattorro,
/// "Effect Design, Part 2: Delay-Line Modulation and Chorus," *JAES* 45(10),
/// pp. 764–788, October 1997). A read tap at delay `d(n)` walks the input
/// stream at `1 − d(d)/dn` samples per output sample, so ramping the delay
/// linearly at `1 − r` resamples the input by exactly `r`:
///
///     d(d)/dn = 1 − r                                          (Eq. 3.1)
///     r       = 2^(semitones/12) = 2^(cents/1200)              (Eq. 3.2)
///
/// A finite line cannot ramp forever, so the delay is a sawtooth over a window
/// of `L` samples with phase `p ∈ [0,1)`, `d = p·L`, and
///
///     dφ = (1 − r) / L   per sample, signed                    (Eq. 3.3)
///
/// Each sawtooth reset is a delay discontinuity. Two taps share the ramp half a
/// window apart and are crossfaded with a raised-cosine window that is zero at
/// each tap's own reset:
///
///     pA = p            pB = frac(p + 0.5)
///     wA = sin²(π·pA)   wB = sin²(π·pB) = cos²(π·pA)           (Eq. 3.4)
///     wet = wA·tapA + wB·tapB
///
/// `wA + wB ≡ 1` at every sample, so the pair is a convex combination and the
/// wet leg can never exceed the buffer's peak.
///
/// ## The warble IS the effect
///
/// The sawtooth — and therefore the crossfade — completes one cycle per window
/// traversal. `fs` cancels (series law 7):
///
///     f_warble = |1 − r| · 1000 / window_ms   [Hz]             (Eq. 3.5)
///
/// Short windows buzz, long windows smear. A 12-cent detune at an 80 ms window
/// warbles at 0.087 Hz — a built-in chorus with no LFO. A dive toward `r → 0`
/// drives `|1−r| → 1` and the warble climbs, which is why the bottom of a
/// dive-bomb turns to grain. None of this is a tuned curve; it falls out of the
/// topology, and `warble_hz()` reports it.
///
/// ### The warble is frequency-dependent, and vanishes on a grid
///
/// Non-obvious and worth knowing before measuring anything: the two taps read
/// the same stream half a window apart, so for an input partial at `f` they are
/// separated by the phase
///
///     ψ = π · f · L / fs = π · f · window_ms / 1000            [rad]
///
/// Write `q = f · window_ms / 1000 = ψ/π`. When `q` is an EVEN integer the taps
/// are exactly in phase, the crossfade sums two identical signals, and that
/// partial is shifted with **no warble at all** — a single clean spectral line
/// at `r·f`. When `q` is an ODD integer the taps are in antiphase, the line at
/// `r·f` is fully suppressed, and all the energy sits in two lines at
/// `r·f ± f_warble`, each at exactly half the input amplitude. Every other `q`
/// lands somewhere between. So the wet output of a broadband source is a comb
/// of lines at `f + k·(1−r)·1000/window_ms` whose weights sweep with `q`: the
/// warble depth is a function of frequency, not a constant. `tap_phase_pi()`
/// exposes `q` because a measurement that ignores it will read a correct
/// implementation as broken (see the suite's characterisation cases).
///
/// ## Composition — what this file does NOT own
///
///   * **Ring storage** — `DelayLineT`. There is no buffer arithmetic here
///     beyond the stencil layout.
///   * **Interpolation** — `Interpolator::linear` / `Interpolator::lagrange`.
///     Dattorro 1997 recommends linear over allpass for *modulated* delays
///     (allpass interpolation has audible transients when the delay is swept);
///     linear is the default and 4-point Lagrange is the brighter opt-in.
///   * **The crossfade law** — `crossfade_gains`. The tap blend uses
///     `CrossfadeGainLaw::EqualGain` and the dry/wet mix uses
///     `CrossfadeGainLaw::EqualPower`; see "Two crossfades, two laws" below.
///   * **Glide** — `SlewLimiterT` in `SlewMode::linear`, so a pedal move takes
///     `glide_*_ms` regardless of how far it travels and arrives EXACTLY.
///   * **Units** — `units::semitones_to_ratio`, `units::ms_to_samples`.
///   * **Drift** — `DriftT` (seeded `Xorshift32` lineage), off by default.
///   * **DC hygiene** — `DcBlocker` on the wet leg.
///
/// ### Two crossfades, two laws — deliberately different
///
/// `crossfade.hpp` says equal-power avoids the mid-fade dip when the two legs
/// are DECORRELATED, and equal-gain avoids a level bump when they are
/// correlated. The two taps here are the same signal at different delays —
/// decorrelated in phase for most content — so equal-power is what that guidance
/// would pick. This module uses **equal-gain anyway**, because the −3 dB dip
/// equal-power exists to remove is precisely the audible warble this effect is
/// made of. Equal-power taps would produce a quieter, smoother, and wrong
/// shifter. The dry/wet mix has no such requirement and uses equal-power as
/// specified. Both laws come from `crossfade_gains`; neither is hand-rolled.
///
/// ## Anti-aliasing policy (series law 4)
///
/// **No oversampling and no anti-alias pre-filter, and — unlike most blocks
/// that say "law 4 does not apply" — this one does alias.** There is no
/// nonlinearity anywhere in the signal path (the taps, the crossfade, and the
/// mix are all linear), so there is no nonlinear aliasing to oversample away.
/// But a ratio shifter is a RESAMPLER: at `r > 1` it decimates, so input content
/// above `fs / (2r)` folds back into the band. At 48 kHz and `r = 2` everything
/// above 12 kHz folds; a 15 kHz partial arrives at 18 kHz, not 30 kHz. Linear
/// interpolation's mild delay-dependent lowpass attenuates but does not remove
/// it. This is the documented behaviour of the topology and of the pedals it
/// descends from, it is measured rather than assumed (see the aliasing case in
/// the suite), and it is left in: band-limiting the input to `fs/(2r)` would
/// dull every up-shift to protect a partial most sources do not have. Callers
/// who need alias-free up-shift should band-limit ahead of this block or use a
/// spectral shifter.
///
/// ## What is not here, and why (§9)
///
/// No phase vocoder (different algorithm, different sound — the warble is the
/// point), no pitch tracking or scale-aware harmony (an analysis problem that
/// belongs with the MIDI FX side; `PedalMode::harmony` is fixed-interval and
/// honestly non-tracking), no formant correction (the coupled formant shift is
/// this shifter's character), and **no feedback**. The last one is load-bearing:
/// with no regeneration path the peak output is analytically bounded — both legs
/// are bounded by the input peak and the equal-power mix sums them at worst at
/// the 50/50 point — so `worst_case_gain = √2 ≈ 1.4142`. Adding regeneration
/// would make that number a guess and it would have to be re-derived from a
/// tested invariant (series law 8).
///
/// ## Constants: published vs design parameter
///
/// Published: `r = 2^(s/12)` and the interval table are 12-tone equal
/// temperament, definitional rather than fitted. Everything else — window
/// length, glide times, detent snap width, dive floor, drift depth — is a
/// **design parameter** carrying a default and a range on its own constant.
/// HONEST GAP: no citable literature exists for the crossfade-window length,
/// detent table, glide defaults, or dive floor of a pedal-style shifter;
/// commercial products do not publish them. The topology is Dattorro's; the
/// calibration consists of explicit original design choices.

#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// How the expression pedal maps to a shift target. The mode picks the LAW,
/// not the mix: see `default_mix_for`.
enum class PedalMode : std::uint8_t {
    whammy,   ///< Continuous heel→toe bend. Dry conventionally muted.
    harmony,  ///< Morph between two fixed intervals. Dry conventionally on.
    detune,   ///< Sub-semitone offset scaled by the pedal. Dry on.
    dive,     ///< Dive-bomb toward a floor near (but never at) `r = 0`.
};

/// Fractional-delay read quality for the two taps.
///
/// Named `PitchInterp` rather than the spec's bare `Interp` because
/// `pulp::signal` already carries `Interpolator`, and an unqualified `Interp` in
/// that namespace is a collision waiting to happen.
enum class PitchInterp : std::uint8_t {
    linear,  ///< 2-point. Dattorro's recommendation for modulated delays.
    cubic,   ///< 4-point Lagrange. Brighter tap, ~4× the read cost.
};

/// Which control feeds the shift target.
///
/// The spec's API sketch carries both `set_shift_semitones` and `set_pedal` but
/// no selector, while its parameter table describes `shift_semitones` as the
/// "direct shift when not pedal-driven" — so the selector has to exist
/// somewhere. Making it explicit beats a last-writer-wins rule that a host
/// re-sending both automation lanes would flip back and forth every block.
enum class ShiftSource : std::uint8_t {
    pedal,   ///< `set_pedal` through the §5 law. The default.
    direct,  ///< `set_shift_semitones`. Detents and the pedal law are bypassed.
};

/// Time-domain ratio pitch shifter: dual-tap crossfaded modulated delay.
///
/// See the file doc block for the topology, the warble law, and the aliasing
/// policy. The short version of the public surface:
///
///   * `set_shift_source` picks between the pedal law and a direct semitone
///     target. A pitch-tracked harmonizer drives `direct`; a pedal drives
///     `pedal`.
///   * `process` returns the dry/wet mix; `process_wet` returns the shifted leg
///     alone, for callers doing their own summing across several voices. Call
///     exactly ONE of them per sample — both advance the line and the phase.
///   * `current_semitones` / `current_ratio` / `warble_hz` / `latency_samples`
///     report what the block is actually doing right now.
template <typename SampleType = float>
class PitchShifterT {
public:
    // ── Calibration table (§6) ────────────────────────────────────────────
    //
    // Every constant below is a [design parameter] with a default and a range,
    // per the HONEST GAP note in the file doc block, except where marked
    // definitional. Tests compute their expectations from these rather than
    // restating literals, so retuning one fails the case that documents it.

    /// Crossfade window in ms — sets the warble rate (Eq. 3.5) and the latency
    /// (Eq. 3.6). [design parameter] default 40, range 10 .. 100.
    static constexpr double kWindowMsDefault = 40.0;
    static constexpr double kWindowMsMin = 10.0;
    static constexpr double kWindowMsMax = 100.0;

    /// Pedal portamento, rise and fall. [design parameter] default 60,
    /// range 0 .. 2000.
    static constexpr double kGlideMsDefault = 60.0;
    static constexpr double kGlideMsMin = 0.0;
    static constexpr double kGlideMsMax = 2000.0;

    /// `PedalMode::detune` offset at the toe, in cents. [design parameter]
    /// default +12, range ±50 (the mode is meaningless below about ±1).
    static constexpr double kDetuneCentsDefault = 12.0;
    static constexpr double kDetuneCentsMax = 50.0;

    /// `PedalMode::dive` floor. Sets `r_min = 2^(floor/12)`; `r` never reaches
    /// zero, which would be DC. [design parameter] default −48, range −72 .. −24.
    static constexpr double kDiveFloorSemisDefault = -48.0;
    static constexpr double kDiveFloorSemisMin = -72.0;
    static constexpr double kDiveFloorSemisMax = -24.0;

    /// Whammy endpoints and harmony intervals.
    /// [design parameter] defaults 0 / +12 and +7 / +12, ranges as the node's
    /// `heel_semis` / `toe_semis` (−48 .. +24) and `interval_*_semis` (±24).
    static constexpr double kHeelSemisDefault = 0.0;
    static constexpr double kToeSemisDefault = 12.0;
    static constexpr double kIntervalASemisDefault = 7.0;
    static constexpr double kIntervalBSemisDefault = 12.0;

    /// The node's `shift_semitones` range. Note it does NOT reach the dive
    /// floor's −72: the internal clamp below is the union of the two, because
    /// a dive to −72 has to be expressible even though the direct-shift knob
    /// stops at −48.
    static constexpr double kShiftSemisMin = -48.0;
    static constexpr double kShiftSemisMax = 24.0;

    /// Hard clamp on the slewed semitone signal, and therefore on `r`.
    /// `[kSemitonesFloor, kSemitonesCeil]` = `[2^-6, 2^2]` = `[0.015625, 4]`.
    static constexpr double kSemitonesFloor = kDiveFloorSemisMin;
    static constexpr double kSemitonesCeil = kShiftSemisMax;

    /// Detent table, in semitones: unison, 4ths, 5ths, octaves, double
    /// octaves. 12-TET, definitional — `+5 → 1.334840`, `+7 → 1.498307`.
    static constexpr double kDetentTable[] = {-24.0, -12.0, -7.0, -5.0, 0.0,
                                              5.0,   7.0,   12.0, 24.0};
    static constexpr int kDetentCount = 9;

    /// Detent capture half-width. A Schmitt band so a resting pedal does not
    /// chatter between neighbours: below this the target snaps to the detent.
    /// [design parameter] default 0.35 semitone, range 0.1 .. 1.0 (the floor
    /// keeps the band audible above pedal-CV jitter, the ceiling keeps
    /// neighbouring detents — closest spacing 2 semitones — from overlapping).
    static constexpr double kDetentSnapSemis = 0.35;

    /// A Schmitt trigger needs TWO thresholds; the spec names the band but
    /// gives only one, which is not hysteresis — a target sitting exactly at
    /// the capture edge would still chatter across it. The release half-width
    /// is this ratio times the capture half-width.
    /// [design parameter] default 1.5, range 1.05 .. 2.0.
    static constexpr double kDetentReleaseRatio = 1.5;

    /// The release band is additionally clamped here, so no reachable
    /// (`kDetentSnapSemis`, `kDetentReleaseRatio`) pair can make two detents
    /// capture the same target. Half the closest detent spacing (`−7`/`−5`
    /// and `+5`/`+7`, 2 semitones apart), so it is a property of the table.
    static constexpr double kDetentReleaseLimitSemis = 1.0;

    /// Guard samples reserved for the read stencil. The floor of 4 is a hard
    /// requirement of the 4-point stencil, not tunable headroom; values above 4
    /// only pad against future higher-order interpolants.
    /// [design parameter] default 4, range 4 .. 8.
    static constexpr int kInterpMargin = 4;

    /// Maximum 1σ pitch drift, as a percent of `r`, at `drift_depth = 100 %`.
    /// 0.5 % is about 8.6 cents — an analog wobble, not a vibrato.
    /// [design parameter] default 0.5, range 0.05 .. 5.
    static constexpr double kDriftMaxDepthPercent = 0.5;

    /// Fixed drift seed (series law 2: seeded, never automated, never exposed
    /// as a parameter). Any non-zero constant works.
    static constexpr std::uint32_t kDriftSeed = 0x57484D59u;  // 'WHMY'

    /// Corner of the wet leg's DC blocker, in Hz, converted to a pole at
    /// `prepare` as `p = 1 − 2π·f_c/fs`.
    ///
    /// Specified as a FREQUENCY rather than as `DcBlocker`'s pole because the
    /// pole is not sample-rate invariant and the default one is not as gentle
    /// as it looks: a first-order blocker's corner is `(1−p)·fs/(2π)`, so the
    /// primitive's 0.995 sits at 38 Hz at 48 kHz — −2.0 dB at 50 Hz and
    /// −0.57 dB at 100 Hz. That lands squarely on the oct-down "bass drop" this
    /// module exists to do (a guitar low E shifted down an octave is 41 Hz), so
    /// the blocker would be eating the effect it is insuring. At 5 Hz the same
    /// 41 Hz partial loses 0.065 dB. Series law 7: the corner is in Hz so the
    /// behaviour is identical at every sample rate.
    /// [design parameter] default 5 Hz, range 1 .. 20 Hz.
    static constexpr double kDcBlockerCornerHz = 5.0;

    /// Sample-to-sample step ceiling the raised-cosine window must hold the wet
    /// output under. Lives here rather than in the test so the acceptance
    /// criterion is computed from a shipped constant. A naive single-tap
    /// sawtooth spikes near 2.0 at a reset.
    /// [design parameter] default 0.25, range 0.15 .. 0.40 (with a 1.0-peak
    /// input; the floor allows legitimate fast-transient content, the ceiling
    /// still rejects an unsuppressed reset click).
    static constexpr double kClickBound = 0.25;

    /// Analytic peak-gain bound with an equal-power dry/wet mix and no
    /// feedback. This is Forge's `worst_case_gain`, so it is a headroom figure
    /// downstream plans against and understating it is the expensive direction.
    ///
    /// `√5`, not `√2`. The dry leg is bounded by the input peak and the wet leg
    /// by `kDcBlockerPeakGain = 2`, and an equal-power mix maximises
    /// `cos θ · 1 + sin θ · 2` at `√(1² + 2²)`. It was `√2` — derived from both
    /// legs being bounded by 1 — which understated the real peak by 3.9 dB.
    static constexpr double kWorstCaseGain = 2.23606797749979;  // √5
    static_assert(kWorstCaseGain > 2.0, "the wet leg alone can reach 2x");

    /// The DC blocker's TIME-DOMAIN peak gain: the L1 norm of its impulse
    /// response, which is exactly 2 for every pole position.
    ///
    /// This used to return `2/(1+p)` — the peak of the MAGNITUDE response at
    /// Nyquist, 1.000327 at the default corner — and call it "the exact bound".
    /// A magnitude-response peak bounds the response to a STEADY SINUSOID; it
    /// says nothing about the largest single sample. For `y[n] = x[n] − x[n−1] +
    /// p·y[n−1]` the impulse response is `h[0] = 1`, `h[n] = p^(n−1)(p−1)`, so
    /// the worst-case sample gain is `1 + (1−p)/(1−p) = 2` — 5.9 dB above what
    /// was claimed, reachable on sustained near-DC content (bass, organ pedal).
    ///
    /// The suite certified the old bound with a single 997 Hz sine, which is
    /// precisely the signal for which a magnitude-response bound IS valid. That
    /// is why it shipped.
    static constexpr double kDcBlockerPeakGain = 2.0;

    /// The DC blocker's MAGNITUDE-response peak, `2/(1+p)` at Nyquist.
    ///
    /// Deliberately a separate accessor from `kDcBlockerPeakGain` above, and
    /// deliberately renamed: the two differ by a factor of ~2 and confusing
    /// them is exactly the defect this pair now documents. Use THIS one for
    /// steady-state questions — sideband amplitudes, frequency response — and
    /// the L1 constant for anything about a worst single sample.
    double dc_blocker_magnitude_peak() const { return 2.0 / (1.0 + dc_pole()); }

    /// Fallback sample rate for a block that is queried before `prepare`.
    static constexpr double kDefaultSampleRate = 48000.0;

    /// Per-mode conventional dry/wet default (§6). Applied by the node or the
    /// preset layer, NOT by `set_pedal_mode`: a mode change mid-performance
    /// must not jump the level under the player's foot.
    static constexpr double default_mix_for(PedalMode mode) {
        switch (mode) {
            case PedalMode::harmony: return 0.5;
            case PedalMode::detune: return 0.4;
            case PedalMode::whammy:
            case PedalMode::dive:
            default: return 1.0;
        }
    }

    /// Whether a mode's targets are intervals the detent table can express.
    ///
    /// The spec states detents generally, which read literally makes two modes
    /// inert: `detune`'s default target is 0.12 semitone, three times INSIDE
    /// the 0.35 capture band, so every detune setting would snap to unison and
    /// the mode would do nothing; and `dive` is a continuous collapse whose
    /// whole character is the un-stepped fall (its −48 floor is not even in the
    /// table). Detents quantise interval targets, so they apply where the
    /// targets are intervals.
    static constexpr bool mode_uses_detents(PedalMode mode) {
        return mode == PedalMode::whammy || mode == PedalMode::harmony;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    PitchShifterT() {
        dc_.set_pole(static_cast<SampleType>(dc_pole()));
        drift_.set_seed(kDriftSeed);
        drift_.set_depth_percent(0.0);
        glide_.set_mode(SlewMode::linear);
        glide_.set_rise_ms(kGlideMsDefault);
        glide_.set_fall_ms(kGlideMsDefault);
        update_window();
        glide_.set_immediate(target_semitones());
        semitones_ = target_semitones();
        update_ratio();
    }

    /// Sizes the delay line for `kWindowMsMax` plus the stencil guard and
    /// prepares the composed primitives. The only allocating call (§3.6):
    /// `N = ceil(window_max_ms · fs / 1000) + 2·kInterpMargin`, which is the
    /// spec's `+ kInterpMargin` plus a second margin so the low-end clamp and
    /// the `i+2` stencil node both stay inside the buffer.
    void prepare(double sample_rate) {
        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0)
                           ? sample_rate
                           : kDefaultSampleRate;

        const int capacity =
            static_cast<int>(
                std::ceil(units::ms_to_samples(kWindowMsMax, sample_rate_))) +
            2 * kInterpMargin;
        line_.prepare(capacity);
        max_read_samples_ =
            static_cast<double>(capacity - kInterpMargin);

        glide_.prepare(sample_rate_);
        drift_.prepare(sample_rate_);
        dc_.set_pole(static_cast<SampleType>(dc_pole()));
        update_window();
        reset();
    }

    /// Zeroes the line, the phase, and the filter state, and rewinds the drift
    /// stream (series law 2). The glide is snapped to the CURRENT target rather
    /// than to zero: a reset at +12 must not glide up from unison.
    void reset() {
        line_.reset();
        dc_.reset();
        drift_.reset();
        phase_ = 0.0;
        window_a_ = window_b_ = window_samples_;
        glide_.set_immediate(target_semitones());
        semitones_ = target_semitones();
        update_ratio();
    }

    /// Constant-time counterpart to `reset()` for hostile audio input.
    void discard_history() noexcept {
        line_.discard_history();
        dc_.reset();
        drift_.reset();
        phase_ = 0.0;
        window_a_ = window_b_ = window_samples_;
        glide_.set_immediate(target_semitones());
        semitones_ = target_semitones();
        update_ratio();
    }

    // ── Control surface ───────────────────────────────────────────────────

    /// Selects the pedal law or the direct semitone target. Default `pedal`.
    void set_shift_source(ShiftSource source) { source_ = source; }
    ShiftSource shift_source() const { return source_; }

    /// Direct shift target, used when `shift_source() == ShiftSource::direct`.
    /// Clamped to the node's `shift_semitones` range.
    void set_shift_semitones(double semitones) {
        if (!std::isfinite(semitones)) return;
        shift_semitones_ = clamp_finite(semitones, kShiftSemisMin, kShiftSemisMax);
    }
    double shift_semitones() const { return shift_semitones_; }

    /// Expression position, heel = 0 to toe = 1.
    void set_pedal(double e01) {
        if (!std::isfinite(e01)) return;
        pedal_ = clamp_finite(e01, 0.0, 1.0);
    }
    double pedal() const { return pedal_; }

    /// Picks the pedal LAW only. Deliberately does not touch the mix — see
    /// `default_mix_for`.
    void set_pedal_mode(PedalMode mode) { mode_ = mode; }
    PedalMode pedal_mode() const { return mode_; }

    /// Whammy/dive endpoints in semitones.
    void set_targets(double heel, double toe) {
        if (!std::isfinite(heel) || !std::isfinite(toe)) return;
        heel_semis_ = clamp_finite(heel, kShiftSemisMin, kShiftSemisMax);
        toe_semis_ = clamp_finite(toe, kShiftSemisMin, kShiftSemisMax);
    }

    /// Harmony A (heel) and B (toe) intervals in semitones.
    void set_harmony(double a, double b) {
        if (!std::isfinite(a) || !std::isfinite(b)) return;
        interval_a_semis_ = clamp_finite(a, -24.0, 24.0);
        interval_b_semis_ = clamp_finite(b, -24.0, 24.0);
    }

    void set_detune_cents(double cents) {
        if (!std::isfinite(cents)) return;
        detune_cents_ = clamp_finite(cents, -kDetuneCentsMax, kDetuneCentsMax);
    }

    void set_dive_floor_semis(double semitones) {
        if (!std::isfinite(semitones)) return;
        dive_floor_semis_ =
            clamp_finite(semitones, kDiveFloorSemisMin, kDiveFloorSemisMax);
    }

    /// Crossfade window in ms. This is a VOICING control, not a performance
    /// one: it sets the warble rate and the latency. The change is deferred —
    /// each tap latches the new length at the instant its own phase passes
    /// zero, where its delay AND its crossfade weight are both zero, so neither
    /// tap steps. The cost is that for at most half a warble cycle the two taps
    /// run different window lengths, during which the fading tap's ratio is
    /// `1 − (1−r)·L_b/L_a` rather than `r` — a brief detune on a tap that is on
    /// its way out, in exchange for no click at all. `latency_samples()`
    /// reports the requested window immediately.
    void set_window_ms(double ms) {
        if (!std::isfinite(ms)) return;
        window_ms_ = clamp_finite(ms, kWindowMsMin, kWindowMsMax);
        update_window();
    }
    double window_ms() const { return window_ms_; }

    /// Independent rise/fall portamento on the SEMITONE signal, so a glide is
    /// perceptually even. `SlewMode::linear`: a move takes this long whether it
    /// is one semitone or two octaves, and it arrives exactly.
    void set_glide_ms(double up_ms, double down_ms) {
        if (!std::isfinite(up_ms) || !std::isfinite(down_ms)) return;
        glide_.set_rise_ms(clamp_finite(up_ms, kGlideMsMin, kGlideMsMax));
        glide_.set_fall_ms(clamp_finite(down_ms, kGlideMsMin, kGlideMsMax));
    }
    double glide_up_ms() const { return glide_.rise_ms(); }
    double glide_down_ms() const { return glide_.fall_ms(); }

    /// Equal-power dry↔wet. `0` is the untouched input; `1` is wet only.
    void set_mix(double wet01) {
        if (!std::isfinite(wet01)) return;
        mix_ = clamp_finite(wet01, 0.0, 1.0);
    }
    double mix() const { return mix_; }

    /// Snap the pedal-law target table to musical intervals. Applies in
    /// `whammy` and `harmony` only (see `mode_uses_detents`) and never in
    /// `ShiftSource::direct`, where the caller's semitone value is the point.
    void set_detents(bool on) {
        detents_ = on;
        if (!on) detent_held_ = false;
    }
    bool detents() const { return detents_; }

    void set_interp(PitchInterp interp) { interp_ = interp; }
    PitchInterp interp() const { return interp_; }

    /// Optional seeded pitch drift, `0` (default) .. `1`. At 0 the drift
    /// generator is not advanced at all, so the deterministic path is bypassed
    /// rather than merely zero-amplitude.
    void set_drift_depth(double d01) {
        if (!std::isfinite(d01)) return;
        drift_depth_ = clamp_finite(d01, 0.0, 1.0);
        drift_.set_depth_percent(drift_depth_ * kDriftMaxDepthPercent);
    }
    double drift_depth() const { return drift_depth_; }

    /// Jumps the glide to its current target with no portamento — what a
    /// harmony voice wants on a note-on, so it starts ON its interval instead
    /// of sliding up from the previous note's.
    void snap_to_target() {
        glide_.set_immediate(target_semitones());
        semitones_ = target_semitones();
        update_ratio();
    }

    // ── Reporting ─────────────────────────────────────────────────────────

    /// The pre-slew target in semitones: the pedal law plus detents, or the
    /// direct value. Pure — safe to call from a test or a UI thread.
    double target_semitones() const {
        if (source_ == ShiftSource::direct)
            return std::clamp(shift_semitones_, kSemitonesFloor, kSemitonesCeil);
        return std::clamp(detented(pedal_law(pedal_)), kSemitonesFloor,
                          kSemitonesCeil);
    }

    /// The §5 pedal law alone, before detents and before the glide. Exposed so
    /// the law can be asserted without rendering audio.
    double pedal_law(double e01) const {
        const double e = std::clamp(e01, 0.0, 1.0);
        switch (mode_) {
            case PedalMode::harmony:
                return lerp(interval_a_semis_, interval_b_semis_, e);
            case PedalMode::detune:
                return (detune_cents_ / units::kCentsPerSemitone) * e;
            case PedalMode::dive:
                return lerp(0.0, dive_floor_semis_, e);
            case PedalMode::whammy:
            default:
                return lerp(heel_semis_, toe_semis_, e);
        }
    }

    /// The post-slew shift the block is applying right now, before drift.
    double current_semitones() const { return semitones_; }

    /// The ratio the taps ACTUALLY ramped at on the last processed sample,
    /// drift included. Identical to `2^(current_semitones/12)` while
    /// `drift_depth() == 0`, which is the default — reporting the undrifted
    /// value would make the one control that is allowed to move the pitch
    /// invisible to every caller and every test.
    double current_ratio() const { return effective_ratio_; }

    /// `f_warble` for the current ratio and window (Eq. 3.5). Zero at unison,
    /// where the taps freeze into a static two-tap comb.
    double warble_hz() const {
        return window_ms_ > 0.0
                   ? std::abs(1.0 - effective_ratio_) * 1000.0 / window_ms_
                   : 0.0;
    }

    /// `q = ψ/π` for an input partial at `hz`: the half-window separation of
    /// the two taps in half-cycles of that partial. Even → that partial shifts
    /// with no warble; odd → its carrier is suppressed and it splits into two
    /// lines at `±f_warble`. See the file doc block.
    double tap_phase_pi(double hz) const {
        return hz * window_ms_ / 1000.0;
    }

    /// Window length in samples at the prepared rate.
    double window_samples() const { return window_samples_; }

    /// Honest processing throughput, not phase-aligned PDC: the delay sweeps
    /// `[0, L]`, so the crossfaded output's effective delay hovers at the
    /// window centre (Eq. 3.6). Series law 5 — report the centre, never claim
    /// zero. Sample-rate invariant in wall-clock terms.
    int latency_samples() const {
        return static_cast<int>(
            std::lround(window_ms_ * sample_rate_ / 2000.0));
    }

    // ── Audio ─────────────────────────────────────────────────────────────

    /// One sample through the dry/wet mix. Call exactly one of `process` /
    /// `process_wet` per sample — both write the line and advance the phase.
    SampleType process(SampleType x) {
        if (!std::isfinite(static_cast<double>(x))) {
            discard_history();
            return SampleType{0};
        }
        const double dry = static_cast<double>(x);
        const double wet = advance(dry);
        double dry_gain = 0.0;
        double wet_gain = 0.0;
        crossfade_gains(mix_, CrossfadeGainLaw::EqualPower, dry_gain, wet_gain);
        return static_cast<SampleType>(dry_gain * dry + wet_gain * wet);
    }

    /// The shifted leg alone, with no dry mixed in — for callers summing
    /// several shifted voices themselves and applying their own balance.
    SampleType process_wet(SampleType x) {
        if (!std::isfinite(static_cast<double>(x))) {
            discard_history();
            return SampleType{0};
        }
        return static_cast<SampleType>(advance(static_cast<double>(x)));
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    static double lerp(double a, double b, double t) { return a + (b - a) * t; }

    static double clamp_finite(double v, double lo, double hi) {
        if (!std::isfinite(v)) return lo;
        return std::clamp(v, lo, hi);
    }

    /// Nearest detent with capture/release hysteresis. `detent_held_` is the
    /// Schmitt state: once captured, a target stays snapped until it leaves the
    /// wider release band.
    double detented(double raw) const {
        if (!detents_ || !mode_uses_detents(mode_)) {
            detent_held_ = false;
            return raw;
        }

        const double release =
            std::min(kDetentSnapSemis * kDetentReleaseRatio,
                     kDetentReleaseLimitSemis);

        if (detent_held_) {
            const double held = kDetentTable[detent_index_];
            if (std::abs(raw - held) <= release) return held;
            detent_held_ = false;
        }

        int nearest = 0;
        double best = std::abs(raw - kDetentTable[0]);
        for (int i = 1; i < kDetentCount; ++i) {
            const double d = std::abs(raw - kDetentTable[i]);
            if (d < best) {
                best = d;
                nearest = i;
            }
        }
        if (best <= kDetentSnapSemis) {
            detent_held_ = true;
            detent_index_ = nearest;
            return kDetentTable[nearest];
        }
        return raw;
    }

    void update_window() {
        window_samples_ = units::ms_to_samples(window_ms_, sample_rate_);
        if (window_a_ <= 0.0) window_a_ = window_samples_;
        if (window_b_ <= 0.0) window_b_ = window_samples_;
    }

    void update_ratio() {
        ratio_ = units::semitones_to_ratio(semitones_);
        effective_ratio_ = ratio_;
    }

    /// `p = 1 − 2π·f_c/fs`, clamped short of 1 so a very low corner at a very
    /// high rate cannot produce a marginally-stable pole.
    double dc_pole() const {
        constexpr double kTwoPi = 2.0 * kPi;
        const double p = 1.0 - kTwoPi * kDcBlockerCornerHz / sample_rate_;
        return std::clamp(p, 0.0, 0.9999);
    }

    /// Reads one tap at a fractional delay. The ring arithmetic and the kernels
    /// both belong to primitives this module composes; all that lives here is
    /// the stencil layout — `y0` at `floor(d)` and `y1` one sample older, with
    /// the outer Lagrange nodes either side.
    double read_tap(double delay_samples) const {
        // Not `std::clamp`: before `prepare` the two bounds cross, and a
        // crossed `clamp` is undefined behaviour rather than a zero read.
        //
        // The low bound is what keeps the stencil off the buffer seam. It bites
        // only for `p < kInterpMargin/L` — 0.2 % of a default window — where the
        // tap's own crossfade weight is already below `sin²(π·0.002) ≈ 4e-5`
        // (−88 dB), so the momentary ratio error it causes is inaudible by
        // construction rather than by tolerance.
        if (max_read_samples_ <= static_cast<double>(kInterpMargin)) return 0.0;
        const double d = std::max(static_cast<double>(kInterpMargin),
                                  std::min(delay_samples, max_read_samples_));
        const int i = static_cast<int>(std::floor(d));
        const double frac = d - static_cast<double>(i);
        const double y0 = static_cast<double>(line_.read(i));
        const double y1 = static_cast<double>(line_.read(i + 1));
        if (interp_ == PitchInterp::linear)
            return Interpolator::linear(frac, y0, y1);
        const double ym1 = static_cast<double>(line_.read(i - 1));
        const double y2 = static_cast<double>(line_.read(i + 2));
        return Interpolator::lagrange(frac, ym1, y0, y1, y2);
    }

    static double frac1(double v) { return v - std::floor(v); }

    /// The whole per-sample loop (§3.7).
    double advance(double x) {
        // An unprepared instance emits silence rather than faulting. The
        // contract says `prepare` must precede `process`, but `DelayLineT::push`
        // writes its buffer unguarded, so the penalty for a host that calls
        // `process` early — which real hosts do — would be an out-of-bounds
        // write rather than a diagnosable silence. Guarding the read while
        // leaving the write to fault would be the worst of both.
        if (max_read_samples_ <= static_cast<double>(kInterpMargin)) return 0.0;

        line_.push(static_cast<SampleType>(x));

        // Control cadence: pedal law → detents → glide → r. `semitones_to_ratio`
        // is a `pow`, so it is recomputed only when the glide actually moved.
        const double slewed = glide_.process(
            std::clamp(target_semitones(), kSemitonesFloor, kSemitonesCeil));
        if (slewed != semitones_) {
            semitones_ = slewed;
            update_ratio();
        }
        effective_ratio_ = ratio_;
        if (drift_depth_ > 0.0)
            effective_ratio_ *= static_cast<double>(drift_.next());
        const double r = effective_ratio_;

        // Two taps, half a window apart, each on its own latched window length.
        const double pa = phase_;
        const double pb = frac1(phase_ + 0.5);
        const double tap_a = read_tap(pa * window_a_);
        const double tap_b = read_tap(pb * window_b_);

        // Eq. 3.4 through the house law. `1 − wA` for the second weight keeps
        // `wA + wB ≡ 1` EXACT regardless of how `sin` rounds, which is what
        // bounds the wet leg by the buffer peak.
        const double s = std::sin(kPi * pa);
        double w_b = 0.0;
        double w_a = 0.0;
        crossfade_gains(s * s, CrossfadeGainLaw::EqualGain, w_b, w_a);
        const double wet = static_cast<double>(
            dc_.process(static_cast<SampleType>(w_a * tap_a + w_b * tap_b)));

        // Advance the sawtooth (Eq. 3.3) and latch any pending window change at
        // each tap's own zero crossing, where its delay and its weight are both
        // zero. `window_a_` drives the increment, so tap A is always exactly on
        // ratio; tap B is only ever off during the half cycle after a change.
        const double dphi = window_a_ > 0.0 ? (1.0 - r) / window_a_ : 0.0;
        const double pa_next = frac1(phase_ + dphi);
        const double pb_next = frac1(pa_next + 0.5);
        const bool rising = dphi > 0.0;
        const bool wrapped_a = rising ? (pa_next < pa) : (pa_next > pa);
        const bool wrapped_b = rising ? (pb_next < pb) : (pb_next > pb);
        if (wrapped_a) window_a_ = window_samples_;
        if (wrapped_b) window_b_ = window_samples_;
        phase_ = pa_next;

        return wet;
    }

    // ── Composed primitives ───────────────────────────────────────────────
    DelayLineT<SampleType> line_{};
    DcBlocker<SampleType> dc_{};
    ConstantTimeSlewLimiterT<double> glide_{};
    DriftT<double> drift_{};

    // ── Configuration ─────────────────────────────────────────────────────
    double sample_rate_ = kDefaultSampleRate;
    PedalMode mode_ = PedalMode::whammy;
    ShiftSource source_ = ShiftSource::pedal;
    PitchInterp interp_ = PitchInterp::linear;

    double pedal_ = 0.0;
    double shift_semitones_ = 0.0;
    double heel_semis_ = kHeelSemisDefault;
    double toe_semis_ = kToeSemisDefault;
    double interval_a_semis_ = kIntervalASemisDefault;
    double interval_b_semis_ = kIntervalBSemisDefault;
    double detune_cents_ = kDetuneCentsDefault;
    double dive_floor_semis_ = kDiveFloorSemisDefault;
    double window_ms_ = kWindowMsDefault;
    double mix_ = 1.0;
    double drift_depth_ = 0.0;
    bool detents_ = false;

    // ── Running state ─────────────────────────────────────────────────────
    double window_samples_ = 0.0;
    double window_a_ = 0.0;
    double window_b_ = 0.0;
    double max_read_samples_ = static_cast<double>(kInterpMargin);
    double phase_ = 0.0;
    double semitones_ = 0.0;
    double ratio_ = 1.0;
    double effective_ratio_ = 1.0;

    // Schmitt state for the detent capture. Mutable because `target_semitones`
    // is the natural place to read the law from and callers expect it to be a
    // query; the state it carries is the hysteresis memory, not a result.
    mutable bool detent_held_ = false;
    mutable int detent_index_ = 0;
};

using PitchShifter = PitchShifterT<float>;
using PitchShifter64 = PitchShifterT<double>;

}  // namespace pulp::signal
