#pragma once

// Bias — adds a constant DC offset to a signal.
// Trivial but included for API completeness.

#include <cstddef>

namespace pulp::signal {

class Bias {
public:
    void set_bias(float b) { bias_ = b; }
    float bias() const { return bias_; }

    float process(float input) const { return input + bias_; }

    void process(float* buffer, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] += bias_;
    }

    void process(const float* input, float* output, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            output[i] = input[i] + bias_;
    }

    void reset() {}
    void set_sample_rate(float) {}

private:
    float bias_ = 0.0f;
};

}  // namespace pulp::signal
