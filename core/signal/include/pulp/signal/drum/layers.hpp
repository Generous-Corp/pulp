#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// The transient: a burst of noise through a lowpass, decaying in a few
/// milliseconds.
///
/// Every percussion voice has one, and it is always built the same way,
/// because a strike is broadband and short and a one-pole is what turns that
/// into a specific-sounding strike. The cutoff is the interesting control, not
/// the level: lowering it turns a beater tick into a thump and a rimshot into
/// a body hit, whereas turning the level down just makes the same tick
/// quieter. That is why velocity brightens the click here rather than only
/// scaling it.
///
/// The layer does not own its noise generator. Voices share one generator
/// across their layers so a hit reseeds once and stays reproducible; giving
/// each layer its own would mean each hit's layers were correlated with each
/// other in a way that changes with the layer count.
///
/// RT contract: `prepare()` allocates nothing; `trigger()` and `process()`
/// allocate nothing and take no locks.
class ClickLayer {
public:
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate;
        envelope_.set_sample_rate(sample_rate);
        filter_.prepare(static_cast<float>(sample_rate));
        apply_cutoff();
        reset();
    }

    void reset() {
        envelope_.reset();
        filter_.reset();
    }

    void set_level(double level) {
        level_ = std::max(level, 0.0);
        if (level_ == 0.0) reset();
    }
    void set_cutoff_hz(double hz) {
        cutoff_hz_ = std::clamp(hz, 20.0, 20000.0);
        apply_cutoff();
    }
    void set_decay_ms(double ms) {
        decay_ms_ = std::clamp(ms, 0.05, 200.0);
        envelope_.set_decay_time_constant_ms(decay_ms_);
    }

    double level() const { return level_; }
    double cutoff_hz() const { return cutoff_hz_; }

    /// Starts a click. `brightness` multiplies the cutoff, which is how
    /// velocity reaches this layer's timbre.
    void trigger(double brightness = 1.0) {
        if (level_ <= 0.0) return;
        brightness_ = std::max(brightness, 0.01);
        apply_cutoff();
        // The filter starts clean on every hit: a strike is a fresh event, and
        // leaving state behind would make a hit depend on how long ago the
        // previous one was.
        filter_.reset();
        envelope_.set_attack_ms(0.0);
        envelope_.set_decay_time_constant_ms(decay_ms_);
        envelope_.trigger();
    }

    /// A silent layer is never active. Both halves matter: without the level
    /// term a layer whose level is zero would report itself active forever,
    /// because `process()` returns early and so never advances the envelope
    /// that would eventually clear the flag -- and a voice built from layers
    /// would then never finish and never leave the CPU.
    bool is_active() const { return level_ > 0.0 && envelope_.is_active(); }

    /// Next sample. `noise` is a raw white sample from the voice's generator.
    double process(double noise) {
        if (!envelope_.is_active() || level_ <= 0.0) return 0.0;
        const double filtered = filter_.process_lowpass(static_cast<float>(noise));
        return filtered * envelope_.process() * level_;
    }

private:
    void apply_cutoff() {
        filter_.set_cutoff(static_cast<float>(
            std::min(cutoff_hz_ * brightness_, 0.49 * sample_rate_)));
    }

    double sample_rate_ = 44100.0;
    double level_ = 0.0;
    double cutoff_hz_ = 4000.0;
    double decay_ms_ = 2.0;
    double brightness_ = 1.0;
    DecayEnvelope64 envelope_;
    TptFilter filter_;
};

/// The air a strike moves: a longer, coloured noise layer under the body.
///
/// Distinct from the click in what it is for, not just in how long it lasts.
/// The click is the contact; this is the sound of the head and the room
/// responding to it, which is why it is coloured (the spectrum is tilted) and
/// decays over tens or hundreds of milliseconds rather than a few.
///
/// RT contract: `prepare()` allocates nothing; every other method allocates
/// nothing and takes no locks.
class NoiseLayer {
public:
    void prepare(double sample_rate) {
        envelope_.set_sample_rate(sample_rate);
        reset();
    }

    void reset() { envelope_.reset(); }

    void set_level(double level) {
        level_ = std::max(level, 0.0);
        if (level_ == 0.0) reset();
    }
    void set_decay_ms(double ms) {
        decay_ms_ = std::clamp(ms, 0.5, 4000.0);
        envelope_.set_decay_t60_ms(decay_ms_);
    }

    double level() const { return level_; }

    void trigger() {
        if (level_ <= 0.0) return;
        envelope_.set_attack_ms(0.2);
        envelope_.set_decay_t60_ms(decay_ms_);
        envelope_.trigger();
    }

    /// A silent layer is never active -- see the note on ClickLayer.
    bool is_active() const { return level_ > 0.0 && envelope_.is_active(); }

    /// Next sample. `coloured` is a sample from the voice's generator with its
    /// colour filter applied.
    double process(double coloured) {
        if (!envelope_.is_active() || level_ <= 0.0) return 0.0;
        return coloured * envelope_.process() * level_;
    }

private:
    double level_ = 0.0;
    double decay_ms_ = 60.0;
    DecayEnvelope64 envelope_;
};

/// A high-Q body resonance mixed back into an excitation path.
///
/// Shell resonance is a reusable topology block rather than a snare-specific
/// filter: noise, click, or any authored body block can excite it, and its
/// stored energy intentionally survives triggers until the owning voice is
/// reset. That makes it suitable for genome block composition without
/// duplicating the state rule in every voice.
class ShellLayer {
public:
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        filter_.set_sample_rate(static_cast<float>(sample_rate_));
        filter_.set_mode(Svf::Mode::bandpass);
        apply();
        reset();
    }

    void reset() {
        filter_.reset();
        ring_level_ = 0.0;
    }

    void set_level(double level) {
        level_ = std::max(level, 0.0);
        if (level_ == 0.0) reset();
    }
    void set_frequency_hz(double hz) {
        frequency_hz_ = std::clamp(hz, 20.0, 20000.0);
        apply();
    }
    void set_resonance(double q) {
        resonance_ = std::clamp(q, 1.0, 30.0);
        apply();
    }

    double level() const { return level_; }
    double frequency_hz() const { return frequency_hz_; }
    double resonance() const { return resonance_; }
    bool is_ringing() const {
        return level_ > 0.0 && ring_level_ > kSilenceLevel;
    }

    double process(double excitation) {
        if (level_ <= 0.0) {
            ring_level_ = 0.0;
            return excitation;
        }
        const double resonance = static_cast<double>(
            filter_.process(static_cast<float>(excitation)));
        ring_level_ =
            std::max(std::fabs(resonance), ring_level_ * kLevelDecay);
        return excitation + level_ * resonance;
    }

private:
    void apply() {
        filter_.set_frequency(static_cast<float>(
            std::min(frequency_hz_, 0.49 * sample_rate_)));
        filter_.set_resonance(static_cast<float>(resonance_));
    }

    static constexpr double kSilenceLevel = 1.0e-6;
    static constexpr double kLevelDecay = 0.999;

    double sample_rate_ = 44100.0;
    double level_ = 0.0;
    double frequency_hz_ = 180.0;
    double resonance_ = 12.0;
    double ring_level_ = 0.0;
    Svf filter_;
};

/// A sine an octave below the body, gated by the body's own envelope.
///
/// This is a mastering layer rather than a synthesis one. It exists because
/// the fundamental of a kick often sits where small speakers cannot reproduce
/// it, and an octave-down sine gives the ear a second, lower reference that
/// survives; it is gated by the body so it cannot outlast the note and turn
/// into a drone.
///
/// RT contract: allocates nothing anywhere.
class SubLayer {
public:
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        reset();
    }

    void reset() { phase_ = 0.0; }

    void set_level(double level) { level_ = std::max(level, 0.0); }
    double level() const { return level_; }

    void trigger() { phase_ = 0.0; }

    /// Next sample at half of `body_hz`, scaled by the body's envelope.
    double process(double body_hz, double body_envelope) {
        if (level_ <= 0.0) return 0.0;
        phase_ += 0.5 * body_hz / sample_rate_;
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);
        return std::sin(2.0 * 3.14159265358979323846 * phase_) * body_envelope * level_;
    }

private:
    double sample_rate_ = 44100.0;
    double level_ = 0.0;
    double phase_ = 0.0;
};

}  // namespace pulp::signal::drum
