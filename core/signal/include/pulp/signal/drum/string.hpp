#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/layers.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/karplus_strong.hpp>
#include <pulp/signal/lowpass_gate.hpp>
#include <pulp/signal/noise_source.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

enum class StringModulation {
    none,
    fm,
    ring,
    sync,
};

/// A plucked or struck string, as a percussion voice.
///
/// The body is a delay line fed back through its own losses, so the harmonics
/// are not written down anywhere -- they are the frequencies whose period
/// divides the loop, and everything else cancels going round. That is what
/// separates this from an oscillator bank: the excitation is part of the
/// sound, so the same string struck with a stick and plucked with a fingertip
/// are genuinely different, and a second hit during the tail adds to what is
/// still travelling rather than replacing it.
///
/// It sits in the percussion set because a struck string *is* percussion --
/// a hammered dulcimer, a kalimba, a piano. What makes it a voice rather than
/// a raw `KarplusStrongT` is the lifecycle every other drum here has: an
/// excitation whose length and colour are controls, velocity that reaches
/// timbre rather than only level, and the shared output stage.
///
/// Velocity drives the string's dynamic-level filter, which is the physical
/// claim: playing harder puts more high-frequency energy into the string, so a
/// hard note is brighter as well as louder. That is the same coupling the tom
/// makes with its bend and the FM voices make with their index.
///
/// RT contract: `prepare()` allocates the delay lines. Everything else,
/// including `note_on` and `process`, allocates nothing and takes no locks.
class StringVoice : public Voice {
public:
    StringVoice() {
        VelocityResponse r;
        r.level_db = 16.0f;
        // A struck string tightens very little in pitch, so the bend stays
        // small; the brightness term is where its velocity lives.
        r.bend_octaves = 0.05f;
        r.brightness_octaves = 2.0f;
        set_velocity_response(r);
    }

    /// Pitch of the string, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 30.0, 4000.0); }

    /// Time for the note to fall by 60 dB, in seconds. Derived per round trip,
    /// so a high note decays faster at the same setting -- as a real string does.
    void set_decay_seconds(double seconds) { decay_s_ = std::clamp(seconds, 0.05, 20.0); }

    /// How much faster the upper partials die than the fundamental.
    void set_damping(double amount) { damping_ = std::clamp(amount, 0.0, 1.0); }

    /// Inharmonicity from string stiffness, 0 (a true harmonic series) to 1.
    /// This is what separates a struck bar or a piano from a guitar.
    void set_stiffness(double amount) { stiffness_ = std::clamp(amount, 0.0, 1.0); }

    /// Where along the string it is excited, 0 (at the end) to 0.5 (the middle).
    /// A comb, not a level: it removes the partials with a node at that point.
    void set_pluck_position(double position) {
        pluck_position_ = std::clamp(position, 0.0, 0.5);
    }

    /// Length of the excitation burst in milliseconds. Short is a hard mallet;
    /// longer is a fingertip.
    void set_exciter_ms(double ms) { exciter_ms_ = std::clamp(ms, 0.1, 50.0); }

    /// Bandwidth of the excitation in Hz at zero velocity, before the velocity
    /// response opens it.
    ///
    /// Deliberately well below the audio band. This is the floor the velocity
    /// coupling works up from, so setting it wide leaves velocity nothing to
    /// do -- the excitation is already passing everything and a hard hit sounds
    /// like a soft one turned up. A dark floor is what makes the coupling
    /// audible.
    void set_brightness_hz(double hz) { brightness_hz_ = std::clamp(hz, 200.0, 18000.0); }

    /// Darkens the attack without retuning the string.
    void set_pick_direction(double amount) { pick_direction_ = std::clamp(amount, 0.0, 0.99); }

    /// Whether a new hit restarts the string or adds to what is still ringing.
    /// Adding is the physical behaviour and is why a fast repeated figure does
    /// not sound like one sample fired twice.
    void set_restart_on_hit(bool restart) { restart_on_hit_ = restart; }

    /// Optional post-body modulation. `mix` crossfades from the unmodified
    /// string to the selected mode. FM bends the delay length, Ring multiplies
    /// by a sine, and Sync substitutes a slave saw reset by the string pitch.
    void set_modulation(StringModulation mode) {
        if (modulation_ == StringModulation::fm &&
            mode != StringModulation::fm && is_active()) {
            string_.set_frequency(base_frequency_);
        }
        modulation_ = mode;
    }
    void set_modulation_mix(double mix) { modulation_mix_ = std::clamp(mix, 0.0, 1.0); }
    void set_modulation_ratio(double ratio) {
        modulation_ratio_ = std::clamp(ratio, 0.125, 16.0);
    }
    void set_fm_depth_octaves(double octaves) {
        fm_depth_octaves_ = std::clamp(octaves, 0.0, 2.0);
    }

    /// Amount of the lowpass-gate stage after modulation. Zero is a transparent
    /// bypass; one applies the full coupled level/brightness decay.
    void set_lpg_amount(double amount) { lpg_amount_ = std::clamp(amount, 0.0, 1.0); }
    LowpassGate& gate() { return gate_; }

    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    OutputStage& output() { return output_; }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        string_.prepare(sample_rate, 30.0);
        exciter_env_.set_sample_rate(sample_rate);
        gate_env_.set_sample_rate(sample_rate);
        gate_.set_sample_rate(sample_rate);
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        string_.reset();
        exciter_env_.reset();
        gate_env_.reset();
        gate_.reset();
        output_.reset();
        noise_.reset();
        modulation_phase_ = 0.0;
        master_phase_ = 0.0;
        sync_phase_ = 0.0;
    }

    void on_note_on(float velocity) override {
        if (restart_on_hit_) output_.reset();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);

        noise_.reset();
        base_frequency_ = tune_hz_ * std::exp2(response.bend(velocity));
        string_.set_frequency(base_frequency_);
        string_.set_decay_seconds(decay_s_);
        string_.set_damping(damping_);
        string_.set_stiffness(stiffness_);
        string_.set_pluck_position(pluck_position_);
        string_.set_pick_direction(pick_direction_);
        // Velocity opens the excitation's bandwidth: harder is brighter.
        string_.set_dynamic_bandwidth_hz(
            brightness_hz_ * static_cast<double>(response.brightness_scale(velocity)));
        string_.pluck(restart_on_hit_);

        exciter_env_.set_attack_ms(0.0);
        exciter_env_.set_decay_time_constant_ms(exciter_ms_);
        exciter_env_.trigger();

        gate_env_.reset();
        gate_env_.set_attack_ms(0.5);
        gate_env_.set_decay_t60_ms(decay_s_ * 1000.0);
        gate_env_.trigger();
        modulation_phase_ = 0.0;
        master_phase_ = 0.0;
        sync_phase_ = 0.0;
    }

    bool on_is_active() const override {
        return string_.is_active() || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        for (int i = 0; i < num_samples; ++i) {
            modulation_phase_ +=
                base_frequency_ * modulation_ratio_ / sample_rate();
            if (modulation_phase_ >= 1.0) {
                modulation_phase_ -= std::floor(modulation_phase_);
            }
            const double modulator =
                std::sin(2.0 * 3.14159265358979323846 * modulation_phase_);
            if (modulation_ == StringModulation::fm) {
                string_.set_frequency(base_frequency_ *
                                      std::exp2(fm_depth_octaves_ *
                                                modulation_mix_ * modulator));
            }

            const double burst = exciter_env_.is_active()
                                     ? static_cast<double>(noise_.white()) *
                                           exciter_env_.process()
                                     : 0.0;
            const double dry = static_cast<double>(
                string_.process(static_cast<float>(burst)));

            double effected = dry;
            if (modulation_ == StringModulation::ring) {
                effected = dry * modulator;
            } else if (modulation_ == StringModulation::sync) {
                master_phase_ += base_frequency_ / sample_rate();
                const bool wrapped = master_phase_ >= 1.0;
                if (wrapped) {
                    master_phase_ -= std::floor(master_phase_);
                    sync_phase_ = 0.0;
                } else {
                    sync_phase_ +=
                        base_frequency_ * modulation_ratio_ / sample_rate();
                    if (sync_phase_ >= 1.0) {
                        sync_phase_ -= std::floor(sync_phase_);
                    }
                }
                effected = 2.0 * sync_phase_ - 1.0;
            }
            const double modulated =
                dry + modulation_mix_ * (effected - dry);
            const double gate_target =
                gate_env_.is_active() ? gate_env_.process() : 0.0;
            const double gated =
                static_cast<double>(gate_.process(static_cast<float>(modulated),
                                                  gate_target));
            const double lpg =
                modulated + lpg_amount_ * (gated - modulated);
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(lpg)) * velocity_gain_);
        }
    }

private:
    double tune_hz_ = 220.0;
    double decay_s_ = 2.0;
    double damping_ = 0.3;
    double stiffness_ = 0.0;
    double pluck_position_ = 0.25;
    double exciter_ms_ = 1.0;
    double brightness_hz_ = 1800.0;
    double pick_direction_ = 0.0;
    bool restart_on_hit_ = false;
    StringModulation modulation_ = StringModulation::none;
    double modulation_mix_ = 0.0;
    double modulation_ratio_ = 2.0;
    double fm_depth_octaves_ = 0.25;
    double lpg_amount_ = 1.0;

    NoiseSource noise_;
    KarplusStrong string_;
    DecayEnvelope64 exciter_env_;
    DecayEnvelope64 gate_env_;
    LowpassGate gate_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double base_frequency_ = 220.0;
    double modulation_phase_ = 0.0;
    double master_phase_ = 0.0;
    double sync_phase_ = 0.0;
};

}  // namespace pulp::signal::drum
