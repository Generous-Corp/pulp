#pragma once

// Bias — adds a constant DC offset to a signal.
// Trivial but included for API completeness.

#include <cstddef>

namespace pulp::signal {

// RT contract: setters, reset(), set_sample_rate(), and process paths are
// scalar-only and allocate no memory.
template <typename SampleType = float>
class BiasT {
public:
    void set_bias(SampleType b) { bias_ = b; }
    SampleType bias() const { return bias_; }

    SampleType process(SampleType input) const { return input + bias_; }

    void process(SampleType* buffer, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] += bias_;
    }

    void process(const SampleType* input, SampleType* output, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            output[i] = input[i] + bias_;
    }

    void reset() {}
    void set_sample_rate(SampleType) {}

private:
    SampleType bias_ = SampleType{0.0f};
};

using Bias = BiasT<float>;
using Bias64 = BiasT<double>;

}  // namespace pulp::signal
