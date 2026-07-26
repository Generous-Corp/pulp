#pragma once

/// @file lpg.hpp
/// Buchla-style low-pass gate: one control drives amplitude and brightness
/// together, through a vactrol model.
///
/// The vactrol response follows the model family in Parker & D'Angelo,
/// "A digital model of the Buchla lowpass-gate" (DAFx-13): a fast light-onset
/// rise, a slow and level-dependent dark recovery, and a control-to-amplitude
/// power law separate from the control-to-cutoff exponential.
///
/// RT contract: `prepare()` and the `set_*()` setters are control-side calls;
/// `strike()`, `set_gate()`, `process()`, `reset()`, and the accessors allocate
/// nothing and are audio-thread safe. The type owns no memory.
///
/// USE — the physics being borrowed, and why it is worth borrowing:
///
/// A vactrol is an LED facing a photoresistor in one package; the Buchla 292
/// used a single cell to control amplitude and cutoff at once. The
/// photoresistor responds to light onset in about a millisecond but recovers
/// its dark resistance slowly and nonlinearly — a fast snap open, then a long
/// drooping close. Two consequences make it musical. First, **loudness and
/// brightness co-vary**, which is how every acoustic percussive source behaves:
/// as a struck object's level falls, its highs die first. A pinged LPG reads as
/// *a thing that was hit*, not as a synth patch. Second, the closing is **not
/// exponential** — fast at first, then lengthening — which is exactly the decay
/// contour of struck wood and skin that a plain RC envelope misses.
///
/// **The core patch idioms:**
///
/// 1. **The ping** — `strike(velocity)` with no envelope anywhere. The LPG *is*
///    the envelope. Material comes from two controls: `decay` is the size of
///    the object, `colour` is its surface. Starting points: decay 60 ms +
///    colour 0.25 is a woodblock or clave; decay 150 ms + colour 0.5 is the
///    classic bongo; decay 350 ms + colour 0.75 is a marimba or soft tom; decay
///    500 ms and above with a reduced `fc_max` is a felt mallet. Ping anything
///    — a noise burst becomes a shaker, a wavetable becomes mallet keys, a
///    chord stab becomes a plucked ensemble.
/// 2. **Velocity done right with one wire** — `strike(velocity_to_strike(v))`.
///    Harder hits get louder *and* brighter in the physically coupled ratio,
///    with zero extra routing. Per-voice velocity-to-timbre is a debt in most
///    drum synths; the LPG pays it structurally.
/// 3. **The roll** — a re-strike mid-decay starts from the vactrol's *current*
///    state, so fast rolls accumulate: each hit lands slightly louder and
///    brighter than a cold one. That accumulation is why LPG rolls crescendo
///    naturally where retriggered-envelope rolls sound like a machine gun.
///    `BurstGenT -> LpgT` is the two-primitive roll patch.
/// 4. **Gated, not pinged** — feed a sustained control with `set_gate()` (from
///    an `ArT`, or a fader) instead of strikes: swells open from darkness to
///    brightness in one gesture. An `ArT -> LpgT` pad reads more bowed or blown
///    than `ArT -> VcaT`, because the spectrum moves with the level.
/// 5. **In feedback loops** — an LPG in a delay's feedback path makes each echo
///    quieter *and* darker, the way real rooms absorb. A static lowpass gives
///    constant colour at falling level, which is the giveaway of a cheap echo.
///    Strike the loop's LPG from the input's `TransientDetectorT` and the
///    echoes duck open with the playing: a live, breathing echo from three
///    primitives.
/// 6. **Stereo pairs** — two LPGs with strike levels or decays decorrelated by
///    10-15% give instant natural width on percussive material. Nothing is
///    identical twice in the physical world; make the two sides disagree
///    slightly.
///
/// **When not to use it:** sustained material that needs constant timbre — that
/// is `VcaT`. Precise transient design — that is the envelope family. The
/// vactrol's lag and droop are *character*, and character is the wrong tool
/// when you need accuracy.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// Vactrol-modelled low-pass gate.
template <typename SampleType = float>
class LpgT {
public:
    /// Photoresistor light-onset time constant.
    static constexpr double kRiseMs = 1.5;

    /// Length of the light pulse a `strike()` produces, at two rise time
    /// constants. This is the parameter that produces idiom 3: a cold strike
    /// reaches about 86% of the strike level, and a strike landing on a decaying
    /// cell starts from whatever is left, so successive hits in a roll climb.
    /// A pulse much longer than the rise time would saturate the cell and the
    /// accumulation would vanish.
    static constexpr double kStrikePulseMs = 3.0;

    /// Control-to-amplitude power law.
    static constexpr float kVcaExponent = 1.5f;

    static constexpr float kDefaultFcMinHz = 40.0f;
    static constexpr float kDefaultFcMaxHz = 12000.0f;
    static constexpr double kDefaultDecayMs = 150.0;

    /// How much the closing slows as the cell darkens. 0 is a plain
    /// exponential; the default gives the lengthening tail that separates a
    /// struck object from an RC circuit. Capped below 1 because a droop of 1
    /// would stall the decay at zero instead of reaching it.
    static constexpr float kDefaultDroop = 0.5f;
    static constexpr float kMaxDroop = 0.95f;

    /// Velocity-to-strike exponent for idiom 2.
    static constexpr float kVelocityCurve = 0.7f;

    /// Map a 0..1 velocity to a strike level whose loudness and brightness rise
    /// together the way a struck object's do.
    static SampleType velocity_to_strike(SampleType velocity) {
        const SampleType v = std::clamp(velocity, SampleType{0}, SampleType{1});
        return static_cast<SampleType>(std::pow(static_cast<float>(v), kVelocityCurve));
    }

    void prepare(SampleType sample_rate) {
        // The filter accepts cutoffs down to 1 Hz, so its Nyquist-derived
        // upper bound must not fall below that lower bound.
        sample_rate_ = sample_rate >= SampleType{4} ? sample_rate : SampleType{4};
        filter_.prepare(sample_rate_);
        update_coefficients_();
        reset();
    }

    /// Dark-recovery time. 50-500 ms is the musical range; the value is the
    /// one-pole time constant of the closing, before the droop lengthens it.
    void set_decay_ms(double ms) {
        decay_ms_ = std::clamp(ms, 1.0, 5000.0);
        update_coefficients_();
    }

    void set_rise_ms(double ms) {
        rise_ms_ = std::clamp(ms, 0.05, 100.0);
        update_coefficients_();
    }

    /// 0 = pure VCA (the filter stays wide open), 1 = pure filter (the gain
    /// stays at unity), between = both.
    void set_colour(float c) { colour_ = std::clamp(c, 0.0f, 1.0f); }

    /// The effective ceiling is additionally capped at `sample_rate / 4` in
    /// `process()` — see the note there.
    void set_range_hz(float min_hz, float max_hz) {
        fc_min_ = std::clamp(min_hz, 10.0f, 20000.0f);
        fc_max_ = std::clamp(max_hz, fc_min_ * 1.01f, 20000.0f);
        log_range_ = std::log(fc_max_ / fc_min_);
    }

    void set_droop(float d) { droop_ = std::clamp(d, 0.0f, kMaxDroop); }

    /// Ping the cell. The strike accumulates onto whatever charge is left, which
    /// is idiom 3.
    void strike(SampleType level) {
        pulse_level_ = std::clamp(level, SampleType{0}, SampleType{1});
        pulse_remaining_ = pulse_samples_;
    }

    /// Sustained control level, for idiom 4. Held until changed; a strike
    /// briefly overrides it upward.
    void set_gate(SampleType level) {
        gate_ = std::clamp(level, SampleType{0}, SampleType{1});
    }

    void reset() {
        control_ = SampleType{0};
        gate_ = SampleType{0};
        pulse_level_ = SampleType{0};
        pulse_remaining_ = 0;
        filter_.reset();
        command_cutoff_();
    }

    SampleType process(SampleType input) {
        advance_control_();

        // Amplitude and brightness are the same control through two different
        // laws — that coupling is the entire effect.
        const SampleType amplitude = control_ * std::sqrt(control_); // control^1.5
        // The commanded cutoff is capped at sample_rate / 4. Above that the
        // trapezoidal one-pole's step response exceeds one: near-Nyquist
        // content pumps the integrator state past the input bound, and the
        // next low-frequency sample reads it back out as a peak above the
        // input's — up to ~1.5x at 18 kHz / 44.1 kHz. At or below the cap
        // every state update is a convex combination, so peak out <= peak in
        // holds unconditionally, which is the no-boost contract the LPG's
        // consumers (and its own "can only attenuate" gain law) rely on.
        command_cutoff_();

        const SampleType filtered = filter_.process_lowpass(input);
        const SampleType gain =
            amplitude + static_cast<SampleType>(colour_) * (SampleType{1} - amplitude);
        return filtered * gain;
    }

    /// Conditioned cell state, 0..1 — the value both the amplitude and the
    /// cutoff are derived from. Useful as a modulation source in its own right.
    SampleType control() const { return control_; }

    /// Effective cutoff currently commanded to the filter. `reset()` commands
    /// the cutoff derived from the reset cell state before updating this
    /// telemetry, so the value never describes a coefficient the filter does
    /// not hold. This is read-only telemetry for proving the brightness half
    /// of the coupled vactrol response; it does not participate in processing.
    SampleType cutoff_hz() const { return cutoff_hz_; }

private:
    void command_cutoff_() {
        const float filter_control =
            1.0f - colour_ * (1.0f - static_cast<float>(control_));
        const auto cutoff =
            static_cast<SampleType>(fc_min_ * std::exp(log_range_ * filter_control));
        cutoff_hz_ = std::min(cutoff, SampleType{0.25} * sample_rate_);
        filter_.set_cutoff(cutoff_hz_);
    }

    void update_coefficients_() {
        rise_coeff_ = coeff_for_(rise_ms_);
        fall_coeff_ = coeff_for_(decay_ms_);
        pulse_samples_ =
            static_cast<long long>(std::lround(kStrikePulseMs * 0.001 * static_cast<double>(sample_rate_)));
        if (pulse_samples_ < 1) pulse_samples_ = 1;
    }

    SampleType coeff_for_(double ms) const {
        const double samples = std::max(1.0e-6, ms * 0.001 * static_cast<double>(sample_rate_));
        return static_cast<SampleType>(1.0 - std::exp(-1.0 / samples));
    }

    void advance_control_() {
        SampleType target = gate_;
        if (pulse_remaining_ > 0) {
            target = std::max(target, pulse_level_);
            --pulse_remaining_;
        }
        if (target > control_) {
            control_ += rise_coeff_ * (target - control_);
        } else {
            // The droop is what makes the close non-exponential: as the cell
            // darkens the recovery slows, which is the decay contour of struck
            // wood and skin. droop = 0 recovers the plain first-order model.
            const SampleType coeff =
                fall_coeff_ * (SampleType{1}
                               - static_cast<SampleType>(droop_) * (SampleType{1} - control_));
            control_ += coeff * (target - control_);
        }
        control_ = std::clamp(snap_to_zero(control_), SampleType{0}, SampleType{1});
    }

    TptFilterT<SampleType> filter_{};
    SampleType sample_rate_ = SampleType{48000};
    SampleType control_ = SampleType{0};
    SampleType cutoff_hz_ = static_cast<SampleType>(kDefaultFcMinHz);
    SampleType gate_ = SampleType{0};
    SampleType pulse_level_ = SampleType{0};
    SampleType rise_coeff_ = SampleType{0};
    SampleType fall_coeff_ = SampleType{0};
    long long pulse_remaining_ = 0;
    long long pulse_samples_ = 144;
    double rise_ms_ = kRiseMs;
    double decay_ms_ = kDefaultDecayMs;
    float fc_min_ = kDefaultFcMinHz;
    float fc_max_ = kDefaultFcMaxHz;
    float log_range_ = 5.7037825f; // log(12000 / 40)
    float colour_ = 0.5f;
    float droop_ = kDefaultDroop;
};

using Lpg = LpgT<float>;
using Lpg64 = LpgT<double>;

} // namespace pulp::signal
