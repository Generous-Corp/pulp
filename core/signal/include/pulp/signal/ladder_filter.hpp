#pragma once

#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Moog-style ladder filter (4-pole, 24dB/oct)
// Non-linear, self-oscillating at high resonance.
//
// RT contract: setters, process paths, and reset are scalar/fixed-array only
// and allocate no memory.
class LadderFilter {
public:
    void set_sample_rate(float sr) { sample_rate_ = sr; }
    void set_frequency(float hz) { cutoff_ = hz; update(); }
    void set_resonance(float r) { resonance_ = std::clamp(r, 0.0f, 1.0f); update(); }

    float process(float input) {
        // Non-linear feedback
        float feedback = resonance_ * 4.0f * (stage_[3] - input * 0.5f);
        float x = input - feedback;

        // 4 cascaded one-pole filters with tanh saturation
        for (int i = 0; i < 4; ++i) {
            float prev = i > 0 ? stage_[i - 1] : x;
            stage_[i] += g_ * (std::tanh(prev) - std::tanh(stage_[i]));
        }

        return stage_[3];
    }

    void process(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        for (auto& s : stage_) s = 0;
    }

private:
    float sample_rate_ = 44100.0f;
    float cutoff_ = 1000.0f;
    float resonance_ = 0.0f;
    float g_ = 0;
    float stage_[4] = {};

    void update() {
        g_ = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff_ / sample_rate_);
    }
};

} // namespace pulp::signal
