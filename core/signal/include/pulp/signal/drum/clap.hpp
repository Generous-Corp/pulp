#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// A hand clap.
///
/// A clap is not one sound. Several people clapping never land together, and
/// even one person's clap is the hands meeting, separating slightly, and
/// meeting again. What the ear identifies as "a clap" is that short burst
/// train followed by the room -- and the burst train is why a clap cannot be
/// built as a single noise envelope no matter how it is shaped.
///
/// So the voice schedules a small number of noise bursts a few milliseconds
/// apart and runs a longer, quieter tail underneath them from the first burst
/// onward. The tail is what fuses the train into one event: without it the
/// bursts are heard as separate ticks, and with it they are heard as one clap
/// in a room. Widening the spacing past about 20 ms breaks that fusion, which
/// is a usable effect and the reason the control is exposed rather than fixed.
///
/// The schedule is computed at `note_on` and is deterministic, so a clap
/// renders identically for a given parameter set.
///
/// This voice is mono. The alternating left-right placement some hardware
/// claps use is a spatialisation of the burst train rather than part of its
/// synthesis, and belongs to whatever pans the voice.
///
/// RT contract: `prepare()` allocates nothing; every other method allocates
/// nothing and takes no locks.
class ClapVoice : public Voice {
public:
    /// Upper bound on the burst train. Beyond about eight the train stops
    /// reading as a clap and starts reading as a roll.
    static constexpr int max_bursts = 8;

    ClapVoice() {
        VelocityResponse r;
        r.level_db = 15.0f;
        r.brightness_octaves = 0.7f;
        set_velocity_response(r);
    }

    /// How many bursts the train contains.
    void set_burst_count(int count) { burst_count_ = std::clamp(count, 1, max_bursts); }

    /// Milliseconds between bursts.
    void set_burst_spacing_ms(double ms) { spacing_ms_ = std::clamp(ms, 1.0, 60.0); }

    /// How quickly each burst decays.
    void set_burst_decay_ms(double ms) { burst_decay_ms_ = std::clamp(ms, 0.5, 60.0); }

    /// How much quieter each burst is than the one before it, as a ratio. The
    /// hands lose energy each time they meet, and a train at constant level
    /// reads as a machine rather than a person.
    void set_burst_falloff(double ratio) { falloff_ = std::clamp(ratio, 0.2, 1.5); }

    /// Level and decay of the tail that fuses the train.
    void set_tail_level(double level) { tail_level_ = std::max(level, 0.0); }
    void set_tail_decay_ms(double ms) { tail_decay_ms_ = std::clamp(ms, 10.0, 3000.0); }

    /// Centre and resonance of the bandpass the noise is heard through. A clap
    /// occupies a narrow, quite high band; without the filter the voice is
    /// simply noise bursts.
    void set_cutoff_hz(double hz) { cutoff_hz_ = std::clamp(hz, 200.0, 16000.0); }
    void set_resonance(double q) { resonance_ = std::clamp(q, 0.5, 12.0); }

    void set_noise_color(NoiseColor color) { noise_.set_color(color); }

    /// Level of an optional sine under the train, at `set_body_hz`. Some
    /// hardware claps carry one; it gives the voice a pitch centre and helps it
    /// sit in a mix that is otherwise busy in the clap's band.
    void set_body_level(double level) { body_level_ = std::max(level, 0.0); }
    void set_body_hz(double hz) { body_hz_ = std::clamp(hz, 40.0, 2000.0); }

    OutputStage& output() { return output_; }

protected:
    void on_prepare(double sample_rate) override {
        noise_.prepare(sample_rate);
        burst_env_.set_sample_rate(sample_rate);
        tail_env_.set_sample_rate(sample_rate);
        filter_.set_sample_rate(static_cast<float>(sample_rate));
        filter_.set_mode(Svf::Mode::bandpass);
        output_.prepare(sample_rate);
    }

    void on_reset() override {
        burst_env_.reset();
        tail_env_.reset();
        filter_.reset();
        output_.reset();
        noise_.reset();
        body_phase_ = 0.0;
        next_burst_ = 0;
        samples_until_burst_ = 0;
        burst_gain_ = 1.0;
    }

    void on_note_on(float velocity) override {
        const auto& response = velocity_response();
        velocity_gain_ = response.gain(velocity);
        brightness_ = response.brightness_scale(velocity);

        noise_.reset();
        body_phase_ = 0.0;
        // The band filter shapes the strike rather than modelling a body, so
        // it starts clean on every hit. Voices whose filter *is* a body -- the
        // kick's resonator and circuit network, the snare's shell, the hat's
        // oscillator bank -- deliberately carry their state across hits
        // instead, because a struck body is not re-struck from rest.
        filter_.reset();

        filter_.set_frequency(static_cast<float>(
            std::min(cutoff_hz_ * brightness_, 0.49 * sample_rate())));
        filter_.set_resonance(static_cast<float>(resonance_));

        spacing_samples_ = std::max(1, static_cast<int>(0.001 * spacing_ms_ * sample_rate()));

        burst_env_.set_attack_ms(0.0);
        burst_env_.set_decay_time_constant_ms(burst_decay_ms_);
        tail_env_.set_attack_ms(0.5);
        tail_env_.set_decay_t60_ms(tail_decay_ms_);

        // Fire the first burst immediately and schedule the rest; the tail runs
        // from the same instant so it is already present under burst one.
        burst_gain_ = 1.0;
        burst_env_.trigger();
        tail_env_.trigger();
        next_burst_ = 1;
        samples_until_burst_ = spacing_samples_;
    }

    bool on_is_active() const override {
        return burst_env_.is_active() || tail_env_.is_active() || next_burst_ < burst_count_;
    }

    void render_add(float* out, int num_samples) override {
        for (int i = 0; i < num_samples; ++i) {
            if (next_burst_ < burst_count_) {
                if (--samples_until_burst_ <= 0) {
                    burst_gain_ *= falloff_;
                    burst_env_.trigger();
                    ++next_burst_;
                    samples_until_burst_ = spacing_samples_;
                }
            }

            const double filtered = filter_.process(noise_.process());
            const double train = filtered * burst_env_.process() * burst_gain_;
            const double tail_env = tail_env_.process();
            const double room = filtered * tail_env * tail_level_;

            double body = 0.0;
            if (body_level_ > 0.0) {
                body_phase_ += body_hz_ / sample_rate();
                if (body_phase_ >= 1.0) body_phase_ -= std::floor(body_phase_);
                body = std::sin(2.0 * 3.14159265358979323846 * body_phase_) * tail_env *
                       body_level_;
            }

            out[i] += static_cast<float>(
                output_.process(static_cast<float>(train + room + body)) * velocity_gain_);
        }
    }

private:
    int burst_count_ = 4;
    double spacing_ms_ = 11.0;
    double burst_decay_ms_ = 6.0;
    double falloff_ = 0.82;
    double tail_level_ = 0.35;
    double tail_decay_ms_ = 180.0;
    double cutoff_hz_ = 1400.0;
    double resonance_ = 1.4;
    double body_level_ = 0.0;
    double body_hz_ = 300.0;

    NoiseSource noise_;
    DecayEnvelope64 burst_env_;
    DecayEnvelope64 tail_env_;
    Svf filter_;
    OutputStage output_;

    double velocity_gain_ = 1.0;
    double brightness_ = 1.0;
    double body_phase_ = 0.0;

    int spacing_samples_ = 1;
    int next_burst_ = 0;
    int samples_until_burst_ = 0;
    double burst_gain_ = 1.0;
};

}  // namespace pulp::signal::drum
