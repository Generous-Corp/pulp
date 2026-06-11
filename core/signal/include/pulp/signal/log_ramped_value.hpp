#pragma once

/// @file log_ramped_value.hpp
/// Exponential (logarithmic) parameter smoothing for pitch and frequency.

#include <cmath>
#include <algorithm>

namespace pulp::signal {

/// Exponentially-smoothed value for parameters where linear ramping
/// sounds wrong (pitch, frequency, gain in dB).
///
/// Unlike SmoothedValue (linear ramp), LogRampedValue uses a
/// multiplicative approach so that equal time produces equal
/// perceptual change across the range.
///
/// RT contract: setters, `next()`, `skip()`, and query methods are scalar-only
/// and allocate no memory.
///
/// @code
/// LogRampedValue freq(440.0f);
/// freq.set_ramp_time(0.05f, 44100.0f);
/// freq.set_target(880.0f); // smooth glide up one octave
/// for (int i = 0; i < block_size; ++i) osc.set_freq(freq.next());
/// @endcode
class LogRampedValue {
public:
    LogRampedValue() = default;
    explicit LogRampedValue(float initial) : current_(initial), target_(initial) {}

    void set_ramp_time(float seconds, float sample_rate) {
        ramp_samples_ = std::max(1, static_cast<int>(seconds * sample_rate));
    }

    void set_target(float value) {
        target_ = value;
        if (ramp_samples_ <= 1 || current_ <= 0.0f || target_ <= 0.0f) {
            current_ = target_;
            steps_remaining_ = 0;
            multiplier_ = 1.0f;
        } else {
            // Compute per-sample multiplier: current * multiplier^N = target
            multiplier_ = std::pow(target_ / current_,
                                    1.0f / static_cast<float>(ramp_samples_));
            steps_remaining_ = ramp_samples_;
        }
    }

    void set_immediate(float value) {
        current_ = value;
        target_ = value;
        steps_remaining_ = 0;
        multiplier_ = 1.0f;
    }

    float next() {
        if (steps_remaining_ > 0) {
            current_ *= multiplier_;
            --steps_remaining_;
            if (steps_remaining_ == 0) current_ = target_;
        }
        return current_;
    }

    void skip(int n) {
        if (n <= 0) return;
        if (steps_remaining_ <= 0) return;
        if (n >= steps_remaining_) {
            current_ = target_;
            steps_remaining_ = 0;
        } else {
            current_ *= std::pow(multiplier_, static_cast<float>(n));
            steps_remaining_ -= n;
        }
    }

    float current_value() const { return current_; }
    float target_value() const { return target_; }
    bool is_smoothing() const { return steps_remaining_ > 0; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float multiplier_ = 1.0f;
    int ramp_samples_ = 1;
    int steps_remaining_ = 0;
};

} // namespace pulp::signal
