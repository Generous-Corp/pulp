#pragma once

#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/lowpass_gate.hpp>
#include <pulp/signal/phase_distortion.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// A zap: a phase-distortion percussion voice.
///
/// The sound this voice is named for is the one phase distortion is uniquely
/// good at. Two envelopes run at once and they are doing different things: the
/// pitch drops, as in any electronic drum, *and* the distortion amount drops
/// independently, which sweeps the apparent resonant peak down through the
/// spectrum. Because the peak is produced by reading a cosine at a swept
/// multiple of the phase rather than by a filter, it can be swept faster and
/// further than any filter would stay stable through — which is exactly why
/// the resulting sound has no equivalent in a subtractive voice.
///
/// Decoupling the two is the point. Tying the distortion sweep to the pitch
/// sweep gives an ordinary filtered drop; letting the distortion collapse
/// while the pitch is still falling is what produces the zap.
///
/// Two oscillators, the second detuned, because a single phase-distortion
/// oscillator is a very pure sound and the beating between two is most of what
/// gives the voice weight.
///
/// Lineage: Casio's phase-distortion patent, US 4,658,691 (expired),
/// implemented from the published description. See
/// `pulp::signal::PhaseDistortionOscT`.
///
/// RT contract: `prepare()` allocates nothing; every other method, including
/// `note_on` and `process`, allocates nothing and takes no locks.
class ZapVoice : public Voice {
public:
    ZapVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        r.bend_octaves = 1.0f;
        // Velocity also opens the distortion, which reads as a brighter,
        // sharper zap rather than a louder one.
        r.brightness_octaves = 0.8f;
        set_velocity_response(r);
    }

    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 20.0, 4000.0); }

    /// Depth and time constant of the pitch drop, in octaves.
    void set_pitch_sweep_octaves(double octaves) {
        pitch_sweep_oct_ = std::clamp(octaves, 0.0, 6.0);
    }
    void set_pitch_sweep_ms(double ms) { pitch_sweep_ms_ = std::clamp(ms, 0.5, 500.0); }

    /// How long the note lasts, as a T60.
    void set_decay_ms(double ms) { decay_ms_ = std::clamp(ms, 10.0, 4000.0); }

    void set_shape(PhaseDistortionShape shape) { shape_ = shape; }

    /// Distortion depth at the start of the hit, and how quickly it collapses.
    /// This envelope is deliberately independent of the pitch sweep.
    void set_distortion(double amount) { distortion_ = std::clamp(amount, 0.0, 1.0); }
    void set_distortion_ms(double ms) { distortion_ms_ = std::clamp(ms, 0.5, 500.0); }

    /// Highest carrier multiple the resonant shapes reach.
    void set_resonant_depth(double multiples) {
        resonant_depth_ = std::clamp(multiples, 1.0, 32.0);
    }

    /// Detune of the second oscillator, in cents.
    void set_detune_cents(double cents) { detune_cents_ = std::clamp(cents, 0.0, 100.0); }

    /// Depth of a ring modulation applied to the pair, and its ratio to the
    /// tuning. Ring modulation of an already-swept pair is what takes the voice
    /// from a zap toward a clang.
    void set_ring(double amount) { ring_ = std::clamp(amount, 0.0, 1.0); }
    void set_ring_ratio(double ratio) { ring_ratio_ = std::clamp(ratio, 0.25, 16.0); }

    LowpassGate& gate() { return gate_; }
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

protected:
    void on_prepare(double sample_rate) override {
        osc_a_.set_sample_rate(sample_rate);
        osc_b_.set_sample_rate(sample_rate);
        amp_env_.set_sample_rate(sample_rate);
        pitch_env_.set_sample_rate(sample_rate);
        dcw_env_.set_sample_rate(sample_rate);
        gate_.set_sample_rate(sample_rate);
        dc_.reset();
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        osc_a_.reset();
        osc_b_.reset();
        amp_env_.reset();
        pitch_env_.reset();
        dcw_env_.reset();
        gate_.reset();
        dc_.reset();
        output_.reset();
        ring_phase_ = 0.0;
    }

    void on_note_on(float velocity) override {
        output_.reset_nonlinear_state();
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        applied_bend_ = pitch_sweep_oct_ + response.bend(velocity);
        // Velocity opens the distortion as well as the pitch, so a harder hit
        // is a sharper zap rather than the same zap turned up.
        applied_distortion_ =
            std::clamp(distortion_ * static_cast<double>(response.brightness_scale(velocity)),
                       0.0, 1.0);

        osc_a_.reset();
        osc_b_.reset();
        osc_a_.set_shape(shape_);
        osc_b_.set_shape(shape_);
        osc_a_.set_resonant_depth(resonant_depth_);
        osc_b_.set_resonant_depth(resonant_depth_);
        ring_phase_ = 0.0;
        dc_.reset();

        pitch_env_.set_attack_ms(0.0);
        pitch_env_.set_decay_time_constant_ms(pitch_sweep_ms_);
        pitch_env_.trigger();

        // The distortion envelope is its own decay, not a copy of the pitch
        // one. Sharing them would make this an ordinary filtered drop.
        dcw_env_.set_attack_ms(0.0);
        dcw_env_.set_decay_time_constant_ms(distortion_ms_);
        dcw_env_.trigger();

        amp_env_.set_attack_ms(0.3);
        amp_env_.set_decay_t60_ms(decay_ms_);
        amp_env_.trigger();
    }

    bool on_is_active() const override {
        return amp_env_.is_active() || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        const double detune = std::pow(2.0, detune_cents_ / 1200.0);

        for (int i = 0; i < num_samples; ++i) {
            const double pitch = pitch_env_.process();
            const double dcw = dcw_env_.process();
            const double amplitude = amp_env_.process();

            const double f = tune_hz_ * std::exp2(applied_bend_ * pitch);
            osc_a_.set_frequency(f);
            osc_b_.set_frequency(f * detune);

            const double warp = applied_distortion_ * dcw;
            osc_a_.set_amount(warp);
            osc_b_.set_amount(warp);

            double mixed = 0.5 * (static_cast<double>(osc_a_.process()) +
                                  static_cast<double>(osc_b_.process()));

            if (ring_ > 0.0) {
                ring_phase_ += f * ring_ratio_ / sample_rate();
                if (ring_phase_ >= 1.0) ring_phase_ -= std::floor(ring_phase_);
                mixed += ring_ * mixed *
                         std::cos(2.0 * 3.14159265358979323846 * ring_phase_);
            }

            // Warping a cosine's phase moves its mean off zero, so the voice
            // needs a DC blocker that an ordinary oscillator would not.
            const double centred = dc_.process(static_cast<float>(mixed));
            const double gated = gate_.process(static_cast<float>(centred * amplitude),
                                               amplitude);
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(gated)) * velocity_gain_);
        }
    }

private:
    double tune_hz_ = 220.0;
    double pitch_sweep_oct_ = 2.5;
    double pitch_sweep_ms_ = 35.0;
    double decay_ms_ = 350.0;
    PhaseDistortionShape shape_ = PhaseDistortionShape::resonant_saw;
    double distortion_ = 0.8;
    double distortion_ms_ = 60.0;
    double resonant_depth_ = 12.0;
    double detune_cents_ = 9.0;
    double ring_ = 0.0;
    double ring_ratio_ = 1.5;

    PhaseDistortionOsc osc_a_;
    PhaseDistortionOsc osc_b_;
    DecayEnvelope64 amp_env_;
    DecayEnvelope64 pitch_env_;
    DecayEnvelope64 dcw_env_;
    LowpassGate gate_;
    DcBlocker<float> dc_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double applied_bend_ = 2.5;
    double applied_distortion_ = 0.8;
    double ring_phase_ = 0.0;
};

}  // namespace pulp::signal::drum
