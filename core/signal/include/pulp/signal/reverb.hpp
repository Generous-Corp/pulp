#pragma once

#include <algorithm>
#include <pulp/signal/delay_line.hpp>
#include <array>
#include <cmath>

namespace pulp::signal {

// Feedback Delay Network (FDN) reverb
// 4-channel FDN with Hadamard mixing matrix.
//
// RT contract: `prepare()` allocates delay-line storage. After preparation,
// setters, `process()`, and `reset()` allocate no memory.
class Reverb {
public:
    void prepare(float sample_rate) {
        sample_rate_ = sample_rate;
        // Prime delay lengths for maximum density (in samples)
        int delays[] = {1087, 1283, 1481, 1693};
        for (int i = 0; i < 4; ++i) {
            lines_[i].prepare(static_cast<int>(delays[i] * sample_rate / 44100.0f) + 1);
            delay_samples_[i] = static_cast<int>(delays[i] * sample_rate / 44100.0f);
        }
    }

    void set_decay(float seconds) { decay_ = seconds; }
    void set_damping(float d) { damping_ = std::clamp(d, 0.0f, 0.99f); }
    void set_mix(float m) { mix_ = m; }

    struct StereoSample { float left, right; };

    StereoSample process(float input) {
        // Read from delay lines
        float s[4];
        for (int i = 0; i < 4; ++i)
            s[i] = lines_[i].read(delay_samples_[i]);

        // Hadamard mixing (unitary, energy preserving)
        float h[4];
        h[0] = 0.5f * (s[0] + s[1] + s[2] + s[3]);
        h[1] = 0.5f * (s[0] - s[1] + s[2] - s[3]);
        h[2] = 0.5f * (s[0] + s[1] - s[2] - s[3]);
        h[3] = 0.5f * (s[0] - s[1] - s[2] + s[3]);

        // Feedback with decay and damping
        float feedback = decay_feedback();
        for (int i = 0; i < 4; ++i) {
            // One-pole lowpass for damping
            lp_state_[i] += (h[i] - lp_state_[i]) * (1.0f - damping_);
            lines_[i].push(input + lp_state_[i] * feedback);
        }

        // Stereo output (pick pairs)
        float wet_l = (s[0] + s[2]) * 0.5f;
        float wet_r = (s[1] + s[3]) * 0.5f;

        float dry = 1.0f - mix_;
        return {input * dry + wet_l * mix_,
                input * dry + wet_r * mix_};
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        lp_state_.fill(0);
    }

private:
    float sample_rate_ = 44100.0f;
    float decay_ = 2.0f;
    float damping_ = 0.3f;
    float mix_ = 0.3f;

    std::array<DelayLine, 4> lines_;
    std::array<int, 4> delay_samples_{};
    std::array<float, 4> lp_state_{};

    float decay_feedback() const {
        // Average delay time
        float avg_delay = 0;
        for (int i = 0; i < 4; ++i)
            avg_delay += delay_samples_[i];
        avg_delay /= (4.0f * sample_rate_);

        // RT60-based feedback
        if (decay_ <= 0 || avg_delay <= 0) return 0;
        return std::pow(10.0f, -3.0f * avg_delay / decay_);
    }
};

} // namespace pulp::signal
