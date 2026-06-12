#pragma once

/// @file tpt_filter.hpp
/// First-order topology-preserving transform (TPT) filter.
/// Modulation-stable: no transients when sweeping cutoff.

#include <cmath>
#include <algorithm>

namespace pulp::signal {

/// First-order TPT (trapezoidal integration) filter.
///
/// Provides lowpass, highpass, and allpass outputs simultaneously.
/// The TPT structure is unconditionally stable under modulation,
/// making it ideal for filter FM and fast cutoff sweeps.
///
/// RT contract: `prepare()`, cutoff modulation, process paths, queries, and
/// reset are fixed-state only and allocate no memory.
///
/// @code
/// TptFilter filt;
/// filt.prepare(44100.0f);
/// filt.set_cutoff(1000.0f);
/// float lp = filt.process_lowpass(input);
/// @endcode
class TptFilter {
public:
    TptFilter() = default;

    void prepare(float sample_rate) {
        sample_rate_ = sample_rate;
        update_coefficient();
    }

    /// Set cutoff frequency in Hz. Safe to modulate per-sample.
    void set_cutoff(float hz) {
        cutoff_ = std::clamp(hz, 1.0f, sample_rate_ * 0.49f);
        update_coefficient();
    }

    float cutoff() const { return cutoff_; }

    /// Process and return the lowpass output.
    float process_lowpass(float input) {
        float v = g_ * (input - state_);
        float lp = v + state_;
        state_ = lp + v;
        return lp;
    }

    /// Process and return the highpass output.
    float process_highpass(float input) {
        return input - process_lowpass(input);
    }

    /// Process and return the allpass output.
    float process_allpass(float input) {
        float lp = process_lowpass(input);
        return 2.0f * lp - input;
    }

    /// Process and return all three outputs at once.
    struct Outputs { float lowpass, highpass, allpass; };

    Outputs process(float input) {
        float v = g_ * (input - state_);
        float lp = v + state_;
        state_ = lp + v;
        float hp = input - lp;
        float ap = lp - hp; // = 2*lp - input
        return {lp, hp, ap};
    }

    void reset() { state_ = 0.0f; }

private:
    float sample_rate_ = 44100.0f;
    float cutoff_ = 1000.0f;
    float g_ = 0.0f;
    float state_ = 0.0f;

    void update_coefficient() {
        constexpr float pi = 3.14159265358979323846f;
        float wd = 2.0f * pi * cutoff_;
        float wa = (2.0f * sample_rate_) * std::tan(wd / (2.0f * sample_rate_));
        g_ = wa / (2.0f * sample_rate_ + wa);
    }
};

} // namespace pulp::signal
