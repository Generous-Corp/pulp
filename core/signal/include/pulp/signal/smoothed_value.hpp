#pragma once

#include <cmath>
#include <algorithm>

namespace pulp::signal {

/// Linear smoothing for control-rate parameters.
///
/// Real-time safe; no allocations. Use for parameter smoothing where the
/// linear ramp is perceptually correct (gain in linear units, mix, pan
/// position, etc.).
///
/// For parameters where the linear ramp is perceptually wrong — pitch,
/// frequency, gain in dB — use `LogRampedValue` (in
/// `core/signal/include/pulp/signal/log_ramped_value.hpp`) which applies
/// a multiplicative (geometric) per-sample step so equal time produces
/// equal perceptual change across the range.
///
/// The two utilities together cover Pulp's smoothing surface; consumers
/// pick the curve that matches the parameter's semantics.
template<typename T = float>
class SmoothedValue {
public:
    SmoothedValue() = default;
    explicit SmoothedValue(T initial) : current_(initial), target_(initial) {}

    // Set the smoothing time in seconds and sample rate
    void set_ramp_time(T seconds, T sample_rate) {
        ramp_samples_ = static_cast<int>(seconds * sample_rate);
        if (ramp_samples_ < 1) ramp_samples_ = 1;
    }

    // Set the target value (begins ramping)
    void set_target(T value) {
        target_ = value;
        if (ramp_samples_ <= 1) {
            current_ = value;
            steps_remaining_ = 0;
            increment_ = T(0);
        } else {
            increment_ = (target_ - current_) / static_cast<T>(ramp_samples_);
            steps_remaining_ = ramp_samples_;
        }
    }

    // Jump immediately to value (no smoothing)
    void set_immediate(T value) {
        current_ = value;
        target_ = value;
        steps_remaining_ = 0;
        increment_ = T(0);
    }

    // Get the next smoothed sample
    T next() {
        if (steps_remaining_ > 0) {
            current_ += increment_;
            --steps_remaining_;
            if (steps_remaining_ == 0) current_ = target_;
        }
        return current_;
    }

    // Skip N samples of smoothing
    void skip(int n) {
        if (n <= 0) return;
        if (steps_remaining_ <= 0) return;
        if (n >= steps_remaining_) {
            current_ = target_;
            steps_remaining_ = 0;
        } else {
            current_ += increment_ * static_cast<T>(n);
            steps_remaining_ -= n;
        }
    }

    // Is smoothing still active?
    bool is_smoothing() const { return steps_remaining_ > 0; }

    // Current value (without advancing)
    T current() const { return current_; }
    T target() const { return target_; }

private:
    T current_ = T(0);
    T target_ = T(0);
    T increment_ = T(0);
    int ramp_samples_ = 1;
    int steps_remaining_ = 0;
};

} // namespace pulp::signal
