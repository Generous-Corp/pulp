#pragma once

#include <cmath>

namespace pulp::signal {

// dB <-> linear conversion utilities
inline float db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }
inline float linear_to_db(float linear) { return 20.0f * std::log10(std::max(linear, 1e-10f)); }

// Simple gain processor
class Gain {
public:
    void set_gain_db(float db) { gain_ = db_to_linear(db); }
    void set_gain_linear(float linear) { gain_ = linear; }
    float gain_db() const { return linear_to_db(gain_); }
    float gain_linear() const { return gain_; }

    float process(float input) const { return input * gain_; }

    void process(float* buffer, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] *= gain_;
    }

private:
    float gain_ = 1.0f;
};

// Simple dry/wet mixer (single-sample, no latency compensation)
// For the full multi-channel version with latency compensation, use dry_wet_mixer.hpp
class SimpleMixer {
public:
    void set_mix(float mix) { mix_ = std::clamp(mix, 0.0f, 1.0f); }
    float mix() const { return mix_; }

    float process(float dry, float wet) const {
        return dry * (1.0f - mix_) + wet * mix_;
    }

    void process(const float* dry, const float* wet, float* output, int num_samples) const {
        float d = 1.0f - mix_;
        for (int i = 0; i < num_samples; ++i)
            output[i] = dry[i] * d + wet[i] * mix_;
    }

private:
    float mix_ = 1.0f; // 1.0 = fully wet
};

} // namespace pulp::signal
