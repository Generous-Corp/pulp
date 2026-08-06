#pragma once

#include <pulp/music/pitch.hpp>

/// @file harmony_engine.hpp
/// The intelligent harmonizer — track a monophonic line, snap it to a key and
/// scale, and add one or two harmony voices that stay IN KEY.
///
/// RT contract: `prepare(sample_rate)` allocates the analysis and delay buffers
/// once and may be called off the audio thread; `set_*`, `process`, and `reset`
/// never allocate, never lock, and never throw. State is POD apart from the
/// buffers `prepare` owns; a default-constructed instance is valid but must see
/// `prepare` before `process`. — USE: reach for this when a single sung or
/// played line should be doubled a musical interval away and STAY DIATONIC. A
/// third above is 4 semitones on the tonic and 3 on the supertonic; that
/// per-degree difference is the entire distance between a harmonizer and a
/// pitch shifter with a knob. Reach for `PitchShifterT` directly when you want
/// a fixed parallel transpose, an expression-pedal bend, or a dive-bomb — this
/// block is the one that needs to know what key you are in.
///
/// ## Three stages, one block
///
///   1. **Track** — `YinTrackerT` estimates the input fundamental (YIN).
///   2. **Map** — `DiatonicMapT` quantizes it to the active key + scale and
///      converts a requested interval IN SCALE STEPS into a concrete semitone
///      shift.
///   3. **Shift** — one `PitchShifterT` per voice does the actual shifting,
///      driven from a cents-domain glide.
///
/// ## Composition — what this file does NOT own
///
///   * **The pitch shifter.** The harmony voices ARE `PitchShifterT` instances
///     (`pitch_shifter.hpp`) — the same Dattorro dual-tap crossfaded modulated
///     delay the spec describes as `CrossfadeShifterT`, already built, already
///     characterised. There is no second shifter here, and no crossfade,
///     interpolation, or ring-buffer arithmetic in this file.
///   * **Glide** — `SlewLimiterT` in `SlewMode::linear`, on the CENTS signal.
///   * **Units** — `units::hz_to_midi`, `midi_to_hz`, `semitones_to_ratio`,
///     `ratio_to_cents`, `db_to_linear`.
///   * **Ring storage** — `DelayLineT` for the dry/alignment path.
///   * **Tracker front end** — `DcBlocker`.
///   * **Humanise** — `DriftT` (seeded `Xorshift32` lineage), off by default.
///
/// ## What "intelligent" costs, stated up front
///
/// * **Monophonic only.** YIN is a monophonic f0 estimator; polyphonic pitch
///   tracking is a different and far harder problem. Feed this block one line.
///   A chord produces a confident estimate of *something*, and the harmony will
///   follow that something.
/// * **Latency is real and reported.** A period-length autocorrelation cannot
///   be zero-latency: it needs a window containing two periods of the lowest
///   tracked pitch. At the shipped 80 Hz floor and 48 kHz that is 1200 samples
///   = 25 ms. `latency_samples()` reports it, the dry path is delayed to match,
///   and the shifters are pre-delayed so the wet legs land on the same instant
///   (see `latency_samples` for why the dry delay alone is not enough).
/// * **The shifter's own character comes with it.** These are ratio shifters:
///   they warble, they move formants with pitch ("chipmunk" on upshift), and
///   they alias on up-shift. All of that is documented on `PitchShifterT` and
///   applies here unchanged.
///
/// ## Formant preservation — not exposed by this implementation tier
///
/// The dual-tap crossfade method resamples the waveform, so it moves
/// the entire spectral envelope with the pitch; preserving formants requires
/// source-filter separation (LPC or cepstral envelope estimation) and
/// independent envelope re-imposition, which is a phase-vocoder/LPC-tier
/// technique outside this delay-domain block. There is no citable literature
/// for formant-preserving *crossfade-delay* shifting because the method
/// structurally cannot do it. Consequently this block exposes no formant
/// preservation control; callers must choose a source-filter-capable tier.
///
/// ## Anti-aliasing policy (series law 4)
///
/// Nothing here is nonlinear: the tracker is analysis-only, the mapper is
/// integer arithmetic, and the voice sum is a static feed-forward mix. So there
/// is no nonlinear aliasing to oversample away, and none is done. The
/// resampling aliasing that up-shifting voices DO produce belongs to
/// `PitchShifterT` and is documented there — it is inherited, not introduced.
///
/// ## Gain bound (series law 8)
///
/// There is **no feedback path**: the shifters read from a write-only ring and
/// nothing routes output back into any buffer. The bound is therefore the
/// closed-form arithmetic sum at the declared +6 dB parameter ceiling,
/// including each pitch shifter's DC-blocker sample-gain bound.
/// No invariant test is required because no loop exists — contrast the feedback
/// designs in this series, where the registry number must cite a tested bound.
///
/// ## Constants: published vs design parameter
///
/// Published and cited at the constant: `kYinThreshold` = 0.10 (de Cheveigné &
/// Kawahara 2002 §III.D), the YIN difference/CMND/threshold/parabolic steps
/// themselves, and `r = 2^(s/12)` plus the diatonic step structure, which are
/// equal temperament and public music theory — computed here, never tabulated
/// as magic numbers. Everything else is a **design parameter** carrying a
/// default and a range on its own constant.
///
/// HONEST GAPS: (a) formant preservation, above. (b) the octave-error median in
/// `YinTrackerT` is an ORIGINAL heuristic and explicitly **not** YIN's step 6 —
/// the paper's best-local-estimate step is omitted, and a cheaper 3-tap median
/// across hops is substituted. Saying "we implement YIN" without that
/// qualification would be a misattribution.
///
/// ## References
///
/// [1] A. de Cheveigné and H. Kawahara, "YIN, a fundamental frequency estimator
///     for speech and music," *JASA* 111(4), pp. 1917–1930, April 2002 —
///     the difference function, the cumulative mean normalized difference, the
///     absolute threshold, and parabolic interpolation. Steps 1–5 implemented;
///     step 6 deliberately not (see above).
/// [2] J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and Chorus,"
///     *JAES* 45(10), pp. 764–788, October 1997 — the shifting topology, which
///     lives in `pitch_shifter.hpp` rather than here.
///
/// "DigiTech-style" as used in the specification for this module names a
/// DOCUMENTED PRODUCT-BEHAVIOUR CATEGORY — a diatonic harmonizer that follows a
/// key and scale — and nothing else. No proprietary code, constants, or tables
/// from any commercial harmonizer are used, referenced, or implied.

#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/pitch_shifter.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/slew_limiter.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/yin_tracker.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// The shipped scale collection, order-locked. Index 0 is Major; a `scale`
/// selector's domain is the closed index set {0..9} into this table. Masks are
/// the standard diatonic/common-scale set so an audio-side "C Dorian" and a
/// MIDI-side "C Dorian" agree. Public music theory, not a fitted table.
enum class ScaleType : std::uint8_t {
    major = 0,         ///< Ionian    {0,2,4,5,7,9,11}
    natural_minor = 1, ///< Aeolian   {0,2,3,5,7,8,10}
    dorian = 2,        ///<           {0,2,3,5,7,9,10}
    phrygian = 3,      ///<           {0,1,3,5,7,8,10}
    lydian = 4,        ///<           {0,2,4,6,7,9,11}
    mixolydian = 5,    ///<           {0,2,4,5,7,9,10}
    harmonic_minor = 6, ///<         {0,2,3,5,7,8,11}
    melodic_minor = 7,  ///< ascending {0,2,3,5,7,9,11}
    major_pentatonic = 8, ///<        {0,2,4,7,9}
    minor_pentatonic = 9, ///<        {0,3,5,7,10}
};

/// Number of entries in the shipped scale table.
inline constexpr int kScaleCount = 10;

/// Pitch-class masks, bit k set ⇒ semitone k above the root is in the scale.
/// Order-locked: index 0 is Major, index 9 is Minor pentatonic.
inline constexpr std::uint16_t kScaleTable[kScaleCount] = {
    music::scale_intervals(music::kPulpSignalScales[0].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[1].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[2].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[3].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[4].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[5].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[6].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[7].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[8].scale)->mask(),
    music::scale_intervals(music::kPulpSignalScales[9].scale)->mask(),
};

/// How a pitch class that is not in the active scale is resolved.
enum class OffScalePolicy : std::uint8_t {
    nearest_lower,  ///< Snap DOWN to the nearest in-scale degree (ties → lower).
    nearest,        ///< Snap to the nearest in-scale degree in either direction.
    mute_wet,       ///< Report it and let the caller duck the wet voice.
};

/// The result of one key+scale interval mapping. Everything a caller or a test
/// needs to check the answer against the mask rather than against a literal.
struct DiatonicMapping {
    int shift_semitones = 0;  ///< The concrete shift. THIS is the "intelligent" bit.
    int input_midi = 0;       ///< The chromatic note the tracker was rounded to.
    int snapped_midi = 0;     ///< The in-scale note the input was snapped to.
    int target_midi = 0;      ///< The in-scale note the harmony voice plays.
    int degree = 0;           ///< Index of `snapped_midi` in the scale's degree list.
    bool chromatic = false;   ///< The input was off-scale and had to be snapped.
    bool clamped = false;     ///< The shift hit the ±1 octave ratio clamp.
};

// ── Stage 2 — diatonic interval mapping ───────────────────────────────────

/// Key + scale interval mapper. Pure control-domain integer arithmetic — no
/// audio, no state that `process` touches, but the same lifecycle conventions
/// as the audio blocks.
///
/// RT contract: nothing here allocates, locks, or throws. The degree list is a
/// fixed 12-entry array (a 12-bit pitch-class mask cannot have more than 12
/// degrees), so a key or scale change mid-stream is a rewrite of that array
/// rather than a resize.
///
/// The whole point: `+2 steps` means "up two entries in this scale", so a third
/// above the tonic of C major is 4 semitones and a third above the supertonic
/// is 3. That falls straight out of the major scale's W-W-H-W-W-W-H structure —
/// there are no fitted constants and no tabulated shifts anywhere in this class.
template <typename SampleType = float>
class DiatonicMapT {
public:
    /// A pitch-class mask has at most 12 degrees. The specification calls for a
    /// fixed [16]; 12 is the provable bound and is used instead.
    static constexpr int kMaxDegrees = 12;

    /// Interval range in scale steps — ±two octaves of a 7-note scale.
    /// [design parameter] default +2 (a 3rd above), range −14 .. +14.
    static constexpr int kIntervalStepsMax = 14;

    /// The ±1 octave ratio clamp the shifter stage imposes (`[0.5, 2.0]`).
    /// Clamping to exactly ±12 is musically safe rather than merely bounded:
    /// an octave is in every scale in the table, so a clamped target is still
    /// a scale tone.
    /// [design parameter] default ±12 semitones, fixed by `kRatioMin/Max`.
    static constexpr int kShiftSemitonesMax = 12;

    DiatonicMapT() { rebuild(); }

    /// Root pitch class, 0 = C .. 11 = B.
    void set_key(int root_pitch_class) {
        root_ = ((root_pitch_class % 12) + 12) % 12;
    }
    int key() const { return root_; }

    void set_scale(ScaleType scale) {
        scale_ = scale;
        rebuild();
    }
    ScaleType scale() const { return scale_; }

    void set_off_scale_policy(OffScalePolicy policy) { policy_ = policy; }
    OffScalePolicy off_scale_policy() const { return policy_; }

    /// Number of degrees in the active scale — 7 for the modes, 5 for the
    /// pentatonics. Scale-step arithmetic wraps on this.
    int degree_count() const { return degree_count_; }

    /// Semitone offset of degree `index` above the root. Exposed so a test can
    /// compute its expectation FROM THE MASK rather than restate a table.
    int degree_semitone(int index) const {
        if (index < 0 || index >= degree_count_) return 0;
        return degrees_[static_cast<std::size_t>(index)];
    }

    /// Maps a chromatic MIDI note. The integer entry point — exact, and what
    /// the mapping tests drive.
    DiatonicMapping map_midi(int midi_note, int interval_steps) const {
        DiatonicMapping out;
        out.input_midi = midi_note;
        if (degree_count_ <= 0) return out;

        const int steps = std::clamp(interval_steps, -kIntervalStepsMax, kIntervalStepsMax);

        // Pitch class within the key, and the octave block it sits in.
        const int rel = midi_note - root_;
        const int pc = ((rel % 12) + 12) % 12;
        const int octave = (rel - pc) / 12;

        // Snap to a degree.
        int degree = 0;
        bool chromatic = true;
        for (int i = 0; i < degree_count_; ++i) {
            if (degrees_[static_cast<std::size_t>(i)] == pc) {
                degree = i;
                chromatic = false;
                break;
            }
        }
        if (chromatic) degree = snap_degree(pc);

        out.chromatic = chromatic;
        out.degree = degree;
        out.snapped_midi =
            root_ + 12 * octave + degrees_[static_cast<std::size_t>(degree)];

        // Walk `steps` entries up (or down) the scale, wrapping the octave on
        // the degree COUNT — which is what makes a "third" a scale relation
        // rather than a fixed transposition.
        const int target_degree = degree + steps;
        const int wrapped = floor_div(target_degree, degree_count_);
        const int index = target_degree - wrapped * degree_count_;
        out.target_midi = root_ + 12 * (octave + wrapped) +
                          degrees_[static_cast<std::size_t>(index)];

        int shift = out.target_midi - out.snapped_midi;
        const int limited = std::clamp(shift, -kShiftSemitonesMax, kShiftSemitonesMax);
        out.clamped = limited != shift;
        out.shift_semitones = limited;
        return out;
    }

    /// Maps a tracked frequency: rounds to the nearest chromatic note, then
    /// `map_midi`.
    DiatonicMapping map_hz(double f0_hz, int interval_steps) const {
        if (!(f0_hz > 0.0)) return DiatonicMapping{};
        const int midi = static_cast<int>(
            std::lround(units::hz_to_midi(f0_hz)));
        return map_midi(midi, interval_steps);
    }

private:
    /// Integer floor division — `-1 / 7` must be `-1`, not `0`, or a harmony
    /// below the root wraps into the wrong octave.
    static int floor_div(int a, int b) {
        const int q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }

    /// Resolves an off-scale pitch class to a degree index.
    ///
    /// `nearest_lower` is the default and the interesting one: snapping DOWN
    /// keeps a passing chromatic note from jumping the harmony up by a whole
    /// step mid-phrase, which is far more audible than the alternative.
    int snap_degree(int pc) const {
        int lower = degree_count_ - 1;  // wraps to the degree below the octave
        for (int i = degree_count_ - 1; i >= 0; --i) {
            if (degrees_[static_cast<std::size_t>(i)] <= pc) {
                lower = i;
                break;
            }
        }
        if (policy_ != OffScalePolicy::nearest) return lower;

        const int upper = (lower + 1) % degree_count_;
        const int upper_pc = degrees_[static_cast<std::size_t>(upper)] +
                             (upper == 0 ? 12 : 0);
        const int down = pc - degrees_[static_cast<std::size_t>(lower)];
        const int up = upper_pc - pc;
        return up < down ? upper : lower;  // ties → lower
    }

    void rebuild() {
        const auto index = static_cast<std::size_t>(scale_);
        const std::uint16_t mask =
            index < static_cast<std::size_t>(kScaleCount) ? kScaleTable[index]
                                                          : kScaleTable[0];
        degree_count_ = 0;
        for (int k = 0; k < 12; ++k) {
            if ((mask >> k) & 1u) {
                degrees_[static_cast<std::size_t>(degree_count_)] = k;
                ++degree_count_;
            }
        }
        if (degree_count_ == 0) {  // a mask with no root is not a scale
            degrees_[0] = 0;
            degree_count_ = 1;
        }
    }

    int root_ = 0;
    ScaleType scale_ = ScaleType::major;
    OffScalePolicy policy_ = OffScalePolicy::nearest_lower;
    int degrees_[kMaxDegrees] = {};
    int degree_count_ = 0;
};

using DiatonicMap = DiatonicMapT<float>;
using DiatonicMap64 = DiatonicMapT<double>;

// ── Stages 3 & 4 — voices, detune, glide ──────────────────────────────────

/// The diatonic harmonizer: `YinTrackerT` + `DiatonicMapT` + one
/// `PitchShifterT` per voice.
///
/// RT contract: as the file banner. `prepare` allocates; nothing else does.
template <typename SampleType = float>
class HarmonyEngineT {
public:
    /// Voice slots. Capped at 2 because that is the catalog node's slot
    /// allocation (`v1_*`/`v2_*`) and what `kWorstCaseGain` sums over; more
    /// voices is a capacity change, not a tuning change.
    /// [design parameter] default 2, range {1, 2}.
    static constexpr int kMaxVoices = 2;

    /// The shifter's crossfade window. Sets the splice ("warble") rate:
    /// `f_splice = |1−r|·1000/kCrossfadeMs`, so 20 ms and a +4 semitone third
    /// splice at 13.0 Hz. Larger windows lower the warble but lengthen the
    /// transition smear. [design parameter] default 20 ms, range 10 .. 50.
    static constexpr double kCrossfadeMsDefault = 20.0;
    static constexpr double kCrossfadeMsMin = 10.0;
    static constexpr double kCrossfadeMsMax = 50.0;

    /// Cents-domain portamento, applied whenever a target moves — a key change,
    /// a scale change, an interval change, or the input crossing a scale-degree
    /// boundary. [design parameter] default 60 ms, range 0 .. 500.
    static constexpr double kGlideMsDefault = 60.0;
    static constexpr double kGlideMsMax = 500.0;

    /// Wet-mute ramp applied when the tracker reports an unvoiced frame, so
    /// noise and silence do not get harmonized.
    /// [design parameter] default 10 ms, range 2 .. 50.
    static constexpr double kVoiceMuteMs = 10.0;

    /// Per-voice detune. [design parameter] default 0, range ±50 cents.
    static constexpr double kDetuneMaxCents = 50.0;

    /// Optional seeded humanise drift, as a 1σ depth in cents.
    /// [design parameter] default 0 (off), range 0 .. 15 cents.
    static constexpr double kHumanizeDefault = 0.0;
    static constexpr double kHumanizeMaxCents = 15.0;

    /// Voice interval defaults, in scale steps: a 3rd above and a 5th above.
    /// [design parameter] defaults +2 / +4, range ±14.
    static constexpr int kV1IntervalDefault = 2;
    static constexpr int kV2IntervalDefault = 4;

    /// Level range for the dry and voice mixers, in dB.
    /// [design parameter] defaults 0 dB, range −60 .. +6.
    static constexpr double kLevelMinDb = -60.0;
    static constexpr double kLevelMaxDb = 6.0;
    static constexpr double kLevelMaxLinear = 1.9952623149688795;

    /// The closed-form feed-forward bound at the declared parameter ceiling.
    /// There is no feedback path, so this is arithmetic rather than an
    /// invariant to discover: `10^(6/20) · (dry + 2·voice·dc_l1)`.
    ///
    /// This was 3.0, from "unity dry plus two unity voices" — each wet voice
    /// bounded by 1 because the tap crossfade is convex. But each voice's wet
    /// leg passes a DC blocker, whose worst-case SAMPLE gain is the L1 norm of
    /// its impulse response, exactly 2 — not the `2/(1+p)` = 1.000327 magnitude
    /// peak the derivation used. Inherited from `PitchShifterT`, along with the
    /// error: a magnitude-response peak bounds a steady sinusoid, not a single
    /// sample. Understating a registry headroom figure by 4.4 dB is the
    /// expensive direction.
    static constexpr double kWorstCaseGain =
        kLevelMaxLinear *
        (1.0 + kMaxVoices * PitchShifterT<double>::kDcBlockerPeakGain);

    /// Below this the mute gate is treated as fully closed, so an unvoiced
    /// passage costs nothing and cannot leak a denormal tail.
    /// [design parameter] default 1e-5 (−100 dBFS), range 1e-7 .. 1e-3.
    static constexpr double kMuteFloor = 1e-5;

    HarmonyEngineT() {
        for (int v = 0; v < kMaxVoices; ++v) {
            glide_[v].set_mode(SlewMode::linear);
            glide_[v].set_time_ms(kGlideMsDefault);
            interval_[v] = v == 0 ? kV1IntervalDefault : kV2IntervalDefault;
            // Independent drift streams: one seed for both voices would make
            // two "humanised" voices wander in lockstep, which is exactly the
            // decorrelation the feature exists to provide.
            drift_[v].set_seed(kDriftSeedBase ^
                               (0x9E3779B9u * static_cast<std::uint32_t>(v + 1)));
            drift_[v].set_depth_percent(0.0);
        }
        mute_.set_mode(SlewMode::linear);
        mute_.set_time_ms(kVoiceMuteMs);
        enabled_[0] = true;
        enabled_[1] = false;
    }

    /// Sizes the tracker, the alignment delay, and both shifters. The only
    /// allocating call.
    void prepare(double sample_rate) {
        sample_rate_ = (std::isfinite(sample_rate) && sample_rate > 0.0)
                           ? sample_rate
                           : 48000.0;
        tracker_.prepare(sample_rate_);

        for (int v = 0; v < kMaxVoices; ++v) {
            auto& shifter = shifter_[static_cast<std::size_t>(v)];
            shifter.prepare(sample_rate_);
            shifter.set_shift_source(ShiftSource::direct);
            shifter.set_window_ms(crossfade_ms_);
            // The engine owns the glide, in cents, so the shifter's own
            // semitone-domain slew must be out of the way — two serial glides
            // would neither reach the target in `glide_ms` nor be linear in
            // cents.
            shifter.set_glide_ms(0.0, 0.0);
            shifter.set_mix(1.0);
            shifter.set_interp(interp_);
            glide_[v].prepare(sample_rate_);
            drift_[v].prepare(sample_rate_);
        }
        mute_.prepare(sample_rate_);

        update_alignment();
        // Sized for the WORST case, not the current one: `set_crossfade_ms` can
        // grow the shifter's latency after `prepare`, and with a high `kF0Min`
        // the shifter's window can exceed the tracker's. Sizing to the current
        // latency would leave a later crossfade change reading past the end of
        // the ring — which wraps rather than faults, so it would surface as a
        // mysterious wrong-sounding dry path rather than as a crash.
        const int worst_shifter_latency = static_cast<int>(
            std::lround(kCrossfadeMsMax * sample_rate_ / 2000.0));
        align_.prepare(std::max(tracker_.latency_samples(), worst_shifter_latency) + 1);
        reset();
    }

    void reset() {
        tracker_.reset();
        align_.reset();
        dc_.reset();
        mute_.set_immediate(0.0);
        for (int v = 0; v < kMaxVoices; ++v) {
            shifter_[static_cast<std::size_t>(v)].reset();
            drift_[v].reset();
            shift_semitones_[v] = 0;
            glide_[v].set_immediate(target_cents(v));
            apply_ratio(v, glide_[v].value());
        }
    }

    /// Constant-time audio fault recovery. Controls are retained while every
    /// history-bearing component is logically returned to silence.
    void discard_history() noexcept {
        tracker_.discard_history();
        align_.discard_history();
        dc_.reset();
        mute_.set_immediate(0.0);
        for (int v = 0; v < kMaxVoices; ++v) {
            shifter_[static_cast<std::size_t>(v)].discard_history();
            drift_[v].reset();
            shift_semitones_[v] = 0;
            glide_[v].set_immediate(target_cents(v));
            apply_ratio(v, glide_[v].value());
        }
    }

    // ── Control surface ───────────────────────────────────────────────────

    void set_key(int root_pitch_class) { map_.set_key(root_pitch_class); }
    void set_scale(ScaleType scale) { map_.set_scale(scale); }
    void set_off_scale_policy(OffScalePolicy policy) {
        map_.set_off_scale_policy(policy);
    }

    /// Voice interval in SCALE STEPS, not semitones. `+2` is a third, and what
    /// that means in semitones depends on the degree — which is the point.
    void set_voice_interval(int voice, int steps) {
        if (!valid(voice)) return;
        interval_[voice] = std::clamp(steps, -DiatonicMapT<SampleType>::kIntervalStepsMax,
                                      DiatonicMapT<SampleType>::kIntervalStepsMax);
    }
    int voice_interval(int voice) const { return valid(voice) ? interval_[voice] : 0; }

    void set_voice_detune_cents(int voice, double cents) {
        if (!valid(voice) || !std::isfinite(cents)) return;
        detune_cents_[voice] = clamp_finite(cents, -kDetuneMaxCents, kDetuneMaxCents);
    }
    double voice_detune_cents(int voice) const {
        return valid(voice) ? detune_cents_[voice] : 0.0;
    }

    void set_voice_level_db(int voice, double db) {
        if (!valid(voice) || !std::isfinite(db)) return;
        level_db_[voice] = clamp_finite(db, kLevelMinDb, kLevelMaxDb);
        level_[voice] = units::db_to_linear(level_db_[voice]);
    }
    double voice_level_db(int voice) const {
        return valid(voice) ? level_db_[voice] : kLevelMinDb;
    }

    void set_voice_enabled(int voice, bool on) {
        if (!valid(voice)) return;
        enabled_[voice] = on;
    }
    bool voice_enabled(int voice) const { return valid(voice) && enabled_[voice]; }

    void set_dry_level_db(double db) {
        if (!std::isfinite(db)) return;
        dry_db_ = clamp_finite(db, kLevelMinDb, kLevelMaxDb);
        dry_ = units::db_to_linear(dry_db_);
    }
    double dry_level_db() const { return dry_db_; }

    /// Cents-domain portamento time. `0` steps instantly, which is still
    /// click-safe because the shifter's crossfade masks a ratio jump within one
    /// window.
    void set_glide_ms(double ms) {
        if (!std::isfinite(ms)) return;
        glide_ms_ = clamp_finite(ms, 0.0, kGlideMsMax);
        for (auto& g : glide_) g.set_time_ms(glide_ms_);
    }
    double glide_ms() const { return glide_ms_; }

    /// Seeded humanise depth in cents, 1σ. Off by default; deterministic when
    /// on (series law 2).
    void set_humanize_cents(double cents) {
        if (!std::isfinite(cents)) return;
        humanize_cents_ = clamp_finite(cents, 0.0, kHumanizeMaxCents);
        // DriftT's depth is a percentage of a multiplier near 1, and
        // `cents = 1200·log2(m) ≈ (1200/ln2)·ε` for small ε — so the depth that
        // produces a 1σ of `humanize_cents_` is this, not a free constant.
        const double epsilon_sigma = humanize_cents_ / kCentsPerUnitEpsilon;
        for (auto& d : drift_) d.set_depth_percent(100.0 * epsilon_sigma);
    }
    double humanize_cents() const { return humanize_cents_; }

    /// The shifter's crossfade window, shared by both voices.
    void set_crossfade_ms(double ms) {
        if (!std::isfinite(ms)) return;
        crossfade_ms_ = clamp_finite(ms, kCrossfadeMsMin, kCrossfadeMsMax);
        for (auto& s : shifter_) s.set_window_ms(crossfade_ms_);
        update_alignment();
    }
    double crossfade_ms() const { return crossfade_ms_; }

    void set_interp(PitchInterp interp) {
        interp_ = interp;
        for (auto& s : shifter_) s.set_interp(interp);
    }

    // ── Reporting ─────────────────────────────────────────────────────────

    /// The delay the whole block imposes, and the delay the dry path is given.
    ///
    /// This is `max(W, shifter_latency)` rather than the tracker's `W` alone,
    /// and the difference matters: the wet legs have their OWN throughput delay
    /// through the shifter, so delaying the dry by `W` and leaving the wet
    /// alone would leave the two misaligned by `|W − shifter_latency|` — 720
    /// samples, 15 ms, at the shipped defaults. Instead the shifters are fed
    /// from a pre-delay of `latency_samples() − shifter_latency` so BOTH legs
    /// come out at `latency_samples()`. At the defaults the max is `W`, so the
    /// reported number is the tracker window exactly, as specified.
    ///
    /// A ratio shifter's output is not a delayed copy of its input, so wet
    /// alignment is exact only in the sense that the two legs' window centres
    /// coincide — which is the strongest statement resampling permits.
    int latency_samples() const { return latency_; }

    /// The tracker window `W`, on its own.
    int tracker_latency_samples() const { return tracker_.latency_samples(); }

    /// The shifter's own throughput delay, on its own.
    int shifter_latency_samples() const {
        return shifter_[0].latency_samples();
    }

    double tracked_f0_hz() const { return tracker_.f0_hz(); }
    bool voiced() const { return tracker_.voiced(); }

    /// The mapping the engine last computed for a voice, including whether the
    /// input was off-scale or the shift hit the octave clamp.
    const DiatonicMapping& voice_mapping(int voice) const {
        return mapping_[valid(voice) ? voice : 0];
    }

    /// The voice's post-glide offset in cents, and the ratio derived from it.
    double voice_cents(int voice) const {
        return valid(voice) ? glide_[voice].value() : 0.0;
    }
    double voice_ratio(int voice) const {
        return valid(voice) ? units::cents_to_ratio(glide_[voice].value()) : 1.0;
    }
    int voice_shift_semitones(int voice) const {
        return valid(voice) ? shift_semitones_[voice] : 0;
    }

    /// The wet-mute gate, 0 while unvoiced and 1 while voiced.
    double mute_gain() const { return mute_.value(); }

    const YinTrackerT<SampleType>& tracker() const { return tracker_; }
    const DiatonicMapT<SampleType>& diatonic_map() const { return map_; }

    // ── Audio ─────────────────────────────────────────────────────────────

    SampleType process(SampleType x) {
        // An unprepared instance emits silence rather than faulting. The
        // contract says `prepare` must precede `process`, but `DelayLineT::push`
        // writes its buffer unguarded — so without this, a host that calls
        // `process` early (which real hosts do) gets an out-of-bounds write
        // rather than a diagnosable silence.
        if (latency_ <= 0) return SampleType{0};

        if (!std::isfinite(static_cast<double>(x))) {
            discard_history();
            return SampleType{0};
        }

        const double in = static_cast<double>(x);

        // Stage 1. The tracker sees a DC-blocked copy: a DC offset biases the
        // difference function toward long lags and costs low-note accuracy.
        if (tracker_.process(dc_.process(static_cast<SampleType>(in)))) {
            // A new estimate landed — re-run the mapping. Between hops the
            // mapper output is held and only the glide keeps moving.
            update_targets();
        }

        // The alignment line carries the input for BOTH legs: the dry is read
        // at the full reported latency, the shifters at the pre-delay so their
        // own throughput lands them on the same instant.
        align_.push(static_cast<SampleType>(in));
        const double dry = static_cast<double>(align_.read(latency_));
        const double shifter_in = static_cast<double>(align_.read(pre_delay_));

        const double gate = mute_.process(tracker_.voiced() ? 1.0 : 0.0);

        double wet_sum = 0.0;
        for (int v = 0; v < kMaxVoices; ++v) {
            if (!enabled_[v]) continue;

            // Stage 3 control cadence: the cents ramp advances every sample so
            // the glide is smooth between hops, and the ratio follows it.
            double cents = glide_[v].process(target_cents(v));
            if (humanize_cents_ > 0.0)
                cents += units::ratio_to_cents(static_cast<double>(drift_[v].next()));
            apply_ratio(v, cents);

            const double wet = static_cast<double>(
                shifter_[static_cast<std::size_t>(v)].process_wet(
                    static_cast<SampleType>(shifter_in)));
            if (gate > kMuteFloor) wet_sum += level_[v] * gate * wet;
        }

        return static_cast<SampleType>(dry_ * dry + wet_sum);
    }

private:
    /// `1200/ln 2` — the cents-per-unit-epsilon slope used to convert a
    /// humanise depth in cents into `DriftT`'s multiplier depth.
    static constexpr double kCentsPerUnitEpsilon = 1731.2340490667560888;

    /// Humanise seed base. Series law 2: fixed, never automated, never exposed.
    static constexpr std::uint32_t kDriftSeedBase = 0x48524D4Eu;  // 'HRMN'

    static bool valid(int voice) { return voice >= 0 && voice < kMaxVoices; }

    static double clamp_finite(double v, double lo, double hi) {
        if (!std::isfinite(v)) return lo;
        return std::clamp(v, lo, hi);
    }

    /// Cents target for a voice, before the glide: the diatonic shift plus the
    /// detune, so the detune glides too.
    double target_cents(int voice) const {
        return 100.0 * static_cast<double>(shift_semitones_[voice]) +
               detune_cents_[voice];
    }

    void apply_ratio(int voice, double cents) {
        shifter_[static_cast<std::size_t>(voice)].set_shift_semitones(cents / 100.0);
    }

    /// Re-runs the mapper for both voices on a fresh tracker estimate.
    void update_targets() {
        if (!tracker_.voiced()) return;  // hold through unvoiced frames
        for (int v = 0; v < kMaxVoices; ++v) {
            mapping_[v] = map_.map_hz(tracker_.f0_hz(), interval_[v]);
            shift_semitones_[v] = mapping_[v].shift_semitones;
        }
    }

    void update_alignment() {
        const int tracker_latency = tracker_.latency_samples();
        const int shifter_latency = shifter_[0].latency_samples();
        latency_ = std::max(tracker_latency, shifter_latency);
        pre_delay_ = latency_ - shifter_latency;
    }

    // ── Composed primitives ───────────────────────────────────────────────
    YinTrackerT<SampleType> tracker_{};
    DiatonicMapT<SampleType> map_{};
    PitchShifterT<SampleType> shifter_[kMaxVoices]{};
    DelayLineT<SampleType> align_{};
    DcBlocker<SampleType> dc_{};
    ConstantTimeSlewLimiterT<double> glide_[kMaxVoices]{};
    ConstantTimeSlewLimiterT<double> mute_{};
    DriftT<double> drift_[kMaxVoices]{};

    // ── Configuration ─────────────────────────────────────────────────────
    double sample_rate_ = 48000.0;
    double crossfade_ms_ = kCrossfadeMsDefault;
    double glide_ms_ = kGlideMsDefault;
    double humanize_cents_ = kHumanizeDefault;
    double dry_db_ = 0.0;
    double dry_ = 1.0;
    PitchInterp interp_ = PitchInterp::linear;
    int interval_[kMaxVoices] = {kV1IntervalDefault, kV2IntervalDefault};
    double detune_cents_[kMaxVoices] = {0.0, 0.0};
    double level_db_[kMaxVoices] = {0.0, 0.0};
    double level_[kMaxVoices] = {1.0, 1.0};
    bool enabled_[kMaxVoices] = {true, false};

    // ── Running state ─────────────────────────────────────────────────────
    DiatonicMapping mapping_[kMaxVoices] = {};
    int shift_semitones_[kMaxVoices] = {0, 0};
    int latency_ = 0;
    int pre_delay_ = 0;
};

using HarmonyEngine = HarmonyEngineT<float>;
using HarmonyEngine64 = HarmonyEngineT<double>;

}  // namespace pulp::signal
