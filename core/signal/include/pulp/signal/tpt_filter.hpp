#pragma once

/// @file tpt_filter.hpp
/// First-order topology-preserving transform (TPT) filter.
/// Modulation-stable: no transients when sweeping cutoff.

#include <pulp/signal/denormal.hpp>

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
template <typename SampleType = float>
class TptFilterT {
public:
    TptFilterT() = default;

    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate;
        update_coefficient();
    }

    /// Set cutoff frequency in Hz. Safe to modulate per-sample.
    void set_cutoff(SampleType hz) {
        cutoff_ = std::clamp(hz, SampleType{1.0f}, sample_rate_ * SampleType{0.49f});
        update_coefficient();
    }

    SampleType cutoff() const { return cutoff_; }

    /// Process and return the lowpass output.
    SampleType process_lowpass(SampleType input) {
        SampleType v = g_ * (input - state_);
        SampleType lp = v + state_;
        // Snap the integrator state to flush denormal tails where no FTZ guard
        // applies. No-op above 1e-15.
        state_ = snap_to_zero(lp + v);
        return lp;
    }

    /// Process and return the highpass output.
    SampleType process_highpass(SampleType input) {
        return input - process_lowpass(input);
    }

    /// Process and return the allpass output.
    SampleType process_allpass(SampleType input) {
        SampleType lp = process_lowpass(input);
        return SampleType{2.0f} * lp - input;
    }

    /// Process and return all three outputs at once.
    struct Outputs { SampleType lowpass, highpass, allpass; };

    Outputs process(SampleType input) {
        SampleType v = g_ * (input - state_);
        SampleType lp = v + state_;
        state_ = snap_to_zero(lp + v);  // flush denormal tails (see above)
        SampleType hp = input - lp;
        SampleType ap = lp - hp; // = 2*lp - input
        return {lp, hp, ap};
    }

    void reset() { state_ = SampleType{0.0f}; }

private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType cutoff_ = SampleType{1000.0f};
    SampleType g_ = SampleType{0.0f};
    SampleType state_ = SampleType{0.0f};

    void update_coefficient() {
        constexpr SampleType pi = SampleType{3.14159265358979323846f};
        SampleType wd = SampleType{2.0f} * pi * cutoff_;
        SampleType wa = (SampleType{2.0f} * sample_rate_) *
                        std::tan(wd / (SampleType{2.0f} * sample_rate_));
        g_ = wa / (SampleType{2.0f} * sample_rate_ + wa);
    }
};

using TptFilter = TptFilterT<float>;
using TptFilter64 = TptFilterT<double>;

} // namespace pulp::signal
