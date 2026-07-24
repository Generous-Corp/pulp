#pragma once

#include <pulp/signal/bridged_t_resonator.hpp>
#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/two_pole_resonator.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// How a kick's body is produced. The three options are genuinely different
/// instruments rather than three settings of one, which is why this is a mode
/// switch and not a blend control.
enum class KickBody {
    /// A swept oscillator. The pitch envelope is explicit, so the sweep is
    /// exactly what the controls say it is. This is the most controllable
    /// option and the one that reaches the deepest, longest sounds.
    oscillator,

    /// A struck resonance. There is no oscillator and no amplitude envelope on
    /// the body: an exciter rings a resonator and the ring is the sound, so
    /// what the strike sounds like changes what the body sounds like.
    resonant,

    /// The TR-808's bridged-T network in its op-amp feedback loop. Nothing here
    /// is scripted -- the pitch drop is the network retuning itself as its own
    /// ring amplitude falls, and the attack thump is a transistor shunt lifting
    /// the centre frequency for a few milliseconds. The trade is that the
    /// controls are component values and envelope timings, not "pitch" and
    /// "sweep".
    circuit
};

/// A kick drum.
///
/// The layers are the ones every electronic kick has had since the 1970s: a
/// body that carries the pitch, a click that carries the attack, a noise layer
/// that carries the air the beater moves, and an optional sub an octave down
/// for systems that reproduce it. What distinguishes one machine's kick from
/// another's is almost entirely how the body is made, which is what `KickBody`
/// selects.
///
/// Velocity reaches timbre through `VelocityResponse`: harder hits bend the
/// pitch further and brighten the click, which is what a beater striking a head
/// harder actually does. The default response has both enabled, so a caller
/// gets a velocity-sensitive kick without configuring one.
///
/// RT contract: `prepare()` allocates nothing; every other method, including
/// `note_on` and `process`, allocates nothing and takes no locks.
class KickVoice : public Voice {
public:
    KickVoice() {
        VelocityResponse r;
        r.level_db = 14.0f;
        r.bend_octaves = 0.6f;
        r.brightness_octaves = 1.5f;
        set_velocity_response(r);
    }

    // -- Shared controls -----------------------------------------------------

    void set_body(KickBody body) { body_mode_ = body; }
    KickBody body() const { return body_mode_; }

    /// Fundamental of the body, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 20.0, 400.0); }

    /// How long the body rings, as a T60 in milliseconds.
    void set_body_decay_ms(double ms) { body_decay_ms_ = std::clamp(ms, 10.0, 4000.0); }

    /// Depth of the downward pitch sweep, in octaves, and how quickly it
    /// settles. Applies to the oscillator body; the circuit body produces its
    /// sweep from the network and ignores these.
    void set_pitch_sweep_octaves(double octaves) {
        pitch_sweep_oct_ = std::clamp(octaves, 0.0, 6.0);
    }
    void set_pitch_sweep_ms(double ms) { pitch_sweep_ms_ = std::clamp(ms, 0.5, 500.0); }

    /// Level of the attack click, and the corner of the one-pole that tones
    /// it. A click is a noise burst through a lowpass: lowering the corner
    /// turns a beater tick into a thump.
    void set_click_level(double level) { click_level_ = std::max(level, 0.0); }
    void set_click_tone_hz(double hz) { click_tone_hz_ = std::clamp(hz, 100.0, 18000.0); }
    void set_click_decay_ms(double ms) { click_decay_ms_ = std::clamp(ms, 0.2, 100.0); }

    /// Level and decay of the noise layer.
    void set_noise_level(double level) { noise_level_ = std::max(level, 0.0); }
    void set_noise_decay_ms(double ms) { noise_decay_ms_ = std::clamp(ms, 1.0, 2000.0); }
    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    /// Level of the sub layer, a sine an octave below the body gated by the
    /// body's own envelope.
    void set_sub_level(double level) { sub_level_ = std::max(level, 0.0); }

    /// The saturation and degradation stage the voice ends with.
    OutputStage& output() { return output_; }

    // -- Oscillator-body controls -------------------------------------------

    /// Triangle instead of sine. A triangle body carries odd harmonics, which
    /// survive small speakers where a sine disappears.
    void set_oscillator_triangle(bool triangle) { triangle_ = triangle; }

    /// Phase-modulation depth and the modulator's frequency ratio. Modulation
    /// adds inharmonic partials to the attack without lengthening it.
    void set_fm_amount(double amount) { fm_amount_ = std::clamp(amount, 0.0, 8.0); }
    void set_fm_ratio(double ratio) { fm_ratio_ = std::clamp(ratio, 0.25, 16.0); }

    // -- Circuit-body controls ----------------------------------------------

    /// Component values of the bridged-T network. Substituting a capacitor
    /// retunes the drum the way it does on the bench.
    void set_circuit_components(const BridgedTComponents& c) {
        circuit_.set_components(c);
    }

    /// Regeneration around the network, 0 to 1. This is the 808's decay
    /// control: the loop replaces some of what the network dissipates each
    /// cycle, so more feedback is a longer ring rather than a louder one.
    ///
    /// The control is normalised rather than being the raw loop gain, because
    /// the raw gain has a hard ceiling: past about 0.87 the loop replaces more
    /// than the network loses and the drum becomes an oscillator that never
    /// stops. 1 here maps to just under that, which is a very long ring and
    /// still a drum. The real instrument's decay pot has an end stop for the
    /// same reason.
    void set_circuit_feedback(double gain) { circuit_feedback_ = std::clamp(gain, 0.0, 1.0); }

    /// How long the attack shunt grounds the transistor's collector after a
    /// trigger. The shunt lifts the centre frequency by over an octave, so
    /// this is the length of the thump before the note settles.
    void set_circuit_attack_ms(double ms) { circuit_attack_ms_ = std::clamp(ms, 0.0, 50.0); }

    /// Width of the trigger pulse into the pulse shaper, in milliseconds.
    void set_circuit_pulse_ms(double ms) { circuit_pulse_ms_ = std::clamp(ms, 0.2, 20.0); }

    /// Disconnect the leakage path that produces the pitch sigh. Proves the
    /// sigh is emergent rather than scripted, and is a usable sound in itself.
    void set_circuit_sigh(bool enabled) { circuit_.set_sigh_enabled(enabled); }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        body_env_.set_sample_rate(sample_rate);
        pitch_env_.set_sample_rate(sample_rate);
        click_env_.set_sample_rate(sample_rate);
        noise_env_.set_sample_rate(sample_rate);
        resonator_.set_sample_rate(sample_rate);
        circuit_.prepare(sample_rate);
        output_.prepare(sample_rate);
        update_click_filter();
        update_tone_filter();
    }

    void on_reset() override {
        body_env_.reset();
        pitch_env_.reset();
        click_env_.reset();
        noise_env_.reset();
        resonator_.reset();
        circuit_.reset();
        circuit_.set_attack_shunt(false);
        output_.reset();
        noise_.reset();
        phase_ = 0.0;
        mod_phase_ = 0.0;
        sub_phase_ = 0.0;
        click_lp_ = 0.0;
        tone_lp_ = 0.0;
        feedback_z_ = 0.0;
        pulse_shaper_ = 0.0;
        ring_level_ = 0.0;
        pulse_remaining_ = 0;
        shunt_remaining_ = 0;
        excite_remaining_ = 0;
    }

    void on_note_on(float velocity) override {
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        bend_octaves_ = pitch_sweep_oct_ + response.bend(velocity);
        brightness_ = response.brightness_scale(velocity);

        // Reseeding here is what makes a hit reproducible: the same parameters
        // and velocity render the same samples every time.
        noise_.reset();

        pitch_env_.set_decay_time_constant_ms(pitch_sweep_ms_);
        pitch_env_.set_attack_ms(0.0);
        pitch_env_.trigger();

        body_env_.set_attack_ms(0.4);
        body_env_.set_decay_t60_ms(body_decay_ms_);
        body_env_.trigger();

        click_env_.set_attack_ms(0.0);
        click_env_.set_decay_time_constant_ms(click_decay_ms_);
        click_env_.trigger();

        noise_env_.set_attack_ms(0.2);
        noise_env_.set_decay_t60_ms(noise_decay_ms_);
        noise_env_.trigger();

        update_click_filter();
        update_tone_filter();

        // The click's tone filter is part of the strike, not part of the body,
        // so it starts fresh on every hit. Leaving its state behind would make
        // a hit depend on how long ago the previous one was, which is the same
        // non-determinism the noise reseed above exists to remove. The circuit
        // body's own state is deliberately *not* cleared here -- see below.
        click_lp_ = 0.0;

        switch (body_mode_) {
            case KickBody::oscillator:
                phase_ = 0.0;
                mod_phase_ = 0.0;
                sub_phase_ = 0.0;
                break;

            case KickBody::resonant:
                resonator_.set_frequency(tune_hz_);
                resonator_.set_t60_ms(body_decay_ms_);
                // A short broadband burst rings the body. Its length is what
                // the strike sounds like, so it is deliberately sub-millisecond
                // rather than an envelope.
                excite_remaining_ = std::max(
                    1, static_cast<int>(0.0004 * sample_rate()));
                break;

            case KickBody::circuit:
                // The network is deliberately not reset: a trigger adds energy
                // to a body that may still be ringing, which is what stops
                // repeated hits sounding identical and mechanical.
                pulse_remaining_ = std::max(
                    1, static_cast<int>(0.001 * circuit_pulse_ms_ * sample_rate()));
                shunt_remaining_ =
                    static_cast<int>(0.001 * circuit_attack_ms_ * sample_rate());
                circuit_.set_attack_shunt(shunt_remaining_ > 0);
                break;
        }
    }

    bool on_is_active() const override {
        if (body_mode_ == KickBody::circuit) {
            return body_env_.is_active() || click_env_.is_active() ||
                   noise_env_.is_active() || pulse_remaining_ > 0 ||
                   ring_level_ > kRingSilenceLevel;
        }
        if (body_mode_ == KickBody::resonant) {
            return resonator_.is_ringing() || click_env_.is_active() ||
                   noise_env_.is_active() || excite_remaining_ > 0;
        }
        return body_env_.is_active() || click_env_.is_active() || noise_env_.is_active();
    }

    void render_add(float* out, int num_samples) override {
        for (int i = 0; i < num_samples; ++i) {
            const double body_env = body_env_.process();

            double body = 0.0;
            switch (body_mode_) {
                case KickBody::oscillator: body = render_oscillator_body(body_env); break;
                case KickBody::resonant:   body = render_resonant_body(); break;
                case KickBody::circuit:    body = render_circuit_body(); break;
            }

            // The click is a noise burst through a one-pole. Velocity brightens
            // it by moving the corner, which is what a harder beater strike
            // does; scaling its level alone would not change its character.
            const double click_env = click_env_.process();
            double click = 0.0;
            if (click_env > 0.0) {
                click_lp_ += click_a_ * (static_cast<double>(noise_.white()) - click_lp_);
                click = click_lp_ * click_env * click_level_;
            }

            const double noise_env = noise_env_.process();
            const double noise = noise_env > 0.0
                                     ? static_cast<double>(noise_.process()) * noise_env *
                                           noise_level_
                                     : 0.0;

            double sub = 0.0;
            if (sub_level_ > 0.0) {
                sub_phase_ += 0.5 * tune_hz_ / sample_rate();
                if (sub_phase_ >= 1.0) sub_phase_ -= 1.0;
                sub = std::sin(2.0 * 3.14159265358979323846 * sub_phase_) * body_env *
                      sub_level_;
            }

            const double mixed = body + click + noise + sub;
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(mixed)) * velocity_gain_);
        }

        circuit_.snap_denormals();
    }

private:
    double render_oscillator_body(double body_env) {
        const double pitch_env = pitch_env_.process();
        const double f = tune_hz_ * std::exp2(bend_octaves_ * pitch_env);

        double modulation = 0.0;
        if (fm_amount_ > 0.0) {
            mod_phase_ += f * fm_ratio_ / sample_rate();
            if (mod_phase_ >= 1.0) mod_phase_ -= std::floor(mod_phase_);
            modulation = fm_amount_ * std::sin(2.0 * 3.14159265358979323846 * mod_phase_);
        }

        phase_ += f / sample_rate();
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);

        const double angle = 2.0 * 3.14159265358979323846 * phase_ + modulation;
        const double shape = triangle_ ? triangle_from_phase(angle) : std::sin(angle);
        return shape * body_env;
    }

    double render_resonant_body() {
        double excitation = 0.0;
        if (excite_remaining_ > 0) {
            excitation = static_cast<double>(noise_.white());
            --excite_remaining_;
        }
        return static_cast<double>(resonator_.process(static_cast<float>(excitation)));
    }

    double render_circuit_body() {
        // Rectangular trigger pulse into an RC shaper, then a diode that passes
        // the positive edge and softly clamps the negative one. The shaper and
        // the diode together are what turn a square trigger into something the
        // network rings on rather than buzzes on.
        double pulse = 0.0;
        if (pulse_remaining_ > 0) {
            pulse = 1.0;
            --pulse_remaining_;
        }
        pulse_shaper_ += shaper_a_ * (pulse - pulse_shaper_);
        const double diode = pulse_shaper_ >= 0.0
                                 ? pulse_shaper_
                                 : kDiodeVt * (std::exp(pulse_shaper_ / kDiodeVt) - 1.0);

        if (shunt_remaining_ > 0) {
            --shunt_remaining_;
            if (shunt_remaining_ == 0) circuit_.set_attack_shunt(false);
        }

        const auto ring = circuit_.process(diode * kPulseVolts, feedback_z_, 0.0);

        // One sample of delay breaks the delay-free loop. The sign is negative
        // because the network already subtracts its feedback injection, so a
        // positively-signed buffer would damp the ring instead of sustaining
        // it. The saturator's knee is a large fraction of the supply rail, so
        // the buffer is close to linear for an ordinary hit and only softens on
        // the loudest ones; a knee near unity would turn the loop into a
        // bang-bang switch and the drum into a square wave.
        feedback_z_ = -circuit_feedback_ * kMaxLoopGain * kFeedbackKnee *
                      std::tanh(kSwellGain * ring.vbt / kFeedbackKnee);

        tone_lp_ += tone_a_ * (ring.vbt - tone_lp_);
        const double out = tone_lp_ * kCircuitOutputScale;

        // The loop can hold a ring at almost constant amplitude, so "has the
        // voice finished" cannot be read from a single sample -- it crosses
        // zero twice a cycle. A peak follower answers it honestly.
        ring_level_ = std::max(std::fabs(out), ring_level_ * kRingFollowerDecay);
        return out;
    }

    static double triangle_from_phase(double angle) {
        const double normalised =
            angle / (2.0 * 3.14159265358979323846) - std::floor(angle / (2.0 * 3.14159265358979323846));
        return 2.0 * std::fabs(2.0 * normalised - 1.0) - 1.0;
    }

    void update_click_filter() {
        const double fc = std::min(click_tone_hz_ * brightness_, 0.45 * sample_rate());
        click_a_ = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * fc / sample_rate());
    }

    void update_tone_filter() {
        shaper_a_ = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * kShaperHz / sample_rate());
        tone_a_ = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * kToneHz / sample_rate());
    }

    // Circuit-body constants. The pulse amplitude and output scale convert
    // between the network's volts and the +/-1 the rest of the voice works in;
    // the shaper and tone corners are below the band the drum occupies, so
    // they shape the trigger and the output without colouring the ring.
    // The drive level is not a taste control. Below about a volt into the
    // shaper the ring never pushes Vcomm past the -0.556 V knee where the
    // transistor starts to leak, so the pitch sigh -- the drum's signature --
    // silently does not happen; above about two volts the op-amp output sits on
    // its 15 V rail and the ring becomes a square wave. This value keeps the
    // network inside the range the model was fitted over, and the output scale
    // brings its volts back to the range the rest of the voice works in.
    static constexpr double kPulseVolts = 1.0;
    static constexpr double kCircuitOutputScale = 0.1;
    static constexpr double kShaperHz = 120.0;
    static constexpr double kToneHz = 4000.0;
    static constexpr double kDiodeVt = 0.03;
    static constexpr double kSwellGain = 1.1;
    static constexpr double kFeedbackKnee = 8.0;
    // Measured ceiling: above this the loop replaces more than the network
    // dissipates and the ring never decays.
    static constexpr double kMaxLoopGain = 0.86;
    // ~40 ms follower, long enough to bridge a cycle of the lowest tuning.
    static constexpr double kRingFollowerDecay = 0.9995;
    static constexpr double kRingSilenceLevel = 1e-5;

    KickBody body_mode_ = KickBody::oscillator;

    double tune_hz_ = 55.0;
    double body_decay_ms_ = 400.0;
    double pitch_sweep_oct_ = 2.0;
    double pitch_sweep_ms_ = 30.0;
    double click_level_ = 0.3;
    double click_tone_hz_ = 4000.0;
    double click_decay_ms_ = 2.0;
    double noise_level_ = 0.0;
    double noise_decay_ms_ = 60.0;
    double sub_level_ = 0.0;
    bool triangle_ = false;
    double fm_amount_ = 0.0;
    double fm_ratio_ = 1.0;

    double circuit_feedback_ = 0.85;
    double circuit_attack_ms_ = 4.0;
    double circuit_pulse_ms_ = 2.0;

    NoiseSource noise_;
    DecayEnvelope64 body_env_;
    DecayEnvelope64 pitch_env_;
    DecayEnvelope64 click_env_;
    DecayEnvelope64 noise_env_;
    TwoPoleResonator resonator_;
    BridgedTResonator circuit_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double bend_octaves_ = 2.0;
    double brightness_ = 1.0;

    double phase_ = 0.0;
    double mod_phase_ = 0.0;
    double sub_phase_ = 0.0;
    double click_lp_ = 0.0;
    double click_a_ = 0.0;
    double tone_lp_ = 0.0;
    double tone_a_ = 0.0;
    double shaper_a_ = 0.0;
    double pulse_shaper_ = 0.0;
    double feedback_z_ = 0.0;
    double ring_level_ = 0.0;
    int pulse_remaining_ = 0;
    int shunt_remaining_ = 0;
    int excite_remaining_ = 0;
};

}  // namespace pulp::signal::drum
