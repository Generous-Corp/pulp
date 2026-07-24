#pragma once

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// Per-sample multiplier for an exponential decay with time constant `ms`.
///
/// After `ms` milliseconds the envelope has fallen to 1/e (about -8.7 dB) of
/// its starting value. This is the RC convention: it describes a capacitor
/// discharging, which is what an analogue percussion envelope is.
inline double exp_decay_coefficient(double ms, double sample_rate) {
    const double samples = 0.001 * std::max(ms, 1e-6) * sample_rate;
    return std::exp(-1.0 / std::max(samples, 1e-9));
}

/// Per-sample multiplier for a decay that reaches -60 dB after `ms`.
///
/// This is the T60 convention used to describe how long a struck body rings.
/// It is the natural unit for a resonator's decay time and for any control
/// labelled "decay" in seconds, because it matches what a listener hears as
/// the end of the sound; `exp_decay_coefficient` is the natural unit when
/// modelling a specific RC network.
inline double t60_decay_coefficient(double ms, double sample_rate) {
    const double samples = 0.001 * std::max(ms, 1e-6) * sample_rate;
    return std::pow(10.0, -3.0 / std::max(samples, 1e-9));
}

/// A one-shot percussion envelope: linear attack ramp, optional hold, then
/// exponential decay.
///
/// The attack ramp exists to stop a click. Starting an envelope at full scale
/// puts a step edge into the signal, which is broadband and lands on top of
/// whatever transient the voice was designed to have. A ramp of a fraction of a
/// millisecond removes the step without softening the attack audibly, so the
/// default is deliberately short rather than musically chosen.
///
/// The decay is exponential and never reaches zero analytically, so the
/// envelope declares itself finished once it falls below `silence_floor` and
/// then outputs exact zero. A voice polls `is_active()` to know when it can
/// stop rendering, which is what keeps an idle drum kit off the CPU.
///
/// The decay time may be expressed either as an RC time constant
/// (`set_decay_time_constant_ms`) or as a T60 (`set_decay_t60_ms`); the two
/// setters are alternatives, and the most recent call wins.
///
/// RT contract: every member allocates nothing and takes no locks.
template <typename SampleType = float>
class DecayEnvelopeT {
public:
    /// Level below which the envelope is treated as finished. -100 dB is far
    /// under the noise floor of any format Pulp renders to.
    static constexpr double silence_floor = 1e-5;

    /// A default-constructed envelope has usable coefficients at 44.1 kHz, so
    /// triggering one before any setter runs produces the documented default
    /// contour rather than instant silence.
    DecayEnvelopeT() { update(); }

    void set_sample_rate(double sr) {
        sample_rate_ = sr > 0.0 ? sr : sample_rate_;
        update();
    }

    /// Length of the anti-click ramp. Clamped at zero; a zero-length attack
    /// makes the envelope jump to full scale on the first sample.
    void set_attack_ms(double ms) {
        attack_ms_ = std::max(ms, 0.0);
        update();
    }

    /// Time at full scale between the attack and the decay.
    void set_hold_ms(double ms) {
        hold_ms_ = std::max(ms, 0.0);
        update();
    }

    /// Decay expressed as an RC time constant (1/e after `ms`).
    void set_decay_time_constant_ms(double ms) {
        decay_ms_ = std::max(ms, 0.0);
        decay_is_t60_ = false;
        update();
    }

    /// Decay expressed as a T60 (-60 dB after `ms`).
    void set_decay_t60_ms(double ms) {
        decay_ms_ = std::max(ms, 0.0);
        decay_is_t60_ = true;
        update();
    }

    double attack_ms() const { return attack_ms_; }
    double hold_ms() const { return hold_ms_; }
    double decay_ms() const { return decay_ms_; }

    /// Starts the envelope from silence. `peak` scales the whole contour, which
    /// is how velocity reaches the amplitude path.
    void trigger(SampleType peak = SampleType{1}) {
        peak_ = static_cast<double>(peak);
        level_ = 0.0;
        attack_phase_ = 0.0;
        hold_remaining_ = hold_samples_;
        stage_ = attack_samples_ > 0.0 ? Stage::attack : Stage::hold;
        if (stage_ == Stage::hold) level_ = 1.0;
        active_ = peak_ != 0.0;
    }

    /// Silences the envelope immediately without a release ramp. Used by
    /// `reset()` paths and by a hard choke where the caller supplies its own
    /// fade.
    void reset() {
        level_ = 0.0;
        attack_phase_ = 0.0;
        hold_remaining_ = 0;
        stage_ = Stage::idle;
        active_ = false;
    }

    bool is_active() const { return active_; }

    /// Next envelope sample in [0, peak].
    SampleType process() {
        if (!active_) return SampleType{0};

        switch (stage_) {
            case Stage::attack:
                attack_phase_ += attack_step_;
                if (attack_phase_ >= 1.0) {
                    attack_phase_ = 1.0;
                    stage_ = Stage::hold;
                }
                level_ = attack_phase_;
                break;

            case Stage::hold:
                level_ = 1.0;
                if (hold_remaining_ > 0) {
                    --hold_remaining_;
                } else {
                    stage_ = Stage::decay;
                }
                break;

            case Stage::decay:
                level_ = snap_to_zero(level_ * decay_coef_);
                if (level_ < silence_floor) {
                    level_ = 0.0;
                    active_ = false;
                    stage_ = Stage::idle;
                }
                break;

            case Stage::idle:
                level_ = 0.0;
                break;
        }

        return static_cast<SampleType>(level_ * peak_);
    }

    /// Current level without advancing, scaled by the trigger peak. Voices that
    /// need the same envelope value on more than one path read it once through
    /// `process()` and reuse it; this accessor is for the cases where a caller
    /// genuinely wants to observe without consuming.
    SampleType value() const { return static_cast<SampleType>(level_ * peak_); }

private:
    enum class Stage { idle, attack, hold, decay };

    void update() {
        attack_samples_ = 0.001 * attack_ms_ * sample_rate_;
        attack_step_ = attack_samples_ > 0.0 ? 1.0 / attack_samples_ : 1.0;
        hold_samples_ = static_cast<long>(0.001 * hold_ms_ * sample_rate_);
        decay_coef_ = decay_is_t60_ ? t60_decay_coefficient(decay_ms_, sample_rate_)
                                    : exp_decay_coefficient(decay_ms_, sample_rate_);
    }

    double sample_rate_ = 44100.0;
    double attack_ms_ = 0.5;
    double hold_ms_ = 0.0;
    double decay_ms_ = 200.0;
    bool decay_is_t60_ = false;

    double attack_samples_ = 0.0;
    double attack_step_ = 1.0;
    long hold_samples_ = 0;
    double decay_coef_ = 0.0;

    Stage stage_ = Stage::idle;
    double level_ = 0.0;
    double peak_ = 1.0;
    double attack_phase_ = 0.0;
    long hold_remaining_ = 0;
    bool active_ = false;
};

using DecayEnvelope = DecayEnvelopeT<float>;
using DecayEnvelope64 = DecayEnvelopeT<double>;

}  // namespace pulp::signal
