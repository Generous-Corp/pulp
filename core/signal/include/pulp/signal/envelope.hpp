#pragma once

/// @file envelope.hpp
/// The envelope family: one segment engine, six named shapes.
///
/// `AdsrT` (adsr.hpp) already covers the keyboard envelope — the one with a
/// sustain that holds while a key is down. This header covers the rest of the
/// family, which is what modulation actually uses: percussive shapes with no
/// sustain at all (`ArT`, `AdT`, `AhdT`), the full modular shape with a
/// pre-delay and a hold (`DahdsrT`), and a bipolar variant for modulation
/// destinations that go both ways (`ModEnvT`).
///
/// They are all the same machine. Rather than six hand-written state machines
/// that each get the "what happens if the gate falls during attack" question
/// slightly differently right, there is one `EnvelopeCore` walking a fixed
/// segment list, and each named type is that core with segments enabled or
/// disabled. The behaviours a spec has to be able to rely on are therefore
/// decided once:
///
///   - **A gate release during any stage jumps straight to release**, from
///     wherever the level currently is. No stage is "uninterruptible"; a
///     staccato note on a 2-second attack must not hang for 2 seconds.
///   - **Retrigger continues from the current level**, it does not restart
///     from zero. Restarting from zero clicks, and the click is not a
///     character choice anybody asked for.
///   - **Times are real milliseconds** (series law 3), and every stage's curve
///     is the same exponential-approach shape, stated as a `curve` in
///     `[0, 1]`: 0 is linear, 1 is a strongly exponential approach. One curve
///     control, applied identically to every segment, rather than a per-stage
///     shape nobody can predict the sum of.
///   - **There is no randomness**, so determinism (series law 2) is trivial.
///
/// Curve law: a segment of length `L` samples advances a normalised position
/// `p` from 0 to 1 linearly, and the output is `shape(p)` where
/// `shape(p) = (1 − exp(−k·p)) / (1 − exp(−k))` with `k = kCurveKnee·curve`.
/// At `curve = 0` the limit is exactly `p` (linear); as `curve` rises the
/// segment covers more of its distance early, which is what a discharging
/// capacitor does and what a decay needs to sound natural. The normalisation
/// means a segment ALWAYS arrives exactly at its endpoint in exactly `L`
/// samples, whatever the curve — so "a 200 ms decay" is 200 ms at every curve
/// setting, which a linear-vs-exponential switch usually gets wrong.
///
/// RT contract: `prepare()` recomputes per-stage sample counts and allocates
/// nothing. `set_*`, `gate_on()`, `gate_off()`, `next()`, and `reset()`
/// allocate nothing, take no locks, and perform no I/O. `next()` costs one
/// `std::exp` per sample on curved segments; a caller modulating at block rate
/// can run the envelope at block rate instead.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// Which segment an envelope is currently in.
enum class EnvelopeStage {
    idle,     ///< Finished (or never started). Output is 0.
    delay,    ///< Waiting before the attack. Output holds at 0.
    attack,   ///< Rising to peak.
    hold,     ///< Holding at peak for a fixed time.
    decay,    ///< Falling from peak to sustain.
    sustain,  ///< Holding at sustain while the gate is high.
    release,  ///< Falling to 0 after the gate falls.
};

/// The shared segment engine. The named envelopes below are this core with
/// stages enabled; it is not intended to be instantiated directly, but it is
/// public because the named types are aliases over it rather than wrappers,
/// and because a module with a genuinely unusual shape should configure this
/// rather than write a seventh state machine.
template <typename SampleType = float>
class EnvelopeCore {
public:
    using Stage = EnvelopeStage;

    /// Maximum exponent of the curve law. At `curve = 1` a segment covers
    /// `1 − 1/e^kCurveKnee` ≈ 99.3 % of its distance in the first half — steep
    /// enough to read as "snappy", not so steep that the tail vanishes into
    /// denormals.
    /// [design parameter] default 5.0, range 2.0 .. 10.0.
    static constexpr double kCurveKnee = 5.0;

    /// Floor on a stage time. A zero-length stage is legal and means "skip",
    /// but the sample count must never be zero inside the ramp arithmetic.
    /// [design parameter] default 1e-4 ms, range 1e-6 .. 1e-2 ms.
    static constexpr double kMinStageMs = 1e-4;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update();
    }

    void set_delay_ms(double ms) { delay_ms_ = std::max(ms, 0.0); update(); }
    void set_attack_ms(double ms) { attack_ms_ = std::max(ms, 0.0); update(); }
    void set_hold_ms(double ms) { hold_ms_ = std::max(ms, 0.0); update(); }
    void set_decay_ms(double ms) { decay_ms_ = std::max(ms, 0.0); update(); }
    void set_release_ms(double ms) { release_ms_ = std::max(ms, 0.0); update(); }

    /// Sustain level in `[0, 1]`. Ignored by the shapes that have no sustain.
    void set_sustain(double level) { sustain_ = std::clamp(level, 0.0, 1.0); }

    /// Segment curvature in `[0, 1]`: 0 linear, 1 strongly exponential.
    void set_curve(double curve) {
        curve_ = std::clamp(curve, 0.0, 1.0);
        curve_k_ = kCurveKnee * curve_;
        // Precomputed so the per-sample shape is one exp and one multiply.
        curve_norm_ = curve_k_ > 0.0 ? 1.0 / (1.0 - std::exp(-curve_k_)) : 0.0;
    }

    double sustain() const { return sustain_; }
    double curve() const { return curve_; }

    /// Enables or disables individual stages. This is how the named shapes are
    /// built: `ArT` disables delay, hold, decay, and sustain.
    void enable_stages(bool delay, bool hold, bool decay, bool sustain) {
        has_delay_ = delay;
        has_hold_ = hold;
        has_decay_ = decay;
        has_sustain_ = sustain;
    }

    /// Starts (or retriggers) the envelope. Retrigger continues from the
    /// current level rather than restarting from zero, so a fast repeat does
    /// not click.
    void gate_on() {
        gate_ = true;
        position_ = 0.0;
        segment_start_ = level_;
        stage_ = has_delay_ && delay_samples_ > 0.0 ? Stage::delay : Stage::attack;
        if (stage_ == Stage::attack) segment_target_ = 1.0;
    }

    /// Releases the envelope from wherever it currently is. Legal in any
    /// stage — a short note on a long attack releases immediately.
    void gate_off() {
        gate_ = false;
        if (stage_ == Stage::idle) return;
        stage_ = Stage::release;
        position_ = 0.0;
        segment_start_ = level_;
        segment_target_ = 0.0;
    }

    void reset() {
        stage_ = Stage::idle;
        level_ = 0.0;
        position_ = 0.0;
        segment_start_ = 0.0;
        segment_target_ = 0.0;
        gate_ = false;
    }

    Stage stage() const { return stage_; }
    bool active() const { return stage_ != Stage::idle; }
    SampleType value() const { return static_cast<SampleType>(level_); }

    /// Advances one sample and returns the new level in `[0, 1]`.
    SampleType next() {
        switch (stage_) {
            case Stage::idle:
                return SampleType{0};

            case Stage::delay:
                if (advance(delay_samples_)) enter_attack();
                level_ = 0.0;
                break;

            case Stage::attack:
                level_ = ramp(attack_samples_);
                if (position_ >= 1.0) enter_after_attack();
                break;

            case Stage::hold:
                level_ = 1.0;
                if (advance(hold_samples_)) enter_after_hold();
                break;

            case Stage::decay:
                level_ = ramp(decay_samples_);
                if (position_ >= 1.0) enter_after_decay();
                break;

            case Stage::sustain:
                // `segment_start_` is the level this stage was entered at. For
                // a shape with no sustain segment — `ArT`, or `AhdT` with hold
                // disabled — that is the PEAK, and holding there is the whole
                // meaning of "the shape ends at the peak and a held gate holds
                // it". Reading `sustain_` unconditionally instead parked those
                // shapes at an unrelated member (default 0.7), stepping the
                // output down 3.1 dB in a single sample the moment attack
                // finished.
                level_ = has_sustain_ ? sustain_ : segment_start_;
                break;

            case Stage::release:
                level_ = ramp(release_samples_);
                if (position_ >= 1.0) {
                    level_ = 0.0;
                    stage_ = Stage::idle;
                }
                break;
        }
        level_ = snap_to_zero(level_);
        return static_cast<SampleType>(level_);
    }

private:
    /// Advances `position_` by one sample of a `samples`-long stage. Returns
    /// true once the stage is complete.
    bool advance(double samples) {
        position_ += samples > 0.0 ? 1.0 / samples : 1.0;
        return position_ >= 1.0;
    }

    /// One sample of a curved ramp from `segment_start_` to `segment_target_`.
    double ramp(double samples) {
        advance(samples);
        const double p = std::min(position_, 1.0);
        return segment_start_ + (segment_target_ - segment_start_) * shape(p);
    }

    double shape(double p) const {
        if (curve_k_ <= 0.0) return p;
        return (1.0 - std::exp(-curve_k_ * p)) * curve_norm_;
    }

    void enter_attack() {
        stage_ = Stage::attack;
        position_ = 0.0;
        segment_start_ = level_;
        segment_target_ = 1.0;
    }

    void enter_after_attack() {
        level_ = 1.0;
        position_ = 0.0;
        segment_start_ = 1.0;
        if (has_hold_ && hold_samples_ > 0.0) {
            stage_ = Stage::hold;
        } else {
            enter_after_hold();
        }
    }

    void enter_after_hold() {
        position_ = 0.0;
        segment_start_ = level_;
        if (has_decay_) {
            stage_ = Stage::decay;
            segment_target_ = has_sustain_ ? sustain_ : 0.0;
        } else if (has_sustain_) {
            stage_ = Stage::sustain;
        } else {
            // No decay and no sustain: the shape ends at the peak, so a gate
            // that is still high holds there and a gate that has fallen
            // releases. `AhdT` with hold disabled degenerates to this.
            stage_ = gate_ ? Stage::sustain : Stage::release;
            segment_target_ = 0.0;
        }
    }

    void enter_after_decay() {
        level_ = has_sustain_ ? sustain_ : 0.0;
        position_ = 0.0;
        segment_start_ = level_;
        if (has_sustain_) {
            stage_ = Stage::sustain;
        } else {
            level_ = 0.0;
            stage_ = Stage::idle;
        }
    }

    void update() {
        // NOT `stage_samples()`. That floors every stage at `kMinStageMs` so a
        // ramp can never divide by zero — but a delay is a WAIT, not a ramp,
        // and zero is a meaningful, safe value for it (`advance()` completes a
        // zero-length stage immediately). Flooring it meant `delay_samples_`
        // was nonzero even when the caller asked for no delay, so `gate_on()`
        // always routed through the delay stage, which forces the level to
        // zero. A retrigger therefore punched a full-scale one-sample notch —
        // a broadband click — into every delay-capable shape, contradicting
        // this class's own promise that retrigger continues from the current
        // level.
        delay_samples_ = units::ms_to_samples(std::max(delay_ms_, 0.0), sample_rate_);
        attack_samples_ = stage_samples(attack_ms_);
        hold_samples_ = stage_samples(hold_ms_);
        decay_samples_ = stage_samples(decay_ms_);
        release_samples_ = stage_samples(release_ms_);
    }

    double stage_samples(double ms) const {
        return units::ms_to_samples(std::max(ms, kMinStageMs), sample_rate_);
    }

    double sample_rate_ = 44100.0;
    double delay_ms_ = 0.0;
    double attack_ms_ = 5.0;
    double hold_ms_ = 0.0;
    double decay_ms_ = 200.0;
    double release_ms_ = 200.0;
    double sustain_ = 0.7;
    double curve_ = 0.5;
    double curve_k_ = kCurveKnee * 0.5;
    double curve_norm_ = 1.0 / (1.0 - std::exp(-kCurveKnee * 0.5));

    double delay_samples_ = 0.0;
    double attack_samples_ = 1.0;
    double hold_samples_ = 0.0;
    double decay_samples_ = 1.0;
    double release_samples_ = 1.0;

    Stage stage_ = Stage::idle;
    double level_ = 0.0;
    double position_ = 0.0;
    double segment_start_ = 0.0;
    double segment_target_ = 0.0;
    bool gate_ = false;

    bool has_delay_ = true;
    bool has_hold_ = true;
    bool has_decay_ = true;
    bool has_sustain_ = true;
};

/// Attack–Release: rises to peak, then falls. No sustain — a trigger shape, not
/// a gate shape. The percussion envelope.
template <typename SampleType = float>
class ArT : public EnvelopeCore<SampleType> {
public:
    ArT() { this->enable_stages(false, false, false, false); }
};

/// Attack–Decay: rises to peak, then falls to zero without waiting for a gate
/// release. Distinct from `ArT` only in which time control names the fall,
/// which matters because a spec that says "decay" and gets "release" is a spec
/// whose test reads the wrong setter.
template <typename SampleType = float>
class AdT : public EnvelopeCore<SampleType> {
public:
    AdT() { this->enable_stages(false, false, true, false); }
};

/// Attack–Hold–Decay: rises, sits at peak for a stated time, then falls. The
/// shape a gated reverb's envelope and a drum's body both want.
template <typename SampleType = float>
class AhdT : public EnvelopeCore<SampleType> {
public:
    AhdT() { this->enable_stages(false, true, true, false); }
};

/// Delay–Attack–Hold–Decay–Sustain–Release: the full modular shape. The
/// pre-delay is what lets a second envelope arrive after the first, which is
/// how a delayed vibrato or a two-stage swell is built without a sequencer.
template <typename SampleType = float>
class DahdsrT : public EnvelopeCore<SampleType> {
public:
    DahdsrT() { this->enable_stages(true, true, true, true); }
};

/// A `DahdsrT` mapped to bipolar `[-1, +1]` output, with its own depth and
/// polarity — the modulation-destination shape.
///
/// Kept separate from a plain envelope plus an `AttenuverterT` because the
/// combination has a trap: attenuverting a `[0, 1]` envelope to bipolar makes
/// its REST state −depth rather than 0, so an idle envelope silently offsets
/// its destination. This maps rest to 0 and peak to ±depth, which is what
/// "modulation envelope" means everywhere it is used.
///
/// RT contract: as `EnvelopeCore`.
template <typename SampleType = float>
class ModEnvT {
public:
    using Stage = EnvelopeStage;

    void prepare(double sample_rate) { core_.prepare(sample_rate); }

    /// Modulation depth. Negative inverts the envelope's direction.
    void set_depth(double depth) { depth_ = std::clamp(depth, -1.0, 1.0); }
    double depth() const { return depth_; }

    void set_delay_ms(double ms) { core_.set_delay_ms(ms); }
    void set_attack_ms(double ms) { core_.set_attack_ms(ms); }
    void set_hold_ms(double ms) { core_.set_hold_ms(ms); }
    void set_decay_ms(double ms) { core_.set_decay_ms(ms); }
    void set_release_ms(double ms) { core_.set_release_ms(ms); }
    void set_sustain(double level) { core_.set_sustain(level); }
    void set_curve(double curve) { core_.set_curve(curve); }

    void gate_on() { core_.gate_on(); }
    void gate_off() { core_.gate_off(); }
    void reset() { core_.reset(); }

    Stage stage() const { return core_.stage(); }
    bool active() const { return core_.active(); }

    /// Advances one sample. Rest is exactly 0; peak is `depth`.
    SampleType next() {
        return static_cast<SampleType>(static_cast<double>(core_.next()) * depth_);
    }

private:
    EnvelopeCore<SampleType> core_{};
    double depth_ = 1.0;
};

using Ar = ArT<float>;
using Ad = AdT<float>;
using Ahd = AhdT<float>;
using Dahdsr = DahdsrT<float>;
using ModEnv = ModEnvT<float>;

}  // namespace pulp::signal
