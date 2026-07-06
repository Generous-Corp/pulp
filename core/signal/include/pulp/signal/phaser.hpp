#pragma once

#include <algorithm>
#include <cmath>
#include <array>

namespace pulp::signal {

// Phaser effect using cascaded allpass filters with LFO modulation.
// RT contract: configuration setters, process, and reset use fixed member
// storage and allocate no memory.
template <typename SampleType = float>
class PhaserT {
public:
    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }
    void set_rate(SampleType hz) { rate_ = hz; }       // LFO rate
    void set_depth(SampleType d) { depth_ = d; }       // Modulation depth (0-1)
    void set_feedback(SampleType fb) {
        feedback_ = std::clamp(fb, SampleType{-0.95f}, SampleType{0.95f});
    }
    void set_mix(SampleType m) { mix_ = m; }
    void set_stages(int n) { stages_ = std::clamp(n, 2, max_stages); }

    SampleType process(SampleType input) {
        // LFO
        SampleType lfo = std::sin(SampleType{2.0f} * pi * phase_) *
                         SampleType{0.5f} + SampleType{0.5f};
        phase_ += rate_ / sample_rate_;
        if (phase_ >= SampleType{1.0f}) phase_ -= SampleType{1.0f};

        // Map LFO to frequency range (200Hz - 5000Hz)
        SampleType min_freq = SampleType{200.0f};
        SampleType max_freq = SampleType{5000.0f};
        SampleType freq = min_freq + lfo * depth_ * (max_freq - min_freq);

        // Allpass coefficient
        SampleType w0 = SampleType{2.0f} * pi * freq / sample_rate_;
        SampleType coeff = (SampleType{1.0f} - std::tan(w0 * SampleType{0.5f})) /
                           (SampleType{1.0f} + std::tan(w0 * SampleType{0.5f}));

        // Process through allpass chain
        SampleType x = input + feedback_state_ * feedback_;
        for (int i = 0; i < stages_; ++i) {
            SampleType y = coeff * (x - ap_state_[i * 2 + 1]) + ap_state_[i * 2];
            ap_state_[i * 2] = x;
            ap_state_[i * 2 + 1] = y;
            x = y;
        }
        feedback_state_ = x;

        return input * (SampleType{1.0f} - mix_) + x * mix_;
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        ap_state_.fill(0);
        feedback_state_ = 0;
        phase_ = 0;
    }

private:
    static constexpr SampleType pi = SampleType{3.14159265358979323846f};
    static constexpr int max_stages = 8;

    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType rate_ = SampleType{0.5f};
    SampleType depth_ = SampleType{0.7f};
    SampleType feedback_ = SampleType{0.5f};
    SampleType mix_ = SampleType{0.5f};
    int stages_ = 4;
    SampleType phase_ = 0;
    SampleType feedback_state_ = 0;
    std::array<SampleType, max_stages * 2> ap_state_{};
};

using Phaser = PhaserT<float>;
using Phaser64 = PhaserT<double>;

} // namespace pulp::signal
