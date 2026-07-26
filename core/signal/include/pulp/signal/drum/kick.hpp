#pragma once

#include <pulp/signal/bridged_t_resonator.hpp>
#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/hit_life.hpp>
#include <pulp/signal/drum/layers.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/tpt_filter.hpp>
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
        // Deliberately small. An 808-family kick barely moves in pitch with how
        // hard it is hit -- its velocity story is level and click tone, and a
        // bend deep enough to hear reads as portamento rather than as tension.
        r.bend_octaves = 0.15f;
        r.brightness_octaves = 1.5f;
        set_velocity_response(r);

        // A kick without a beater click reads as a sine with an envelope, so
        // the click is on by default; the noise and sub layers are additions a
        // caller opts into.
        click_.set_level(0.3);
        click_.set_cutoff_hz(4000.0);
        click_.set_decay_ms(2.0);
        noise_layer_.set_decay_ms(60.0);
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
    ///
    /// Octaves rather than a linear multiplier, so the sweep composes
    /// additively with the velocity bend, which is also in octaves. A depth
    /// quoted in the multiplier form converts as `octaves = log2(1 + amount)`.
    void set_pitch_sweep_octaves(double octaves) {
        pitch_sweep_oct_ = std::clamp(octaves, 0.0, 6.0);
    }
    void set_pitch_sweep_ms(double ms) { pitch_sweep_ms_ = std::clamp(ms, 0.5, 500.0); }

    /// Level of the attack click, and the corner of the one-pole that tones
    /// it. A click is a noise burst through a lowpass: lowering the corner
    /// turns a beater tick into a thump.
    void set_click_level(double level) { click_.set_level(level); }
    void set_click_tone_hz(double hz) { click_.set_cutoff_hz(hz); }
    void set_click_decay_ms(double ms) { click_.set_decay_ms(ms); }

    /// Level and decay of the noise layer.
    void set_noise_level(double level) { noise_layer_.set_level(level); }
    void set_noise_decay_ms(double ms) { noise_layer_.set_decay_ms(ms); }
    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    /// Level of the sub layer, a sine an octave below the body gated by the
    /// body's own envelope.
    void set_sub_level(double level) { sub_.set_level(level); }

    /// The saturation and degradation stage the voice ends with.
    OutputStage& output() { return output_; }
    const OutputStage& output() const { return output_; }
    int latency_samples() const noexcept override {
        return output_.latency_samples();
    }
    OutputOversampling output_oversampling() const noexcept override {
        return output_.oversampling();
    }
    void set_output_oversampling(OutputOversampling factor) override {
        output_.set_oversampling(factor);
    }

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

    /// How hard the trigger drives the network, 0 to 1.
    ///
    /// Unlike most drive controls this one is not a taste control layered on
    /// top of a working circuit -- it moves the network through the only range
    /// in which it behaves at all, so the control is mapped across that window
    /// rather than across an arbitrary one.
    ///
    /// At 0 the ring only just pushes the transistor past its conduction knee,
    /// so the pitch drop is at its weakest. Below that window the drop stops
    /// happening entirely, which is why 0 is the floor. At 1 the op-amp is
    /// approaching its supply rail, so the ring begins to flatten into a
    /// square. Both ends are audible character rather than faults, and they are
    /// reachable on purpose: clamping the range to the middle would remove the
    /// two sounds the circuit is most recognisable for.
    void set_circuit_drive(double amount) {
        circuit_drive_ = std::clamp(amount, 0.0, 1.0);
    }

    /// Disconnect the leakage path that produces the pitch sigh. Proves the
    /// sigh is emergent rather than scripted, and is a usable sound in itself.
    void set_circuit_sigh(bool enabled) { circuit_.set_sigh_enabled(enabled); }

    /// Select whether circuit retriggers restart, vary their excitation, or
    /// preserve the network memory. The authentic default is preserved_state.
    void set_circuit_hit_life(HitLifeMode mode) {
        circuit_life_.set_mode(mode);
    }
    HitLifeMode circuit_hit_life() const { return circuit_life_.mode(); }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        body_env_.set_sample_rate(sample_rate);
        pitch_env_.set_sample_rate(sample_rate);
        click_.prepare(sample_rate);
        noise_layer_.prepare(sample_rate);
        sub_.prepare(sample_rate);
        resonator_.set_sample_rate(sample_rate);
        circuit_.prepare(sample_rate);
        output_.prepare(sample_rate);
        shaper_.prepare(static_cast<float>(sample_rate));
        shaper_.set_cutoff(static_cast<float>(kShaperHz));
        tone_.prepare(static_cast<float>(sample_rate));
        tone_.set_cutoff(static_cast<float>(kToneHz));
    }

    void on_reset() override {
        body_env_.reset();
        pitch_env_.reset();
        click_.reset();
        noise_layer_.reset();
        sub_.reset();
        resonator_.reset();
        circuit_.reset();
        circuit_.set_attack_shunt(false);
        output_.reset();
        noise_.reset();
        circuit_life_.reset();
        shaper_.reset();
        tone_.reset();
        phase_ = 0.0;
        mod_phase_ = 0.0;
        feedback_z_ = 0.0;
        ring_level_ = 0.0;
        pulse_remaining_ = 0;
        shunt_remaining_ = 0;
        excite_remaining_ = 0;
    }

    void on_note_on(float velocity) override {
        HitLifeDecision circuit_life;
        if (body_mode_ == KickBody::circuit)
            circuit_life =
                circuit_life_.trigger(NoiseSource::default_seed);

        if (body_mode_ == KickBody::oscillator ||
            (body_mode_ == KickBody::circuit &&
             circuit_life.reset_dsp_state))
            output_.reset_nonlinear_state();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        bend_octaves_ = pitch_sweep_oct_ + response.bend(velocity);
        brightness_ = response.brightness_scale(velocity);

        if (body_mode_ != KickBody::circuit) {
            // Non-circuit bodies intentionally restart their procedural
            // excitation on every hit.
            noise_.reset();
        }

        pitch_env_.set_decay_time_constant_ms(pitch_sweep_ms_);
        pitch_env_.set_attack_ms(0.0);
        pitch_env_.trigger();

        body_env_.set_attack_ms(0.4);
        body_env_.set_decay_t60_ms(body_decay_ms_);
        body_env_.trigger();

        // Velocity brightens the click by moving its cutoff, not by scaling it.
        click_.trigger(brightness_);
        noise_layer_.trigger();
        sub_.trigger();

        switch (body_mode_) {
            case KickBody::oscillator:
                phase_ = 0.0;
                mod_phase_ = 0.0;
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
                // The authentic default does not reset the network: a trigger
                // adds energy to a body that may still be ringing. Fixed and
                // advancing modes explicitly restart it.
                if (circuit_life.reset_dsp_state) {
                    reset_circuit_memory();
                    if (circuit_life.reseed_excitation) {
                        noise_.set_seed(circuit_life.seed);
                        noise_.reset();
                    }
                }
                circuit_excitation_gain_ =
                    1.0 + 0.03 * static_cast<double>(noise_.white());
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
            return body_env_.is_active() || layers_active() ||
                   pulse_remaining_ > 0 || ring_level_ > kRingSilenceLevel ||
                   output_.has_tail();
        }
        if (body_mode_ == KickBody::resonant) {
            return resonator_.is_ringing() || layers_active() || excite_remaining_ > 0 ||
                   output_.has_tail();
        }
        return body_env_.is_active() || layers_active() || output_.has_tail();
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

            const double click = click_.process(static_cast<double>(noise_.white()));
            const double noise = noise_layer_.process(static_cast<double>(noise_.process()));
            const double sub = sub_.process(tune_hz_, body_env);

            const double mixed = body + click + noise + sub;
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(mixed)) * velocity_gain_);
        }

        circuit_.snap_denormals();
    }

private:
    void reset_circuit_memory() {
        circuit_.reset();
        circuit_.set_attack_shunt(false);
        shaper_.reset();
        tone_.reset();
        feedback_z_ = 0.0;
        ring_level_ = 0.0;
        pulse_remaining_ = 0;
        shunt_remaining_ = 0;
    }

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
        const double shaped = shaper_.process_lowpass(static_cast<float>(pulse));
        const double diode = shaped >= 0.0
                                 ? shaped
                                 : kDiodeVt * (std::exp(shaped / kDiodeVt) - 1.0);

        if (shunt_remaining_ > 0) {
            --shunt_remaining_;
            if (shunt_remaining_ == 0) circuit_.set_attack_shunt(false);
        }

        const double drive_volts =
            kPulseVoltsMin + circuit_drive_ * (kPulseVoltsMax - kPulseVoltsMin);
        const auto ring = circuit_.process(
            diode * drive_volts * circuit_excitation_gain_, feedback_z_, 0.0);

        // One sample of delay breaks the delay-free loop. The sign is negative
        // because the network already subtracts its feedback injection, so a
        // positively-signed buffer would damp the ring instead of sustaining
        // it. The saturator's knee is a large fraction of the supply rail, so
        // the buffer is close to linear for an ordinary hit and only softens on
        // the loudest ones; a knee near unity would turn the loop into a
        // bang-bang switch and the drum into a square wave.
        feedback_z_ = -circuit_feedback_ * kMaxLoopGain * kFeedbackKnee *
                      std::tanh(kSwellGain * ring.vbt / kFeedbackKnee);

        const double out =
            tone_.process_lowpass(static_cast<float>(ring.vbt)) * kCircuitOutputScale;

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

    bool layers_active() const { return click_.is_active() || noise_layer_.is_active(); }

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
    // Measured bounds of the window the network behaves in. Below the low end
    // the ring never reaches the -0.556 V conduction knee and the pitch drop
    // silently does not happen; above the high end the op-amp sits on its 15 V
    // rail. `set_circuit_drive` maps its 0..1 across exactly this span.
    static constexpr double kPulseVoltsMin = 1.0;
    static constexpr double kPulseVoltsMax = 1.7;
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
    bool triangle_ = false;
    double fm_amount_ = 0.0;
    double fm_ratio_ = 1.0;

    double circuit_feedback_ = 0.85;
    double circuit_drive_ = 0.3;
    double circuit_attack_ms_ = 4.0;
    double circuit_pulse_ms_ = 2.0;

    NoiseSource noise_;
    DecayEnvelope64 body_env_;
    DecayEnvelope64 pitch_env_;
    ClickLayer click_;
    NoiseLayer noise_layer_;
    SubLayer sub_;
    TwoPoleResonator resonator_;
    BridgedTResonator circuit_;
    HitLife circuit_life_{HitLifeMode::preserved_state};
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double bend_octaves_ = 2.0;
    double brightness_ = 1.0;

    TptFilter shaper_;
    TptFilter tone_;

    double phase_ = 0.0;
    double mod_phase_ = 0.0;
    double feedback_z_ = 0.0;
    double ring_level_ = 0.0;
    double circuit_excitation_gain_ = 1.0;
    int pulse_remaining_ = 0;
    int shunt_remaining_ = 0;
    int excite_remaining_ = 0;
};

}  // namespace pulp::signal::drum
