#pragma once

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Moog-style ladder filter (4-pole, 24dB/oct)
// Non-linear, self-oscillating at high resonance.
//
// RT contract: setters, process paths, and reset are scalar/fixed-array only
// and allocate no memory.
template <typename SampleType = float>
class LadderFilterT {
public:
    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }
    void set_frequency(SampleType hz) { cutoff_ = hz; update(); }
    void set_resonance(SampleType r) {
        resonance_ = std::clamp(r, SampleType{0.0f}, SampleType{1.0f});
        update();
    }

    SampleType process(SampleType input) {
        // Non-linear feedback
        SampleType feedback =
            resonance_ * SampleType{4.0f} * (stage_[3] - input * SampleType{0.5f});
        SampleType x = input - feedback;

        // 4 cascaded one-pole filters with tanh saturation
        for (int i = 0; i < 4; ++i) {
            SampleType prev = i > 0 ? stage_[i - 1] : x;
            // Snap each ladder stage: at high resonance the stages self-
            // oscillate and their tails otherwise decay into denormals with
            // no FTZ guard. No-op above 1e-15.
            stage_[i] = snap_to_zero(
                stage_[i] + g_ * (std::tanh(prev) - std::tanh(stage_[i])));
        }

        return stage_[3];
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        for (auto& s : stage_) s = 0;
    }

private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType cutoff_ = SampleType{1000.0f};
    SampleType resonance_ = SampleType{0.0f};
    SampleType g_ = 0;
    SampleType stage_[4] = {};

    void update() {
        g_ = SampleType{1.0f} -
             std::exp(SampleType{-2.0f} * SampleType{3.14159265f} *
                      cutoff_ / sample_rate_);
    }
};

using LadderFilter = LadderFilterT<float>;
using LadderFilter64 = LadderFilterT<double>;

} // namespace pulp::signal
