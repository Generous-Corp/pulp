#pragma once

#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Feed-forward compressor with adjustable attack/release
// Real-time safe. Operates on single samples.
class Compressor {
public:
    struct Params {
        float threshold_db = -20.0f;  // Compression threshold
        float ratio = 4.0f;           // Compression ratio (4:1)
        float attack_ms = 5.0f;       // Attack time in ms
        float release_ms = 100.0f;    // Release time in ms
        float knee_db = 6.0f;         // Soft knee width in dB (0 = hard knee)
        float makeup_db = 0.0f;       // Makeup gain
    };

    void set_params(const Params& p) { params_ = p; }
    void set_sample_rate(float sr) { sample_rate_ = sr; }

    float process(float input) {
        float input_db = 20.0f * std::log10(std::max(std::abs(input), 1e-10f));

        // Compute gain reduction
        float gain_db = compute_gain(input_db);

        // Envelope follower (smooth the gain)
        float target = gain_db;
        float coeff = (target < envelope_db_)
            ? attack_coeff()
            : release_coeff();

        envelope_db_ = envelope_db_ + coeff * (target - envelope_db_);

        // Apply gain
        float gain_linear = std::pow(10.0f, (envelope_db_ + params_.makeup_db) / 20.0f);
        return input * gain_linear;
    }

    void process(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    float gain_reduction_db() const { return envelope_db_; }

    void reset() { envelope_db_ = 0.0f; }

private:
    Params params_;
    float sample_rate_ = 44100.0f;
    float envelope_db_ = 0.0f;

    float compute_gain(float input_db) const {
        float knee = params_.knee_db;
        float threshold = params_.threshold_db;
        float ratio = params_.ratio;

        if (knee <= 0.0f) {
            // Hard knee
            if (input_db <= threshold) return 0.0f;
            return (input_db - threshold) * (1.0f / ratio - 1.0f);
        }

        // Soft knee
        float half_knee = knee / 2.0f;
        if (input_db < threshold - half_knee)
            return 0.0f;
        if (input_db > threshold + half_knee)
            return (input_db - threshold) * (1.0f / ratio - 1.0f);

        // In the knee region
        float x = input_db - threshold + half_knee;
        return (1.0f / ratio - 1.0f) * x * x / (2.0f * knee);
    }

    float attack_coeff() const {
        if (params_.attack_ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (params_.attack_ms * 0.001f * sample_rate_));
    }

    float release_coeff() const {
        if (params_.release_ms <= 0.0f) return 1.0f;
        return 1.0f - std::exp(-1.0f / (params_.release_ms * 0.001f * sample_rate_));
    }
};

// Brickwall limiter — hard limit at threshold with lookahead-free design
class Limiter {
public:
    void set_threshold_db(float db) { threshold_ = std::pow(10.0f, db / 20.0f); }
    void set_release_ms(float ms) { release_ms_ = ms; }
    void set_sample_rate(float sr) { sample_rate_ = sr; }

    float process(float input) {
        float abs_input = std::abs(input);

        // Envelope follower
        if (abs_input > envelope_)
            envelope_ = abs_input; // Instant attack
        else {
            float coeff = 1.0f - std::exp(-1.0f / (release_ms_ * 0.001f * sample_rate_));
            envelope_ += coeff * (abs_input - envelope_);
        }

        // Compute gain
        float gain = (envelope_ > threshold_) ? threshold_ / envelope_ : 1.0f;
        return input * gain;
    }

    void process(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() { envelope_ = 0.0f; }

private:
    float threshold_ = 1.0f;
    float release_ms_ = 50.0f;
    float sample_rate_ = 44100.0f;
    float envelope_ = 0.0f;
};

} // namespace pulp::signal
