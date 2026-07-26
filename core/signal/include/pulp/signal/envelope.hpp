#pragma once

/// @file envelope.hpp
/// The envelope family — AR, AD, AHD, DAHDSR, and a trigger-driven modulation
/// envelope — plus a level-independent transient detector.
///
/// `AdsrT` (adsr.hpp) is the existing keyboard envelope and stays exactly as it
/// is. Everything here is additive: the shapes it does not cover, and the one
/// superset to reach for when it is not enough.
///
/// RT contract: every type here holds fixed scalar state and owns no memory.
/// `prepare()` and the `set_*()` setters are control-side calls; `trigger()`,
/// `gate()`, `note_on()`, `note_off()`, `next()`, `reset()`, and the accessors
/// allocate nothing and are audio-thread safe.
///
/// USE — the whole point of having five of these is that reaching for the right
/// one is cheaper than configuring the wrong one:
///
/// - **`ArT`** is the sustained-gate workhorse: pad and drone VCAs, gate-CV
///   amplitude, sidechain duck-and-recover shapes (invert it with an
///   `AttenuverterT`). When a sound has no decay stage, use this, not an ADSR
///   with sustain pinned to 1.
/// - **`AdT`** is percussion and plucks from triggers, and modulation blips
///   like filter pings — anything fired by an event rather than held by a key.
/// - **`AhdT`** is drum voices and sample playback: the hold keeps the
///   transient and body at full level for a defined window before the tail. It
///   is also how you get gate-length-independent chopped shapes, with the hold
///   as the chop length.
/// - **`AdsrT`** (existing) is keyboard and sustain instruments — the
///   note-length-aware classic. Do not extend it; reach for `DahdsrT`.
/// - **`DahdsrT`** is the superset for layered and orchestral patches. The
///   delay staggers layers (a swell pad under a pluck), the hold shapes the
///   pre-decay plateau, and per-stage curves match sampled-instrument contours.
///   When in doubt in a mod matrix, this is the mod source.
/// - **Looping** on `AdT` / `AhdT` / `DahdsrT` gives envelope-as-LFO hybrids:
///   rhythmic swells that are shaped rather than symmetric. With synced stage
///   times it is a groove-locked modulation sequence from one primitive.
/// - **`ModEnvT`** is per-hit modulation shaping — filter sweep depth per drum
///   hit, pitch-drop envelopes. Trigger-driven, ignores gates, signed depth.
/// - **`TransientDetectorT`** is level-independent attack detection: input
///   duckers, adaptive transient shapers, "how hard was that hit" as a mod
///   source.

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/mod_tools.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Stage of any envelope in this header. Reported by every type's `stage()`;
/// the shorter shapes simply never enter the stages they do not have.
enum class EnvelopeStage : std::uint8_t {
    idle,
    delay,
    attack,
    hold,
    decay,
    sustain,
    release,
};

namespace detail {

/// Sample counter for one envelope stage. `progress()` runs 0 -> 1 exclusive
/// over `length` samples; `advance()` reports the sample on which the stage is
/// over. A zero-length stage is complete immediately and is skipped by the
/// engine rather than consuming a sample.
struct EnvelopeStageClock {
    long long pos = 0;
    long long length = 0;

    void restart(long long len) {
        pos = 0;
        length = len;
    }

    float progress() const {
        if (length <= 0) return 1.0f;
        return static_cast<float>(pos) / static_cast<float>(length);
    }

    bool advance() { return ++pos >= length; }
};

inline long long envelope_ms_to_samples(double ms, double sample_rate) {
    return static_cast<long long>(std::lround(std::max(0.0, ms) * 0.001 * sample_rate));
}

/// The DAHDSR superset state machine. Every public envelope in this header is
/// this engine with some stages disabled and some behavior flags set, so the
/// stage timing, the curve law, retrigger-from-current-level, and looping have
/// exactly one implementation to get right.
///
/// RT contract: fixed scalar state; `next()`, `trigger()`, `release()`, and
/// `reset()` allocate nothing.
template <typename SampleType = float>
class EnvelopeEngine {
public:
    using Stage = EnvelopeStage;

    static constexpr int kMaxLoops = 128;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 1.0;
        update_lengths_();
        reset();
    }

    void set_delay_ms(double ms) { delay_ms_ = std::max(0.0, ms); update_lengths_(); }
    void set_attack_ms(double ms) { attack_ms_ = std::max(0.0, ms); update_lengths_(); }
    void set_hold_ms(double ms) { hold_ms_ = std::max(0.0, ms); update_lengths_(); }
    void set_decay_ms(double ms) { decay_ms_ = std::max(0.0, ms); update_lengths_(); }
    void set_release_ms(double ms) { release_ms_ = std::max(0.0, ms); update_lengths_(); }

    void set_attack_curve(float c) { attack_curve_ = std::clamp(c, -1.0f, 1.0f); }
    void set_decay_curve(float c) { decay_curve_ = std::clamp(c, -1.0f, 1.0f); }
    void set_release_curve(float c) { release_curve_ = std::clamp(c, -1.0f, 1.0f); }

    void set_sustain(SampleType level) {
        sustain_ = std::clamp(level, SampleType{0}, SampleType{1});
    }

    void set_has_sustain(bool on) { has_sustain_ = on; }
    void set_gate_driven(bool on) { gate_driven_ = on; }

    void set_loop(bool on, int count) {
        loop_ = on;
        loop_count_ = std::clamp(count, 0, kMaxLoops);
    }

    void reset() {
        stage_ = Stage::idle;
        level_ = SampleType{0};
        peak_ = SampleType{1};
        start_ = SampleType{0};
        release_start_ = SampleType{0};
        loops_done_ = 0;
        clock_.restart(0);
    }

    void trigger(SampleType velocity) {
        peak_ = std::clamp(velocity, SampleType{0}, SampleType{1});
        loops_done_ = 0;
        begin_shape_();
    }

    void release() {
        if (!gate_driven_ || stage_ == Stage::idle || stage_ == Stage::release) return;
        release_start_ = level_;
        stage_ = Stage::release;
        clock_.restart(release_samples_);
        if (release_samples_ <= 0) {
            // Straight to idle, not through finish_(): a release is the end of
            // the note, and finish_() would loop a looping envelope — a
            // note-off would start it over.
            stage_ = Stage::idle;
            level_ = SampleType{0};
            clock_.restart(0);
        }
    }

    SampleType next() {
        level_ = level_at_();
        advance_();
        return level_;
    }

    SampleType current() const { return level_; }
    Stage stage() const { return stage_; }
    bool active() const { return stage_ != Stage::idle; }
    int loops_completed() const { return loops_done_; }

private:
    void update_lengths_() {
        delay_samples_ = envelope_ms_to_samples(delay_ms_, sample_rate_);
        attack_samples_ = envelope_ms_to_samples(attack_ms_, sample_rate_);
        hold_samples_ = envelope_ms_to_samples(hold_ms_, sample_rate_);
        decay_samples_ = envelope_ms_to_samples(decay_ms_, sample_rate_);
        release_samples_ = envelope_ms_to_samples(release_ms_, sample_rate_);
    }

    long long shape_length_() const {
        return delay_samples_ + attack_samples_ + hold_samples_ + decay_samples_;
    }

    SampleType sustain_level_() const {
        return has_sustain_ ? sustain_ * peak_ : SampleType{0};
    }

    void begin_shape_() {
        start_ = level_;
        enter_delay_();
    }

    void enter_delay_() {
        if (delay_samples_ <= 0) {
            enter_attack_();
            return;
        }
        stage_ = Stage::delay;
        clock_.restart(delay_samples_);
    }

    void enter_attack_() {
        if (attack_samples_ <= 0) {
            level_ = peak_;
            enter_hold_();
            return;
        }
        stage_ = Stage::attack;
        clock_.restart(attack_samples_);
    }

    void enter_hold_() {
        if (hold_samples_ <= 0) {
            enter_decay_();
            return;
        }
        stage_ = Stage::hold;
        clock_.restart(hold_samples_);
    }

    void enter_decay_() {
        if (decay_samples_ <= 0) {
            level_ = sustain_level_();
            enter_after_decay_();
            return;
        }
        stage_ = Stage::decay;
        clock_.restart(decay_samples_);
    }

    void enter_after_decay_() {
        // Looping skips the sustain hold: an envelope that stops to wait for a
        // note-off cannot also be a cycling modulation source.
        if (has_sustain_ && !loop_) {
            stage_ = Stage::sustain;
            clock_.restart(0);
            return;
        }
        finish_();
    }

    void finish_() {
        // A shape with no length at all cannot be looped — that is an infinite
        // loop, not a modulation source.
        if (loop_ && shape_length_() > 0) {
            ++loops_done_;
            if (loop_count_ == 0 || loops_done_ < loop_count_) {
                start_ = level_;
                enter_delay_();
                return;
            }
        }
        stage_ = Stage::idle;
        level_ = SampleType{0};
        clock_.restart(0);
    }

    SampleType level_at_() const {
        const float p = clock_.progress();
        switch (stage_) {
            case Stage::delay:
                // A fresh trigger enters the delay at zero because `start_` is
                // zero. A retrigger holds the level it captured instead:
                // snapping to zero for the delay and then resuming the attack
                // from `start_` would be two full-scale steps, which is the
                // opposite of retrigger-from-current-level.
                return start_;
            case Stage::attack:
                return start_ + (peak_ - start_)
                                    * static_cast<SampleType>(curve_rise(p, attack_curve_));
            case Stage::hold:
                return peak_;
            case Stage::decay: {
                const SampleType floor_level = sustain_level_();
                return floor_level + (peak_ - floor_level)
                                         * static_cast<SampleType>(curve_fall(p, decay_curve_));
            }
            case Stage::sustain:
                return sustain_level_();
            case Stage::release:
                return release_start_ * static_cast<SampleType>(curve_fall(p, release_curve_));
            case Stage::idle:
            default:
                return SampleType{0};
        }
    }

    void advance_() {
        switch (stage_) {
            case Stage::delay:
                if (clock_.advance()) enter_attack_();
                return;
            case Stage::attack:
                if (clock_.advance()) enter_hold_();
                return;
            case Stage::hold:
                if (clock_.advance()) enter_decay_();
                return;
            case Stage::decay:
                if (clock_.advance()) enter_after_decay_();
                return;
            case Stage::release:
                if (clock_.advance()) {
                    stage_ = Stage::idle;
                    level_ = SampleType{0};
                    clock_.restart(0);
                }
                return;
            case Stage::sustain:
            case Stage::idle:
            default:
                return;
        }
    }

    EnvelopeStageClock clock_{};
    double sample_rate_ = 48000.0;
    double delay_ms_ = 0.0;
    double attack_ms_ = 10.0;
    double hold_ms_ = 0.0;
    double decay_ms_ = 100.0;
    double release_ms_ = 200.0;
    long long delay_samples_ = 0;
    long long attack_samples_ = 480;
    long long hold_samples_ = 0;
    long long decay_samples_ = 4800;
    long long release_samples_ = 9600;
    SampleType level_ = SampleType{0};
    SampleType peak_ = SampleType{1};
    SampleType start_ = SampleType{0};
    SampleType release_start_ = SampleType{0};
    SampleType sustain_ = SampleType{0.7};
    float attack_curve_ = 0.0f;
    float decay_curve_ = 0.0f;
    float release_curve_ = 0.0f;
    int loop_count_ = 0;
    int loops_done_ = 0;
    Stage stage_ = Stage::idle;
    bool has_sustain_ = true;
    bool gate_driven_ = true;
    bool loop_ = false;
};

} // namespace detail

/// Attack-Release, gate-driven. Attack while the gate is high, sustain at 1,
/// release when it falls.
///
/// USE: the sustained-gate workhorse — pad and drone VCAs, gate-CV amplitude,
/// and sidechain duck-and-recover shapes (invert it through an
/// `AttenuverterT`). When a sound has no decay stage, this is the right
/// primitive; an ADSR with sustain pinned to 1 is the same shape with three
/// more parameters to explain and get wrong.
template <typename SampleType = float>
class ArT {
public:
    void prepare(double sample_rate) {
        engine_.set_has_sustain(true);
        engine_.set_sustain(SampleType{1});
        engine_.set_gate_driven(true);
        engine_.set_hold_ms(0.0);
        engine_.set_decay_ms(0.0);
        engine_.prepare(sample_rate);
    }

    void set_attack_ms(double ms) { engine_.set_attack_ms(ms); }
    void set_release_ms(double ms) { engine_.set_release_ms(ms); }
    void set_attack_curve(float c) { engine_.set_attack_curve(c); }
    void set_release_curve(float c) { engine_.set_release_curve(c); }

    /// Round-2 compatibility curve: 0 is linear and 1 is the strongest
    /// capacitor-like attack/release pair. The shared engine's signed curves
    /// use opposite signs for rising and falling segments.
    void set_curve(double c) {
        const float curve = static_cast<float>(std::clamp(c, 0.0, 1.0));
        engine_.set_attack_curve(-curve);
        engine_.set_release_curve(curve);
    }

    /// Gate edges drive the envelope; repeated calls with the same state are
    /// no-ops, so this can be called every sample from a gate signal.
    void gate(bool on) {
        if (on == gate_) return;
        gate_ = on;
        if (on) engine_.trigger(SampleType{1});
        else engine_.release();
    }

    /// Convenience spelling with exactly the same level-before-advance sample
    /// ordering as `gate(true)`.
    void gate_on() { gate(true); }
    void gate_off() { gate(false); }

    void reset() {
        engine_.reset();
        gate_ = false;
    }

    SampleType next() { return engine_.next(); }
    SampleType current() const { return engine_.current(); }
    SampleType value() const { return current(); }
    bool active() const { return engine_.active(); }
    EnvelopeStage stage() const { return engine_.stage(); }

private:
    detail::EnvelopeEngine<SampleType> engine_{};
    bool gate_ = false;
};

using Ar = ArT<float>;
using Ar64 = ArT<double>;

/// Attack-Decay one-shot, trigger-driven. Gate release is ignored by
/// construction.
///
/// USE: percussion and plucks fired from `TriggerDetectT` or `BurstGenT`;
/// modulation blips such as filter pings; anything where the event has no
/// duration. Retriggering during the decay ramps from the current level rather
/// than from zero, so a fast roll does not click.
template <typename SampleType = float>
class AdT {
public:
    void prepare(double sample_rate) {
        engine_.set_has_sustain(false);
        engine_.set_gate_driven(false);
        engine_.set_hold_ms(0.0);
        engine_.prepare(sample_rate);
    }

    void set_attack_ms(double ms) { engine_.set_attack_ms(ms); }
    void set_decay_ms(double ms) { engine_.set_decay_ms(ms); }
    void set_attack_curve(float c) { engine_.set_attack_curve(c); }
    void set_decay_curve(float c) { engine_.set_decay_curve(c); }

    void set_curve(double c) {
        const float curve = static_cast<float>(std::clamp(c, 0.0, 1.0));
        engine_.set_attack_curve(-curve);
        engine_.set_decay_curve(curve);
    }

    /// @param on true to loop; `count` 0 loops forever, otherwise 1..128.
    void set_loop(bool on, int count = 0) { engine_.set_loop(on, count); }

    void trigger(SampleType velocity = SampleType{1}) { engine_.trigger(velocity); }
    void gate_on() { trigger(); }
    void gate_off() {}
    void reset() { engine_.reset(); }

    SampleType next() { return engine_.next(); }
    SampleType current() const { return engine_.current(); }
    SampleType value() const { return current(); }
    bool active() const { return engine_.active(); }
    int loops_completed() const { return engine_.loops_completed(); }
    EnvelopeStage stage() const { return engine_.stage(); }

private:
    detail::EnvelopeEngine<SampleType> engine_{};
};

using Ad = AdT<float>;
using Ad64 = AdT<double>;

/// Attack-Hold-Decay one-shot, trigger-driven.
///
/// USE: drum voices and sample playback. The hold keeps the transient and body
/// at full level for a defined window before the tail starts, which is the
/// output-VCA shape of essentially every drum synth. It is also the way to get
/// a chopped shape whose length does not depend on how long a gate was held —
/// the hold *is* the chop length.
template <typename SampleType = float>
class AhdT {
public:
    void prepare(double sample_rate) {
        engine_.set_has_sustain(false);
        engine_.set_gate_driven(false);
        engine_.prepare(sample_rate);
    }

    void set_attack_ms(double ms) { engine_.set_attack_ms(ms); }
    void set_hold_ms(double ms) { engine_.set_hold_ms(ms); }
    void set_decay_ms(double ms) { engine_.set_decay_ms(ms); }
    void set_attack_curve(float c) { engine_.set_attack_curve(c); }
    void set_decay_curve(float c) { engine_.set_decay_curve(c); }
    void set_curve(double c) {
        const float curve = static_cast<float>(std::clamp(c, 0.0, 1.0));
        engine_.set_attack_curve(-curve);
        engine_.set_decay_curve(curve);
    }
    void set_loop(bool on, int count = 0) { engine_.set_loop(on, count); }

    void trigger(SampleType velocity = SampleType{1}) { engine_.trigger(velocity); }
    void gate_on() { trigger(); }
    void gate_off() {}
    void reset() { engine_.reset(); }

    SampleType next() { return engine_.next(); }
    SampleType current() const { return engine_.current(); }
    SampleType value() const { return current(); }
    bool active() const { return engine_.active(); }
    int loops_completed() const { return engine_.loops_completed(); }
    EnvelopeStage stage() const { return engine_.stage(); }

private:
    detail::EnvelopeEngine<SampleType> engine_{};
};

using Ahd = AhdT<float>;
using Ahd64 = AhdT<double>;

/// Delay-Attack-Hold-Decay-Sustain-Release with per-stage curves and velocity.
///
/// USE: the superset for layered and orchestral patches. The delay staggers
/// layers — a swell pad arriving under a pluck is one parameter, not a second
/// voice. The hold shapes the pre-decay plateau. The per-stage curves are what
/// let a synthesized contour match a sampled instrument's. When in doubt about
/// which mod source a matrix slot should carry, this is it.
///
/// Velocity scales the peak, so the decay floor (`sustain * velocity`) tracks
/// it too — a soft note is quieter throughout, not just at its attack.
///
/// In looping mode the sustain stage is skipped: an envelope that stops to wait
/// for a note-off cannot also be a cycling source.
template <typename SampleType = float>
class DahdsrT {
public:
    void prepare(double sample_rate) {
        engine_.set_has_sustain(true);
        engine_.set_gate_driven(true);
        engine_.prepare(sample_rate);
    }

    void set_delay_ms(double ms) { engine_.set_delay_ms(ms); }
    void set_attack_ms(double ms) { engine_.set_attack_ms(ms); }
    void set_hold_ms(double ms) { engine_.set_hold_ms(ms); }
    void set_decay_ms(double ms) { engine_.set_decay_ms(ms); }
    void set_sustain(SampleType level) { engine_.set_sustain(level); }
    void set_release_ms(double ms) { engine_.set_release_ms(ms); }

    void set_attack_curve(float c) { engine_.set_attack_curve(c); }
    void set_decay_curve(float c) { engine_.set_decay_curve(c); }
    void set_release_curve(float c) { engine_.set_release_curve(c); }

    void set_curve(double c) {
        const float curve = static_cast<float>(std::clamp(c, 0.0, 1.0));
        engine_.set_attack_curve(-curve);
        engine_.set_decay_curve(curve);
        engine_.set_release_curve(curve);
    }

    void set_loop(bool on, int count = 0) { engine_.set_loop(on, count); }

    void note_on(SampleType velocity = SampleType{1}) { engine_.trigger(velocity); }
    void note_off() { engine_.release(); }
    void gate_on() { note_on(); }
    void gate_off() { note_off(); }
    void reset() { engine_.reset(); }

    SampleType next() { return engine_.next(); }
    SampleType current() const { return engine_.current(); }
    SampleType value() const { return current(); }
    bool active() const { return engine_.active(); }
    int loops_completed() const { return engine_.loops_completed(); }
    EnvelopeStage stage() const { return engine_.stage(); }

private:
    detail::EnvelopeEngine<SampleType> engine_{};
};

using Dahdsr = DahdsrT<float>;
using Dahdsr64 = DahdsrT<double>;

/// Delay-Attack-Hold-Decay modulation envelope with a signed depth. No sustain,
/// no release, gates ignored.
///
/// USE: per-hit modulation shaping — filter sweep depth per drum hit, pitch
/// drops, the mod-envelope role in a drum rack. The signed depth is why this is
/// not just an `AhdT`: a modulation destination usually wants to be pushed
/// *down* as often as up, and negating at the source keeps the matrix slot's
/// depth free for the amount.
template <typename SampleType = float>
class ModEnvT {
public:
    void prepare(double sample_rate) {
        engine_.set_has_sustain(false);
        engine_.set_gate_driven(false);
        engine_.prepare(sample_rate);
    }

    void set_delay_ms(double ms) { engine_.set_delay_ms(ms); }
    void set_attack_ms(double ms) { engine_.set_attack_ms(ms); }
    void set_hold_ms(double ms) { engine_.set_hold_ms(ms); }
    void set_decay_ms(double ms) { engine_.set_decay_ms(ms); }
    void set_release_ms(double ms) { engine_.set_release_ms(ms); }
    void set_sustain(SampleType level) { engine_.set_sustain(level); }
    void set_attack_curve(float c) { engine_.set_attack_curve(c); }
    void set_decay_curve(float c) { engine_.set_decay_curve(c); }
    void set_curve(double c) {
        const float curve = static_cast<float>(std::clamp(c, 0.0, 1.0));
        engine_.set_attack_curve(-curve);
        engine_.set_decay_curve(curve);
        engine_.set_release_curve(curve);
    }
    void set_loop(bool on, int count = 0) { engine_.set_loop(on, count); }

    /// Signed, -1..+1. Applied by `modulation()`, not by `next()`.
    void set_depth(SampleType d) { depth_ = std::clamp(d, SampleType{-1}, SampleType{1}); }

    void trigger(SampleType velocity = SampleType{1}) {
        legacy_scaled_next_ = false;
        engine_.set_has_sustain(false);
        engine_.set_gate_driven(false);
        engine_.trigger(velocity);
    }
    void gate_on() {
        legacy_scaled_next_ = true;
        engine_.set_has_sustain(true);
        engine_.set_gate_driven(true);
        engine_.trigger(SampleType{1});
    }
    void gate_off() { engine_.release(); }
    void reset() { engine_.reset(); }

    /// Raw unipolar level, 0..1.
    SampleType next() {
        const SampleType level = engine_.next();
        return legacy_scaled_next_ ? level * depth_ : level;
    }

    /// The last `next()` scaled by the signed depth — the value to hand a
    /// modulation destination.
    SampleType modulation() const { return engine_.current() * depth_; }

    SampleType current() const { return engine_.current(); }
    SampleType value() const {
        return legacy_scaled_next_ ? engine_.current() * depth_ : engine_.current();
    }
    bool active() const { return engine_.active(); }
    EnvelopeStage stage() const { return engine_.stage(); }

private:
    detail::EnvelopeEngine<SampleType> engine_{};
    SampleType depth_ = SampleType{1};
    bool legacy_scaled_next_ = false;
};

using ModEnv = ModEnvT<float>;
using ModEnv64 = ModEnvT<double>;

/// Level-independent transient detector: the normalized excess of a fast
/// envelope follower over a slow one.
///
///     out = max(0, (fast - slow) / max(slow, epsilon))
///
/// The division is what makes it level-independent: the same musical gesture at
/// -6 dBFS and at -30 dBFS produces the same output, because both followers
/// scale with the input and the ratio does not. An absolute-difference detector
/// needs its threshold re-tuned for every source; this one does not.
///
/// RT contract: two `BallisticsFilterT` instances of scalar state.
/// `process()` and `reset()` allocate nothing.
///
/// USE: input duckers; adaptive transient shapers; "how hard was that hit" as a
/// modulation source; striking an `LpgT` from a live input's attacks so an
/// effect breathes with the playing rather than with a fixed clock.
template <typename SampleType = float>
class TransientDetectorT {
public:
    /// Fast enough to sit on the attack itself.
    static constexpr SampleType kDefaultFastMs = SampleType{2};
    /// Slow enough to represent "the level this material has been running at".
    static constexpr SampleType kDefaultSlowMs = SampleType{40};

    void prepare(SampleType sample_rate) {
        // RMS rather than peak on both followers: a peak follower fast enough
        // to sit on an attack also tracks the waveform ripple of any low tone,
        // which reads as a permanent 20% transient on a held bass note. RMS
        // averages the ripple out without slowing the attack response.
        fast_.set_mode(BallisticsFilterT<SampleType>::Mode::rms);
        slow_.set_mode(BallisticsFilterT<SampleType>::Mode::rms);
        fast_.prepare(sample_rate);
        slow_.prepare(sample_rate);
        apply_times_();
    }

    void set_fast_ms(SampleType ms) {
        fast_ms_ = std::max(SampleType{0.01}, ms);
        apply_times_();
    }

    void set_slow_ms(SampleType ms) {
        slow_ms_ = std::max(SampleType{0.01}, ms);
        apply_times_();
    }

    void reset() {
        fast_.reset();
        slow_.reset();
    }

    SampleType process(SampleType input) {
        const SampleType magnitude = std::abs(input);
        const SampleType f = fast_.process(magnitude);
        const SampleType s = slow_.process(magnitude);
        // The floor only engages in near-silence, well below any level where a
        // transient means anything, so it does not break scale invariance in
        // the range the detector is actually used in.
        const SampleType denominator = std::max(s, SampleType{1e-9});
        return std::max(SampleType{0}, (f - s) / denominator);
    }

private:
    void apply_times_() {
        // Symmetric ballistics on both followers: the detector compares two
        // time scales, and an asymmetric follower would make the comparison
        // depend on whether the signal happens to be rising.
        fast_.set_attack_ms(fast_ms_);
        fast_.set_release_ms(fast_ms_);
        slow_.set_attack_ms(slow_ms_);
        slow_.set_release_ms(slow_ms_);
    }

    BallisticsFilterT<SampleType> fast_{};
    BallisticsFilterT<SampleType> slow_{};
    SampleType fast_ms_ = kDefaultFastMs;
    SampleType slow_ms_ = kDefaultSlowMs;
};

using TransientDetector = TransientDetectorT<float>;
using TransientDetector64 = TransientDetectorT<double>;

} // namespace pulp::signal
