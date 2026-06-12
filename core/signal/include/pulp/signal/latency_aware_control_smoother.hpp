#pragma once

/// @file latency_aware_control_smoother.hpp
/// Sample-accurate, block-size-independent control smoothing with
/// independent attack/release time constants and value domains.
///
/// Complements `SmoothedValue` (fixed-length linear ramp): this smoother
/// is a one-pole exponential with closed-form evaluation, so a processor
/// can read the control value at any sample offset inside a block
/// (`value_at`) — e.g. at spectral frame centers — and advance state in
/// one call per block (`advance`). Closed-form advancement makes the
/// trajectory exactly independent of how the stream is chopped into
/// blocks.
///
/// The "latency-aware" part: `value_at` accepts negative offsets, so a
/// caller can evaluate the control trajectory `latency_samples` behind
/// the current position and keep a UI-visible value aligned with the
/// audibly delayed output.
///
/// One-pole smoothing per J. O. Smith III, *Introduction to Digital
/// Filters* (W3K); time constants are caller-chosen ramp times, never
/// transcribed from any reference product.

#include <cassert>
#include <cmath>

namespace pulp::signal {

class LatencyAwareControlSmoother {
public:
    /// Value domain. `semitone` smooths in semitones and additionally
    /// exposes the smoothed value as a frequency ratio via `ratio_at()`.
    enum class Domain { linear, semitone };

    struct Config {
        Domain domain = Domain::linear;
        float attack_seconds = 0.05f;   // toward larger values
        float release_seconds = 0.05f;  // toward smaller values
    };

    /// RT contract: fixed-state and allocation-free. prepare() does not
    /// allocate, but should be called from prepare/control code. set_target(),
    /// set_immediate(), value_at(), ratio_at(), advance(), and accessors are
    /// audio-thread safe.
    void prepare(double sample_rate, const Config& config) {
        assert(sample_rate > 0.0);
        config_ = config;
        // Per-sample retention coefficients; tau == 0 snaps immediately.
        attack_k_ = coefficient(config.attack_seconds, sample_rate);
        release_k_ = coefficient(config.release_seconds, sample_rate);
        current_ = 0.0;
        target_ = 0.0;
    }

    /// RT-safe; takes effect from the current stream position.
    void set_target(float value) { target_ = static_cast<double>(value); }

    /// Jump immediately (e.g. on reset/preset load).
    void set_immediate(float value) {
        target_ = static_cast<double>(value);
        current_ = target_;
    }

    float target() const { return static_cast<float>(target_); }
    float current() const { return static_cast<float>(current_); }

    /// Value `offset` samples ahead of (or, if negative, behind) the
    /// current position, without advancing state. Negative offsets
    /// rewind along the same exponential toward where the value was.
    float value_at(int offset) const {
        return static_cast<float>(project(offset));
    }

    /// `value_at` in the ratio domain: 2^(semitones / 12). Only
    /// meaningful for Domain::semitone.
    float ratio_at(int offset) const {
        return std::exp2(static_cast<float>(project(offset)) / 12.0f);
    }

    /// Advance the stream position by `samples` and return the new value.
    float advance(int samples) {
        current_ = project(samples);
        return static_cast<float>(current_);
    }

    bool is_settled(float tolerance) const {
        return std::abs(current_ - target_) <= static_cast<double>(tolerance);
    }

private:
    static double coefficient(float seconds, double sample_rate) {
        if (seconds <= 0.0f) return 0.0;
        // Standard one-pole: value covers ~63% of the step per tau.
        return std::exp(-1.0 / (static_cast<double>(seconds) * sample_rate));
    }

    double project(int offset) const {
        const double k = target_ > current_ ? attack_k_ : release_k_;
        if (k <= 0.0) return offset >= 0 ? target_ : current_;
        return target_ + (current_ - target_) * std::pow(k, static_cast<double>(offset));
    }

    Config config_;
    double attack_k_ = 0.0;
    double release_k_ = 0.0;
    double current_ = 0.0;
    double target_ = 0.0;
};

} // namespace pulp::signal
