#pragma once

#include <algorithm>
#include <cmath>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/special_functions.hpp>
#include <type_traits>

namespace pulp::signal {

// Simple gain processor.
// RT contract: setters and process paths are scalar-only and allocate no
// memory.
template <typename SampleType = float> class GainT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "GainT requires a floating-point sample type");

  public:
    /// Enable a linear per-sample gain ramp. The default time is one sample,
    /// preserving the historical immediate-set behavior.
    void set_ramp_time(SampleType seconds, SampleType sample_rate) {
        gain_ramp_.set_ramp_time(seconds, sample_rate);
        ramp_enabled_ = true;
    }

    void set_gain_db(SampleType db) {
        set_gain_linear(std::pow(SampleType{10}, db / SampleType{20}));
    }
    void set_gain_linear(SampleType linear) {
        if (ramp_enabled_)
            gain_ramp_.set_target(linear);
        else
            gain_ramp_.set_immediate(linear);
    }
    SampleType gain_db() const {
        return SampleType{20} * std::log10(std::max(gain_ramp_.current(), SampleType{1e-10f}));
    }
    SampleType gain_linear() const {
        return gain_ramp_.current();
    }

    SampleType process(SampleType input) const {
        return input * gain_ramp_.next();
    }

    void process(SampleType* buffer, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] *= gain_ramp_.next();
    }

  private:
    mutable SmoothedValue<SampleType> gain_ramp_{SampleType{1}};
    bool ramp_enabled_ = false;
};

// Simple dry/wet mixer (single-sample, no latency compensation).
// RT contract: setters and process paths are scalar-only and allocate no
// memory. For the full multi-channel version with latency compensation, use
// dry_wet_mixer.hpp.
template <typename SampleType = float> class SimpleMixerT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "SimpleMixerT requires a floating-point sample type");

  public:
    void set_mix(SampleType mix) {
        mix_ = std::clamp(mix, SampleType{0}, SampleType{1});
    }
    SampleType mix() const {
        return mix_;
    }

    SampleType process(SampleType dry, SampleType wet) const {
        return dry * (SampleType{1} - mix_) + wet * mix_;
    }

    void process(const SampleType* dry, const SampleType* wet, SampleType* output,
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
