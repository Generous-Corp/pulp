#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/layers.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// A snare drum.
///
/// A snare is two instruments in a shared shell and it has to be built that
/// way. The drum itself is a small tuned membrane, which the tone pair below
/// stands in for; the snares are a set of wires under the bottom head that
/// rattle against it, which is the filtered noise. Neither alone reads as a
/// snare -- the tone on its own is a tom, and the noise on its own is a
/// splash -- and the balance between them is the single most recognisable
/// control on the instrument.
///
/// Three details separate this from a tom with noise added:
///
/// * **Two tone oscillators, not one.** A snare's shell is small enough that
///   its two lowest modes are close together and beat audibly. One oscillator
///   cannot produce that beating, and the ratio between the two is what makes
///   a snare sound taut rather than hollow.
/// * **The wires buzz.** They are not simply excited by the strike; they leave
///   the head and return, which amplitude-modulates the noise at a rate set by
///   how tight they are. `set_rattle` is that modulation, and without it the
///   noise layer reads as a hiss laid over a drum.
/// * **The shell resonates the noise.** The high-Q bandpass takes the strike
///   and the wires and puts the shell's own pitch back into them, which is why
///   a snare's noise is pitched even though noise has no pitch.
///
/// RT contract: `prepare()` allocates nothing; every other method, including
/// `note_on` and `process`, allocates nothing and takes no locks.
class SnareVoice : public Voice {
public:
    SnareVoice() {
        VelocityResponse r;
        r.level_db = 16.0f;
        // Tension, not portamento: a struck head tightens slightly under a
        // harder strike, and that is all this is meant to convey.
        r.bend_octaves = 0.08f;
        r.brightness_octaves = 1.2f;
        // A harder snare hit puts proportionally more into the wires than into
        // the shell -- which is why a rimshot is nearly all snap.
        r.noise_balance = 0.25f;
        set_velocity_response(r);

        snap_.set_level(0.4);
        snap_.set_cutoff_hz(6000.0);
        snap_.set_decay_ms(4.0);
    }

    // -- Tone (the drum) -----------------------------------------------------

    /// Fundamental of the lower tone oscillator, in Hz.
    void set_tune_hz(double hz) { tune_hz_ = std::clamp(hz, 60.0, 800.0); }

    /// Frequency ratio of the upper tone oscillator to the lower. Values
    /// between about 1.5 and 1.8 are where a shell's two lowest modes actually
    /// sit; further apart the pair stops beating and reads as two drums.
    void set_tone_ratio(double ratio) { tone_ratio_ = std::clamp(ratio, 1.0, 4.0); }

    void set_tone_level(double level) { tone_level_ = std::max(level, 0.0); }
    void set_tone_decay_ms(double ms) { tone_decay_ms_ = std::clamp(ms, 5.0, 2000.0); }

    /// Depth and time constant of the shared downward pitch sweep, in octaves.
    void set_pitch_sweep_octaves(double octaves) {
        pitch_sweep_oct_ = std::clamp(octaves, 0.0, 4.0);
    }
    void set_pitch_sweep_ms(double ms) { pitch_sweep_ms_ = std::clamp(ms, 0.5, 300.0); }

    /// Phase-modulation depth of the upper oscillator into the lower.
    void set_fm_amount(double amount) { fm_amount_ = std::clamp(amount, 0.0, 8.0); }

    /// Crossfade from the two oscillators summed (0) to ring-modulated (1).
    /// Ring modulation replaces the pair with their sum and difference tones,
    /// which removes the fundamentals and leaves a hollow, metallic shell.
    void set_ring(double amount) { ring_ = std::clamp(amount, 0.0, 1.0); }

    // -- Snares (the wires) --------------------------------------------------

    void set_noise_level(double level) { noise_level_ = std::max(level, 0.0); }
    void set_noise_decay_ms(double ms) { noise_decay_ms_ = std::clamp(ms, 5.0, 3000.0); }
    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    /// Corner and resonance of the filter the wires are heard through.
    void set_noise_cutoff_hz(double hz) { noise_cutoff_hz_ = std::clamp(hz, 100.0, 18000.0); }
    void set_noise_resonance(double q) { noise_q_ = std::clamp(q, 0.5, 12.0); }

    /// How far that corner sweeps over the note, in octaves. Positive opens the
    /// wires up as they decay; negative closes them down.
    void set_noise_sweep_octaves(double octaves) {
        noise_sweep_oct_ = std::clamp(octaves, -4.0, 4.0);
    }

    /// Depth and rate of the wires' buzz against the head.
    void set_rattle(double amount) { rattle_ = std::clamp(amount, 0.0, 1.0); }
    void set_rattle_hz(double hz) { rattle_hz_ = std::clamp(hz, 5.0, 400.0); }

    /// The strike itself: a short highpassed crack, distinct from the wires.
    void set_snap_level(double level) { snap_.set_level(level); }
    void set_snap_cutoff_hz(double hz) { snap_.set_cutoff_hz(hz); }
    void set_snap_decay_ms(double ms) { snap_.set_decay_ms(ms); }

    /// Level of the shell resonance applied to the noise path, and its Q.
    void set_shell_level(double level) { shell_.set_level(level); }
    void set_shell_resonance(double q) { shell_.set_resonance(q); }

    OutputStage& output() { return output_; }
    const OutputStage& output() const { return output_; }
    OutputStage* output_stage() noexcept override { return &output_; }
    int latency_samples() const noexcept override {
        return output_.latency_samples();
    }
    OutputOversampling output_oversampling() const noexcept override {
        return output_.oversampling();
    }
    void set_output_oversampling(OutputOversampling factor) override {
        output_.set_oversampling(factor);
    }

    /// Degradation applied to the pitched shell before it meets the wires.
    /// Keeping this separate from both the wire path and the shared output
    /// stage lets a preset age either half of the instrument independently.
    LofiChain& tone_lofi() { return tone_lofi_; }

    /// A second degradation chain on the noise path alone. Crushing the wires
    /// harder than the shell is a distinct and much-used sound, and one shared
    /// chain cannot produce it.
    LofiChain& noise_lofi() { return noise_lofi_; }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        tone_env_.set_sample_rate(sample_rate);
        pitch_env_.set_sample_rate(sample_rate);
        noise_env_.set_sample_rate(sample_rate);
        snap_.prepare(sample_rate);
        noise_filter_.set_sample_rate(static_cast<float>(sample_rate));
        shell_.prepare(sample_rate);
        snap_hp_.prepare(static_cast<float>(sample_rate));
        tone_lofi_.set_sample_rate(sample_rate);
        noise_lofi_.set_sample_rate(sample_rate);
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        tone_env_.reset();
        pitch_env_.reset();
        noise_env_.reset();
        snap_.reset();
        noise_filter_.reset();
        shell_.reset();
        snap_hp_.reset();
        tone_lofi_.reset();
        noise_lofi_.reset();
        output_.reset();
        noise_.reset();
        phase_low_ = 0.0;
        phase_high_ = 0.0;
        rattle_phase_ = 0.0;
        high_previous_ = 0.0;
    }

    void on_note_on(float velocity) override {
        // The shell keeps ringing across hits; preserve the shared output
        // history with it so a retrigger cannot truncate the pending FIR.
        output_.trigger();
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        bend_octaves_ = pitch_sweep_oct_ + response.bend(velocity);
        brightness_ = response.brightness_scale(velocity);
        // Velocity tilts the mix toward the wires without touching the levels
        // the caller set, so a preset's balance is preserved and only the
        // performance moves it.
        noise_tilt_ = response.noise_shift(velocity);

        noise_.reset();
        phase_low_ = 0.0;
        phase_high_ = 0.0;
        rattle_phase_ = 0.0;
        high_previous_ = 0.0;
        snap_hp_.reset();
        // The wire filter shapes the strike and starts clean; `shell_` is the
        // drum's body and deliberately keeps ringing across hits.
        noise_filter_.reset();

        pitch_env_.set_attack_ms(0.0);
        pitch_env_.set_decay_time_constant_ms(pitch_sweep_ms_);
        pitch_env_.trigger();

        // A layer at zero level is not started, and is not counted as active
        // below. Both halves are needed: its render path returns early, so a
        // started envelope would never advance and the voice would report
        // itself active forever.
        if (tone_level_ > 0.0) {
            tone_env_.set_attack_ms(0.3);
            tone_env_.set_decay_t60_ms(tone_decay_ms_);
            tone_env_.trigger();
        }
        if (noise_level_ > 0.0) {
            noise_env_.set_attack_ms(0.2);
            noise_env_.set_decay_t60_ms(noise_decay_ms_);
            noise_env_.trigger();
        }

        snap_.trigger(brightness_);
        shell_.set_frequency_hz(tune_hz_);
    }

    bool on_is_active() const override {
        return (tone_level_ > 0.0 && tone_env_.is_active()) ||
               (noise_level_ > 0.0 && noise_env_.is_active()) || snap_.is_active() ||
               shell_.is_ringing() || tone_lofi_.has_tail() ||
               noise_lofi_.has_tail() || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        const double tone_gain = tone_level_ * (1.0 - noise_tilt_);
        const double wire_gain = noise_level_ * (1.0 + noise_tilt_);

        for (int i = 0; i < num_samples; ++i) {
            const auto tone = tone_lofi_.process(
                static_cast<float>(render_tone(tone_gain)));
            out[i] += static_cast<float>(
                output_.process(static_cast<float>(
                    static_cast<double>(tone) + render_wires(wire_gain))) *
                velocity_gain_);
        }
    }

private:
    double render_tone(double gain) {
        if (!tone_env_.is_active() || gain <= 0.0) return 0.0;

        const double pitch_env = pitch_env_.process();
        const double sweep = std::exp2(bend_octaves_ * pitch_env);
        const double f_low = tune_hz_ * sweep;
        const double f_high = f_low * tone_ratio_;

        phase_high_ += f_high / sample_rate();
        if (phase_high_ >= 1.0) phase_high_ -= std::floor(phase_high_);
        const double high = std::sin(2.0 * 3.14159265358979323846 * phase_high_);

        phase_low_ += f_low / sample_rate();
        if (phase_low_ >= 1.0) phase_low_ -= std::floor(phase_low_);
        // The modulator is read one sample late so the pair cannot form a
        // delay-free loop when a caller cross-modulates them.
        const double low = std::sin(2.0 * 3.14159265358979323846 * phase_low_ +
                                    fm_amount_ * high_previous_);
        high_previous_ = high;

        const double summed = 0.5 * (low + high);
        const double ringed = low * high;
        return (summed + ring_ * (ringed - summed)) * tone_env_.process() * gain;
    }

    double render_wires(double gain) {
        const double snap = snap_hp_.process_highpass(
            static_cast<float>(snap_.process(static_cast<double>(noise_.white()))));

        double wires = 0.0;
        if (noise_env_.is_active() && gain > 0.0) {
            const double envelope = noise_env_.process();

            // The filter sweeps over the note, so the wires change character as
            // they die rather than simply fading.
            const double corner = noise_cutoff_hz_ * brightness_ *
                                  std::exp2(noise_sweep_oct_ * envelope);
            noise_filter_.set_frequency(
                static_cast<float>(std::clamp(corner, 20.0, 0.49 * sample_rate())));
            noise_filter_.set_resonance(static_cast<float>(noise_q_));

            wires = noise_filter_.process(noise_.process()) * envelope * gain;

            if (rattle_ > 0.0) {
                rattle_phase_ += rattle_hz_ / sample_rate();
                if (rattle_phase_ >= 1.0) rattle_phase_ -= std::floor(rattle_phase_);
                wires *= 1.0 + rattle_ * std::sin(2.0 * 3.14159265358979323846 * rattle_phase_);
            }
        }

        const double path = shell_.process(wires + snap);
        return noise_lofi_.process(static_cast<float>(path));
    }

    double tune_hz_ = 180.0;
    double tone_ratio_ = 1.6;
    double tone_level_ = 0.5;
    double tone_decay_ms_ = 120.0;
    double pitch_sweep_oct_ = 0.5;
    double pitch_sweep_ms_ = 25.0;
    double fm_amount_ = 0.0;
    double ring_ = 0.0;

    double noise_level_ = 0.6;
    double noise_decay_ms_ = 180.0;
    double noise_cutoff_hz_ = 3000.0;
    double noise_q_ = 1.0;
    double noise_sweep_oct_ = 0.0;
    double rattle_ = 0.0;
    double rattle_hz_ = 45.0;

    NoiseSource noise_;
    DecayEnvelope64 tone_env_;
    DecayEnvelope64 pitch_env_;
    DecayEnvelope64 noise_env_;
    ClickLayer snap_;
    Svf noise_filter_;
    ShellLayer shell_;
    TptFilter snap_hp_;
    LofiChain tone_lofi_;
    LofiChain noise_lofi_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double bend_octaves_ = 0.5;
    double brightness_ = 1.0;
    double noise_tilt_ = 0.0;

    double phase_low_ = 0.0;
    double phase_high_ = 0.0;
    double rattle_phase_ = 0.0;
    double high_previous_ = 0.0;
};

}  // namespace pulp::signal::drum
