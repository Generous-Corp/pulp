#pragma once

#include <pulp/signal/decay_envelope.hpp>
#include <pulp/signal/drum/voice.hpp>
#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

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
/// `process()` is the canonical mono realization. `process_stereo()` places
/// successive bursts on alternating sides while keeping the tail and optional
/// body in the centre. The two stereo channels sum to the canonical mono
/// signal sample for sample, so a host can collapse the result without a level
/// or timbre change.
///
/// What a mono sum does cost is decorrelation. Identically spaced, identically
/// signed bursts are coherent with each other, so summing them combs: the train
/// acquires a faint pitch at one over the gap that no single burst has. Two
/// controls exist to break that up, and at least one of them wants to be on:
/// `set_gap_jitter` varies the spacing per burst, and
/// `set_alternate_polarity` flips every other burst. Both stay deterministic,
/// so a render is still reproducible.
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

    /// How much each gap varies from the nominal spacing, 0 to 1. Perfectly
    /// even bursts comb against each other and give the train a pitch; hands
    /// are never that regular anyway. Driven by a fixed sequence, so this adds
    /// irregularity without costing reproducibility.
    void set_gap_jitter(double amount) { gap_jitter_ = std::clamp(amount, 0.0, 1.0); }

    /// Invert every other burst. A cheaper decorrelation than jitter and free
    /// of any timing change: opposite-signed bursts cannot reinforce each
    /// other's comb even at exactly even spacing.
    void set_alternate_polarity(bool alternate) { alternate_polarity_ = alternate; }

    /// Alternating burst width, 0 for mono through 1 for hard left/right. The
    /// room tail and optional tonal body remain centred at every setting.
    void set_stereo_width(double width) {
        stereo_width_ = std::clamp(width, 0.0, 1.0);
    }

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
        noise_.prepare(sample_rate);
        burst_env_.set_sample_rate(sample_rate);
        tail_env_.set_sample_rate(sample_rate);
        filter_.set_sample_rate(static_cast<float>(sample_rate));
        filter_.set_mode(Svf::Mode::bandpass);
        output_.prepare(sample_rate);
        burst_output_.prepare(sample_rate);
    }

    void on_reset() override {
        burst_env_.reset();
        tail_env_.reset();
        filter_.reset();
        output_.reset();
        burst_output_.reset();
        stereo_path_active_ = false;
        noise_.reset();
        body_phase_ = 0.0;
        next_burst_ = 0;
        samples_until_burst_ = 0;
        burst_gain_ = 1.0;
        burst_sign_ = 1.0;
        burst_pan_ = -1.0;
        reset_pan_delay();
        jitter_state_ = kJitterSeed;
    }

    void on_note_on(float velocity) override {
        output_.reset_nonlinear_state();
        output_.trigger();
        if (stereo_path_active_) {
            burst_output_.sync_configuration_from(output_);
            burst_output_.reset_nonlinear_state();
            burst_output_.trigger();
        }
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
        burst_sign_ = 1.0;
        burst_pan_ = -1.0;
        jitter_state_ = kJitterSeed;
        burst_env_.trigger();
        tail_env_.trigger();
        next_burst_ = 1;
        samples_until_burst_ = next_gap();
    }

    bool on_is_active() const override {
        return burst_env_.is_active() || tail_env_.is_active() ||
               next_burst_ < burst_count_ || output_.has_tail();
    }

    void render_add(float* out, int num_samples) override {
        // A later switch back to stereo must start from fresh side-path state;
        // the optional processor is deliberately not advanced by mono blocks.
        stereo_path_active_ = false;
        for (int i = 0; i < num_samples; ++i) {
            out[i] += render_frame(false).mono;
        }
    }

    void render_add_stereo(float* left, float* right,
                           int num_samples) override {
        const bool quality_changed =
            burst_output_.oversampling() != output_.oversampling();
        burst_output_.sync_configuration_from(output_);
        if (!stereo_path_active_ || quality_changed) {
            burst_output_.reset();
            burst_output_.trigger();
            reset_pan_delay();
            stereo_path_active_ = true;
        }
        for (int i = 0; i < num_samples; ++i) {
            const RenderFrame frame = render_frame(true);
            const double pan = frame.burst_pan * stereo_width_;
            left[i] += static_cast<float>(
                0.5 * frame.mono - 0.5 * pan * frame.burst);
            right[i] += static_cast<float>(
                0.5 * frame.mono + 0.5 * pan * frame.burst);
        }
    }

private:
    struct RenderFrame {
        float mono = 0.0f;
        double burst = 0.0;
        double burst_pan = 0.0;
    };

    RenderFrame render_frame(bool render_burst_side) {
        if (next_burst_ < burst_count_ && --samples_until_burst_ <= 0) {
            burst_gain_ *= falloff_;
            if (alternate_polarity_) burst_sign_ = -burst_sign_;
            burst_pan_ = (next_burst_ & 1) != 0 ? 1.0 : -1.0;
            burst_env_.trigger();
            ++next_burst_;
            samples_until_burst_ = next_gap();
        }

        const double filtered = filter_.process(noise_.process());
        const double train =
            filtered * burst_env_.process() * burst_gain_ * burst_sign_;
        const double tail_env = tail_env_.process();
        const double room = filtered * tail_env * tail_level_;

        double body = 0.0;
        if (body_level_ > 0.0) {
            body_phase_ += body_hz_ / sample_rate();
            if (body_phase_ >= 1.0) body_phase_ -= std::floor(body_phase_);
            body = std::sin(2.0 * 3.14159265358979323846 * body_phase_) *
                   tail_env * body_level_;
        }

        const double centre = room + body;

        RenderFrame frame;
        frame.mono = static_cast<float>(
            output_.process(static_cast<float>(train + centre)) * velocity_gain_);
        // The side channel carries only the burst train. Any nonlinear
        // interaction between the train and centred layers remains in the
        // mid channel, which preserves the canonical mono sum exactly without
        // allowing room or body energy to leak into stereo width.
        if (render_burst_side) {
            frame.burst =
                burst_output_.process(static_cast<float>(train)) * velocity_gain_;
        }
        frame.burst_pan = delayed_burst_pan();
        return frame;
    }

    void reset_pan_delay() {
        burst_pan_delay_.fill(burst_pan_);
        burst_pan_write_ = 0;
    }

    double delayed_burst_pan() {
        burst_pan_delay_[static_cast<std::size_t>(burst_pan_write_)] =
            burst_pan_;
        const int size = static_cast<int>(burst_pan_delay_.size());
        const int delay = burst_output_.latency_samples();
        const int read = (burst_pan_write_ - delay + size) % size;
        const double delayed =
            burst_pan_delay_[static_cast<std::size_t>(read)];
        burst_pan_write_ = (burst_pan_write_ + 1) % size;
        return delayed;
    }

    // A fixed seed for the gap sequence, kept separate from the voice's audio
    // noise so that changing the jitter does not also change the noise the
    // bursts are made of.
    static constexpr std::uint32_t kJitterSeed = 0x2545F491u;

    // Next gap in samples, perturbed by up to +/- half the jitter amount.
    int next_gap() {
        if (gap_jitter_ <= 0.0) return spacing_samples_;
        jitter_state_ ^= jitter_state_ << 13;
        jitter_state_ ^= jitter_state_ >> 17;
        jitter_state_ ^= jitter_state_ << 5;
        const double bipolar =
            static_cast<double>(jitter_state_) * (1.0 / 2147483648.0) - 1.0;
        const double scaled =
            static_cast<double>(spacing_samples_) * (1.0 + 0.5 * gap_jitter_ * bipolar);
        return std::max(1, static_cast<int>(scaled));
    }

    int burst_count_ = 4;
    double spacing_ms_ = 11.0;
    double burst_decay_ms_ = 6.0;
    double falloff_ = 0.82;
    // On by default: an unjittered train combs audibly, and a clap that needs
    // a control turned on to stop sounding wrong is a clap with a bad default.
    double gap_jitter_ = 0.35;
    bool alternate_polarity_ = false;
    double stereo_width_ = 1.0;
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
    OutputStage burst_output_;
    bool stereo_path_active_ = false;
    static constexpr int kMaxOutputLatency =
        OutputStage::latency_samples_for(OutputOversampling::x4);
    std::array<double, kMaxOutputLatency + 1> burst_pan_delay_{};
    int burst_pan_write_ = 0;

    double velocity_gain_ = 1.0;
    double brightness_ = 1.0;
    double body_phase_ = 0.0;

    int spacing_samples_ = 1;
    int next_burst_ = 0;
    int samples_until_burst_ = 0;
    double burst_gain_ = 1.0;
    double burst_sign_ = 1.0;
    double burst_pan_ = -1.0;
    std::uint32_t jitter_state_ = kJitterSeed;
};

}  // namespace pulp::signal::drum
