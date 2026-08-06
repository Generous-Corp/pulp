#pragma once

// MAGNETO — the tape echo the panel is a face for.
//
// Kept beside the Processor rather than inside it so the DSP can be tested
// without constructing a plugin: the audio proof runs this directly, and the
// plugin is then only responsible for wiring parameters to it.
//
// A `Processor` with a fixed chain, not a SignalGraph: the routing is known at
// build time. Delay, feedback and a one-pole in the loop is a graph only in the
// sense that every DSP is.

#include <pulp/signal/delay_line.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::examples {

/// One channel of magnetic echo.
class TapeEchoChannel {
public:
    /// `max_delay_seconds` sizes the line once. Everything after prepare() is
    /// allocation-free, which is the contract the audio thread needs.
    void prepare(double sample_rate, double max_delay_seconds = 2.0) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        line_.prepare(static_cast<int>(sample_rate_ * max_delay_seconds) + 4);
        reset();
    }

    void reset() {
        line_.reset();
        lowpass_state_ = 0.0f;
        smoothed_delay_ = target_delay_;
        level_ = 0.0f;
    }

    /// Delay in samples. Smoothed toward, never jumped to: stepping the read
    /// head produces a click, and on a tape echo the pitch slew while the head
    /// moves is the sound people are reaching for.
    void set_delay_samples(float samples) {
        const float max_delay =
            static_cast<float>(sample_rate_ * 2.0);
        target_delay_ = std::clamp(samples, 1.0f, max_delay);
    }

    /// 0..1. Held below unity — a tape echo self-oscillates musically, but a
    /// plugin that can run away on a parameter the user dragged is a defect,
    /// not a feature.
    void set_feedback(float amount) {
        feedback_ = std::clamp(amount, 0.0f, 0.98f);
    }

    /// 0..1, dark → bright. Each repeat passes the loop filter again, so the
    /// tail darkens progressively the way tape does rather than every repeat
    /// sharing one fixed timbre.
    void set_tone(float tone) {
        const float t = std::clamp(tone, 0.0f, 1.0f);
        // 400 Hz .. 12 kHz, geometric so the knob feels even across its travel.
        const float hz = 400.0f * std::pow(30.0f, t);
        const float w = static_cast<float>(
            1.0 - std::exp(-2.0 * 3.14159265358979 * hz / sample_rate_));
        lowpass_coeff_ = std::clamp(w, 0.0f, 1.0f);
    }

    /// Returns the WET sample. The caller owns dry/wet, so this stays a pure
    /// echo generator and the mixer can be tested on its own.
    float process(float input) {
        // One-pole slew on the read position. 20ms reaches a new setting fast
        // enough to feel immediate and slow enough not to click.
        const float slew = static_cast<float>(
            1.0 - std::exp(-1.0 / (0.020 * sample_rate_)));
        smoothed_delay_ += (target_delay_ - smoothed_delay_) * slew;

        const float delayed = line_.read(smoothed_delay_);

        // The loop filter sits INSIDE the feedback path. Outside it, every
        // repeat is equally bright and only the final output is dark, which
        // sounds like an EQ over an echo rather than like tape.
        lowpass_state_ += lowpass_coeff_ * (delayed - lowpass_state_);

        // Denormals arrive as a tail decays toward zero and cost far more than
        // the arithmetic they carry; flushing keeps a quiet plugin cheap.
        if (std::fabs(lowpass_state_) < 1e-20f) lowpass_state_ = 0.0f;

        line_.push(input + lowpass_state_ * feedback_);

        // Output level for the meter, tracked here so the meter reads what was
        // actually produced rather than what a parameter implies.
        const float mag = std::fabs(lowpass_state_);
        level_ += (mag > level_ ? 0.30f : 0.002f) * (mag - level_);
        return lowpass_state_;
    }

    float level() const { return level_; }

private:
    signal::DelayLineT<float> line_;
    double sample_rate_ = 48000.0;
    float target_delay_ = 1.0f;
    float smoothed_delay_ = 1.0f;
    float feedback_ = 0.0f;
    float lowpass_coeff_ = 1.0f;
    float lowpass_state_ = 0.0f;
    float level_ = 0.0f;
};

/// The whole effect: N channels of echo plus the dry/wet blend.
class TapeEcho {
public:
    void prepare(double sample_rate, int channels) {
        channels_.resize(static_cast<std::size_t>(std::max(channels, 1)));
        for (auto& c : channels_) c.prepare(sample_rate);
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    }

    void reset() { for (auto& c : channels_) c.reset(); }

    void set_time_seconds(float seconds) {
        const float samples = static_cast<float>(
            std::clamp(seconds, 0.001f, 2.0f) * sample_rate_);
        for (auto& c : channels_) c.set_delay_samples(samples);
    }
    void set_feedback(float v) { for (auto& c : channels_) c.set_feedback(v); }
    void set_tone(float v) { for (auto& c : channels_) c.set_tone(v); }

    /// 0 = dry only, 1 = wet only. At 0 the output is the input unchanged —
    /// bit-identical, not merely quiet, which is what makes the claim testable.
    void set_mix(float v) { mix_ = std::clamp(v, 0.0f, 1.0f); }

    void process(float* const* io, int num_channels, int num_samples) {
        const int n = std::min(num_channels,
                               static_cast<int>(channels_.size()));
        for (int ch = 0; ch < n; ++ch) {
            auto& echo = channels_[static_cast<std::size_t>(ch)];
            float* buf = io[ch];
            for (int i = 0; i < num_samples; ++i) {
                const float dry = buf[i];
                const float wet = echo.process(dry);
                buf[i] = dry * (1.0f - mix_) + wet * mix_;
            }
        }
    }

    /// Loudest channel, for the panel's meter.
    float level() const {
        float peak = 0.0f;
        for (const auto& c : channels_) peak = std::max(peak, c.level());
        return peak;
    }

private:
    std::vector<TapeEchoChannel> channels_;
    double sample_rate_ = 48000.0;
    float mix_ = 0.0f;
};

}  // namespace pulp::examples
