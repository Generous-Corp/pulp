#pragma once

/// @file tempo_delay.hpp
/// Musical-division conversion and fixed-state delay-time transitions.

#include <pulp/timebase/beat_division.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::signal {

enum class TempoDelayError : std::uint8_t {
    none,
    not_prepared,
    invalid_division,
    invalid_tempo,
    invalid_sample_rate,
    invalid_delay,
    out_of_range,
};

struct TempoDelayResult {
    TempoDelayError error = TempoDelayError::none;
    double samples = 0.0;

    constexpr explicit operator bool() const noexcept {
        return error == TempoDelayError::none;
    }
};

/// These bounds intentionally match `TempoMap` / `CompiledTempoMap`: a delay
/// synchronized to a transport must accept exactly the tempo and sample-rate
/// domain that the transport can compile.
inline constexpr double kMinimumTempoDelayBpm = timebase::kMinimumCompiledTempoBpm;
inline constexpr double kMaximumTempoDelayBpm = timebase::kMaximumCompiledTempoBpm;
inline constexpr double kMaximumTempoDelaySampleRate =
    static_cast<double>(timebase::kMaximumCompiledSampleRate);

/// Convert an exact persisted musical division to a fractional delay length.
///
/// A beat is one quarter note regardless of meter. Straight, dotted, and
/// triplet values come from `timebase::beat_fraction()` as reduced integer
/// rationals. The returned fractional sample count is not rounded; the delay
/// interpolation owner chooses its own sample-addressing policy.
[[nodiscard]] inline TempoDelayResult tempo_delay_samples(timebase::BeatDivision division,
                                                          double beats_per_minute,
                                                          double sample_rate) noexcept {
    if (!std::isfinite(beats_per_minute) || beats_per_minute < kMinimumTempoDelayBpm ||
        beats_per_minute > kMaximumTempoDelayBpm)
        return {TempoDelayError::invalid_tempo, 0.0};
    if (!std::isfinite(sample_rate) || !(sample_rate > 0.0) ||
        sample_rate > kMaximumTempoDelaySampleRate)
        return {TempoDelayError::invalid_sample_rate, 0.0};

    const auto fraction = timebase::beat_fraction(division);
    if (!fraction)
        return {TempoDelayError::invalid_division, 0.0};
    const auto value = fraction.value();
    if (value.numerator <= 0 || value.denominator <= 0)
        return {TempoDelayError::invalid_division, 0.0};

    const long double samples =
        static_cast<long double>(value.numerator) * 60.0L * static_cast<long double>(sample_rate) /
        (static_cast<long double>(value.denominator) * static_cast<long double>(beats_per_minute));
    constexpr long double maximum = static_cast<long double>(std::numeric_limits<int>::max() - 1);
    if (!std::isfinite(samples) || samples < 1.0L || samples > maximum)
        return {TempoDelayError::out_of_range, 0.0};
    return {TempoDelayError::none, static_cast<double>(samples)};
}

/// Fixed-state controller for deterministic delay read-time modulation.
///
/// The first valid setting after `prepare()` is adopted immediately. Later
/// settings linearly move the published fractional delay over exactly
/// `transition_samples`: the first `next()` returns 1/N of the move and the
/// Nth returns the exact target. Retargeting starts from the current published
/// value. `reset()` collapses an active transition to its target without
/// changing configuration, which makes transport restarts deterministic.
///
/// This controller owns no audio history, feedback, routing, or tempo map.
/// Consumers pass `next()` to their existing fractional-delay/read-head API.
/// This is a linear read-time glide, so large moves can produce a pitch sweep;
/// an audio engine that requires a click-free jump owns the dual-read-head
/// crossfade rather than this control-only helper.
/// Consequently it introduces zero audio latency and zero audio tail.
/// Prepared setters, stepping, block rendering, reset, and inspection allocate
/// no memory, lock nothing, perform no I/O, and throw no exceptions.
class TempoDelayTime {
  public:
    static constexpr std::uint32_t kDefaultTransitionSamples = 64;

    [[nodiscard]] TempoDelayError
    prepare(double sample_rate, double maximum_delay_samples,
            std::uint32_t transition_samples = kDefaultTransitionSamples) noexcept {
        if (!std::isfinite(sample_rate) || !(sample_rate > 0.0) ||
            sample_rate > kMaximumTempoDelaySampleRate)
            return TempoDelayError::invalid_sample_rate;
        constexpr double maximum = static_cast<double>(std::numeric_limits<int>::max() - 1);
        if (!std::isfinite(maximum_delay_samples) || maximum_delay_samples < 1.0)
            return TempoDelayError::invalid_delay;
        if (maximum_delay_samples > maximum)
            return TempoDelayError::out_of_range;

        sample_rate_ = sample_rate;
        maximum_delay_samples_ = maximum_delay_samples;
        transition_samples_ = transition_samples;
        current_ = 1.0;
        start_ = 1.0;
        target_ = 1.0;
        remaining_ = 0;
        initialized_ = false;
        prepared_ = true;
        return TempoDelayError::none;
    }

    /// Adopt a direct fractional-sample target. Rejected updates preserve the
    /// complete current transition.
    [[nodiscard]] TempoDelayError set_delay_samples(double samples) noexcept {
        if (!prepared_)
            return TempoDelayError::not_prepared;
        if (!std::isfinite(samples))
            return TempoDelayError::invalid_delay;
        if (samples < 1.0 || samples > maximum_delay_samples_)
            return TempoDelayError::out_of_range;
        set_valid_target(samples);
        return TempoDelayError::none;
    }

    /// Convert and adopt a musical target transactionally. Conversion or
    /// capacity failure leaves the current target and transition untouched.
    [[nodiscard]] TempoDelayError set_tempo(timebase::BeatDivision division,
                                            double beats_per_minute) noexcept {
        if (!prepared_)
            return TempoDelayError::not_prepared;
        const auto result = tempo_delay_samples(division, beats_per_minute, sample_rate_);
        if (!result)
            return result.error;
        return set_delay_samples(result.samples);
    }

    [[nodiscard]] double next() noexcept {
        if (!prepared_)
            return 0.0;
        if (remaining_ == 0)
            return current_;
        const std::uint32_t elapsed = transition_samples_ - remaining_ + 1U;
        const double amount =
            static_cast<double>(elapsed) / static_cast<double>(transition_samples_);
        current_ = start_ + (target_ - start_) * amount;
        --remaining_;
        if (remaining_ == 0) {
            current_ = target_;
            start_ = target_;
        }
        return current_;
    }

    void render(double* output, std::size_t sample_count) noexcept {
        if (output == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i)
            output[i] = next();
    }

    void reset() noexcept {
        if (!prepared_ || !initialized_)
            return;
        current_ = target_;
        start_ = target_;
        remaining_ = 0;
    }

    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    [[nodiscard]] bool transition_active() const noexcept {
        return remaining_ != 0;
    }
    [[nodiscard]] std::uint32_t remaining_transition_samples() const noexcept {
        return remaining_;
    }
    [[nodiscard]] double sample_rate() const noexcept {
        return sample_rate_;
    }
    [[nodiscard]] double maximum_delay_samples() const noexcept {
        return maximum_delay_samples_;
    }
    [[nodiscard]] double current_delay_samples() const noexcept {
        return prepared_ ? current_ : 0.0;
    }
    [[nodiscard]] double target_delay_samples() const noexcept {
        return prepared_ ? target_ : 0.0;
    }
    [[nodiscard]] double conservative_delay_samples() const noexcept {
        return prepared_ ? std::max({start_, current_, target_}) : 0.0;
    }
    [[nodiscard]] static constexpr int latency_samples() noexcept {
        return 0;
    }
    [[nodiscard]] static constexpr int tail_samples() noexcept {
        return 0;
    }

  private:
    void set_valid_target(double samples) noexcept {
        if (!initialized_) {
            initialized_ = true;
            current_ = samples;
            start_ = samples;
            target_ = samples;
            remaining_ = 0;
            return;
        }
        if (samples == target_)
            return;
        if (samples == current_) {
            start_ = samples;
            target_ = samples;
            remaining_ = 0;
            return;
        }
        start_ = current_;
        target_ = samples;
        remaining_ = transition_samples_;
        if (remaining_ == 0) {
            current_ = target_;
            start_ = target_;
        }
    }

    double sample_rate_ = 0.0;
    double maximum_delay_samples_ = 0.0;
    double start_ = 0.0;
    double current_ = 0.0;
    double target_ = 0.0;
    std::uint32_t transition_samples_ = kDefaultTransitionSamples;
    std::uint32_t remaining_ = 0;
    bool prepared_ = false;
    bool initialized_ = false;
};

} // namespace pulp::signal
