#pragma once

/// @file tpt_filter.hpp
/// First-order topology-preserving transform (TPT) filter.
/// Modulation-stable: no transients when sweeping cutoff.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>

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
template <typename SampleType = float> class TptFilterT {
  public:
    TptFilterT() = default;

    static constexpr SampleType kMinCutoffHz = SampleType{1};
    static constexpr SampleType kMaxCutoffFraction = SampleType{0.49};

    /// Conservative defensive floor that leaves a non-empty valid cutoff
    /// interval above the 1 Hz lower bound.
    static constexpr SampleType kMinSampleRate = SampleType{4};

    void prepare(SampleType sample_rate) {
        sample_rate_ = std::isfinite(sample_rate) && sample_rate >= kMinSampleRate ? sample_rate
                                                                                   : kMinSampleRate;
        cutoff_ = std::clamp(cutoff_, kMinCutoffHz, sample_rate_ * kMaxCutoffFraction);
        update_coefficient();
    }

    /// Set cutoff frequency in Hz. Safe to modulate per-sample.
    void set_cutoff(SampleType hz) {
        if (std::isnan(hz))
            hz = cutoff_;
        cutoff_ = std::clamp(hz, kMinCutoffHz, sample_rate_ * kMaxCutoffFraction);
        update_coefficient();
    }

    SampleType cutoff() const {
        return cutoff_;
    }
    SampleType sample_rate() const {
        return sample_rate_;
    }

    /// Process and return the lowpass output.
    ///
    /// The state update is written as `s + 2g * (input - s)` (algebraically
    /// `lp + v`) so the recursion is one subtract and one multiply-add; the
    /// lowpass output is formed off that path.
    SampleType process_lowpass(SampleType input) {
        const SampleType d = input - state_;
        const SampleType lp = g_ * d + state_;
        // Snap the integrator state to flush denormal tails where no FTZ guard
        // applies. No-op above 1e-15.
        state_ = snap_to_zero(two_g_ * d + state_);
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

    /// Process a buffer in place, replacing it with the lowpass output.
    ///
    /// Same arithmetic as the per-sample path on a register-resident state.
    /// The denormal snap runs every kStateSnapInterval samples and at the end
    /// of the call. The integrator's pole is 1 - 2g; unless |1 - 2g| < 0.44
    /// (cutoffs near a quarter of the sample rate) a state at or above 1e-15
    /// at the start of a chunk cannot decay into the float subnormal range
    /// before the next snap, and inside that band the tail falls through the
    /// subnormal range; there, and with subnormal input, the state can stay
    /// subnormal until the next snap, so residency is bounded by one chunk.
    void process_lowpass(SampleType* buffer, int num_samples) {
        process_block(buffer, num_samples, [](SampleType, SampleType lp) { return lp; });
    }

    /// Process a buffer in place, replacing it with the highpass output.
    void process_highpass(SampleType* buffer, int num_samples) {
        process_block(buffer, num_samples,
                      [](SampleType in, SampleType lp) { return in - lp; });
    }

    /// Process a buffer in place, replacing it with the allpass output.
    void process_allpass(SampleType* buffer, int num_samples) {
        process_block(buffer, num_samples,
                      [](SampleType in, SampleType lp) { return SampleType{2.0f} * lp - in; });
    }

    /// Process and return all three outputs at once.
    struct Outputs {
        SampleType lowpass, highpass, allpass;
    };

    Outputs process(SampleType input) {
        const SampleType d = input - state_;
        const SampleType lp = g_ * d + state_;
        state_ = snap_to_zero(two_g_ * d + state_); // flush denormal tails (see above)
        SampleType hp = input - lp;
        SampleType ap = lp - hp; // = 2*lp - input
        return {lp, hp, ap};
    }

    /// Number of samples the buffer paths run between denormal snaps of the
    /// integrator state.
    static constexpr int kStateSnapInterval = 64;

    void reset() {
        state_ = SampleType{0.0f};
    }

  private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType cutoff_ = SampleType{1000.0f};
    SampleType g_ = SampleType{0.0f};
    SampleType two_g_ = SampleType{0.0f};
    SampleType state_ = SampleType{0.0f};

    void update_coefficient() {
        constexpr SampleType pi = SampleType{3.14159265358979323846};
        SampleType wd = SampleType{2.0f} * pi * cutoff_;
        SampleType wa =
            (SampleType{2.0f} * sample_rate_) * std::tan(wd / (SampleType{2.0f} * sample_rate_));
        g_ = wa / (SampleType{2.0f} * sample_rate_ + wa);
        two_g_ = SampleType{2.0f} * g_;
    }

    template <typename Output>
    void process_block(SampleType* buffer, int num_samples, Output output) {
        const SampleType g = g_;
        const SampleType two_g = two_g_;
        SampleType s = state_;
        for (int start = 0; start < num_samples; start += kStateSnapInterval) {
            const int end = num_samples - start < kStateSnapInterval
                ? num_samples : start + kStateSnapInterval;
            for (int i = start; i < end; ++i) {
                const SampleType input = buffer[i];
                const SampleType d = input - s;
                const SampleType lp = g * d + s;
                s = two_g * d + s;
                buffer[i] = output(input, lp);
            }
            s = snap_to_zero(s);
        }
        state_ = s;
    }
};

using TptFilter = TptFilterT<float>;
using TptFilter64 = TptFilterT<double>;

} // namespace pulp::signal
