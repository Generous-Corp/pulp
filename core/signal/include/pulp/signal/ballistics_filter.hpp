#pragma once

/// @file ballistics_filter.hpp
/// Peak/RMS envelope follower with independent attack and release curves.

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <algorithm>

namespace pulp::signal {

/// Envelope follower with configurable attack and release times.
///
/// RT contract: `process()`, `current()`, and `reset()` allocate no memory.
/// `prepare()` and parameter setters recompute coefficients and should be run
/// outside the audio callback unless the caller owns the retiming point.
///
/// Tracks the envelope of an input signal using first-order IIR
/// smoothing with separate attack (rising) and release (falling)
/// time constants.
///
/// @code
/// BallisticsFilter env;
/// env.prepare(44100.0f);
/// env.set_attack_ms(1.0f);
/// env.set_release_ms(100.0f);
/// float envelope = env.process(std::abs(sample));
/// @endcode
template <typename SampleType = float>
class BallisticsFilterT {
public:
    enum class Mode { peak, rms };

    BallisticsFilterT() = default;

    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate;
        update_coefficients();
    }

    void set_attack_ms(SampleType ms) {
        attack_ms_ = std::max(SampleType{0.01f}, ms);
        update_coefficients();
    }

    void set_release_ms(SampleType ms) {
        release_ms_ = std::max(SampleType{0.01f}, ms);
        update_coefficients();
    }

    void set_mode(Mode m) { mode_ = m; }

    SampleType process(SampleType input) {
        SampleType value = (mode_ == Mode::rms) ? input * input : std::abs(input);
        SampleType coeff = (value > state_) ? attack_coeff_ : release_coeff_;
        // Snap the envelope state so a long release into silence flushes to
        // zero instead of stalling in the denormal range. No-op above 1e-15.
        state_ = snap_to_zero(state_ + coeff * (value - state_));
        return (mode_ == Mode::rms) ? std::sqrt(state_) : state_;
    }

    void process(const SampleType* input, SampleType* output, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            output[i] = process(input[i]);
        }
    }

    SampleType current() const {
        return (mode_ == Mode::rms) ? std::sqrt(state_) : state_;
    }

    void reset() { state_ = SampleType{0.0f}; }

private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType attack_ms_ = SampleType{1.0f};
    SampleType release_ms_ = SampleType{100.0f};
    SampleType attack_coeff_ = SampleType{0.0f};
    SampleType release_coeff_ = SampleType{0.0f};
    SampleType state_ = SampleType{0.0f};
    Mode mode_ = Mode::peak;

    void update_coefficients() {
        if (sample_rate_ <= 0) return;
        attack_coeff_ = time_constant(attack_ms_);
        release_coeff_ = time_constant(release_ms_);
    }

    SampleType time_constant(SampleType ms) const {
        if (ms <= SampleType{0.01f}) return SampleType{1.0f};
        return SampleType{1.0f} -
               std::exp(SampleType{-2.2f} / (ms * SampleType{0.001f} * sample_rate_));
    }
};

using BallisticsFilter = BallisticsFilterT<float>;
using BallisticsFilter64 = BallisticsFilterT<double>;

} // namespace pulp::signal
