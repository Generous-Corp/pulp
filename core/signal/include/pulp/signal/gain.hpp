#pragma once

#include <algorithm>
#include <cmath>
#include <pulp/signal/special_functions.hpp>
#include <type_traits>

namespace pulp::signal {

// Simple gain processor.
// RT contract: setters and process paths are scalar-only and allocate no
// memory.
template <typename SampleType = float>
class GainT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "GainT requires a floating-point sample type");

public:
    void set_gain_db(SampleType db) {
        gain_ = std::pow(SampleType{10}, db / SampleType{20});
    }
    void set_gain_linear(SampleType linear) { gain_ = linear; }
    SampleType gain_db() const {
        return SampleType{20} *
               std::log10(std::max(gain_, SampleType{1e-10f}));
    }
    SampleType gain_linear() const { return gain_; }

    SampleType process(SampleType input) const { return input * gain_; }

    void process(SampleType* buffer, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] *= gain_;
    }

private:
    SampleType gain_ = SampleType{1};
};

// Simple dry/wet mixer (single-sample, no latency compensation).
// RT contract: setters and process paths are scalar-only and allocate no
// memory. For the full multi-channel version with latency compensation, use
// dry_wet_mixer.hpp.
template <typename SampleType = float>
class SimpleMixerT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "SimpleMixerT requires a floating-point sample type");

public:
    void set_mix(SampleType mix) {
        mix_ = std::clamp(mix, SampleType{0}, SampleType{1});
    }
    SampleType mix() const { return mix_; }

    SampleType process(SampleType dry, SampleType wet) const {
        return dry * (SampleType{1} - mix_) + wet * mix_;
    }

    void process(const SampleType* dry,
                 const SampleType* wet,
                 SampleType* output,
                 int num_samples) const {
        const SampleType d = SampleType{1} - mix_;
        for (int i = 0; i < num_samples; ++i)
            output[i] = dry[i] * d + wet[i] * mix_;
    }

private:
    SampleType mix_ = SampleType{1}; // 1.0 = fully wet
};

using Gain = GainT<float>;
using Gain64 = GainT<double>;
using SimpleMixer = SimpleMixerT<float>;
using SimpleMixer64 = SimpleMixerT<double>;

} // namespace pulp::signal
