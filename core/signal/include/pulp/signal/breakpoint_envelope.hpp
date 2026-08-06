#pragma once

/// @file breakpoint_envelope.hpp
/// Fixed-capacity, sample-accurate breakpoint modulation envelope.
///
/// A program is authored as absolute times in milliseconds and values in the
/// caller's real unit. Values are not normalized. The curve stored at point N
/// shapes the segment from N to N+1. Equal adjacent times are legal and become
/// bounded, instantaneous transitions.
///
/// `trigger()` places the source exactly at point zero. Each `next()` advances
/// one sample; a segment quantized to N samples reaches its endpoint on the Nth
/// call. The final value is held after completion. A loop wraps after emitting
/// its end point; if the loop endpoint and start point differ, the discontinuity
/// occurs before the following sample by explicit authored choice.
///
/// RT contract: the program and runtime state are fixed-capacity scalar/array
/// storage. `next()`, `process()`, `trigger()`, and `reset()` allocate nothing,
/// acquire no locks, and perform bounded work. Configuration and `prepare()`
/// are control-thread operations.

#include <pulp/signal/modulation_curve.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class BreakpointEnvelopeStatus : std::uint8_t {
    ok,
    too_few_points,
    too_many_points,
    invalid_time,
    times_not_ordered,
    invalid_value,
    invalid_sample_rate,
    invalid_loop,
};

template <typename SampleType = float> struct BreakpointEnvelopePointT {
    /// Absolute time since trigger, in milliseconds.
    double time_ms = 0.0;
    /// Value in the consumer's real unit; any finite SampleType is legal.
    SampleType value = SampleType{0};
    /// Shape from this point to the next point.
    ModulationCurve curve_to_next{};
};

using BreakpointEnvelopePoint = BreakpointEnvelopePointT<float>;
using BreakpointEnvelopePoint64 = BreakpointEnvelopePointT<double>;

template <typename SampleType = float, std::size_t MaxPoints = 16> class BreakpointEnvelopeT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxPoints >= 2);

  public:
    using Point = BreakpointEnvelopePointT<SampleType>;

    static constexpr double kDefaultSampleRate = 48000.0;
    static constexpr double kMinSampleRate = 1.0;
    static constexpr double kMaxSampleRate = 768000.0;
    static constexpr double kMaxProgramTimeMs = 86400000.0; // one day
    static constexpr std::uint32_t kLoopForever = std::numeric_limits<std::uint32_t>::max();

    BreakpointEnvelopeT() noexcept {
        points_[0] = {};
        points_[1] = {1.0, SampleType{1}, {}};
        point_count_ = 2;
        rebuild_sample_positions_();
        reset();
    }

    [[nodiscard]] BreakpointEnvelopeStatus prepare(double sample_rate) noexcept {
        if (!std::isfinite(sample_rate) || sample_rate < kMinSampleRate ||
            sample_rate > kMaxSampleRate)
            return BreakpointEnvelopeStatus::invalid_sample_rate;
        sample_rate_ = sample_rate;
        rebuild_sample_positions_();
        reset();
        return BreakpointEnvelopeStatus::ok;
    }

    /// Replaces the complete program transactionally. Failure leaves the
    /// previous program and playback position unchanged.
    [[nodiscard]] BreakpointEnvelopeStatus configure(std::span<const Point> points) noexcept {
        if (points.size() < 2)
            return BreakpointEnvelopeStatus::too_few_points;
        if (points.size() > MaxPoints)
            return BreakpointEnvelopeStatus::too_many_points;

        double previous_time = 0.0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& point = points[i];
            if (!std::isfinite(point.time_ms) || point.time_ms < 0.0 ||
                point.time_ms > kMaxProgramTimeMs)
                return BreakpointEnvelopeStatus::invalid_time;
            if (i == 0 && point.time_ms != 0.0)
                return BreakpointEnvelopeStatus::invalid_time;
            if (i > 0 && point.time_ms < previous_time)
                return BreakpointEnvelopeStatus::times_not_ordered;
            if (!std::isfinite(static_cast<double>(point.value)))
                return BreakpointEnvelopeStatus::invalid_value;
            previous_time = point.time_ms;
        }

        std::array<Point, MaxPoints> replacement{};
        for (std::size_t i = 0; i < points.size(); ++i) {
            replacement[i] = points[i];
            replacement[i].curve_to_next = sanitize_modulation_curve(points[i].curve_to_next);
        }
        points_ = replacement;
        point_count_ = points.size();
        loop_enabled_ = false;
        loop_repeat_count_ = 0;
        rebuild_sample_positions_();
        reset();
        return BreakpointEnvelopeStatus::ok;
    }

    /// Repeats the range from `first_point` through `last_point`. A repeat
    /// count of zero disables looping; `kLoopForever` loops indefinitely.
    [[nodiscard]] BreakpointEnvelopeStatus set_loop(std::size_t first_point, std::size_t last_point,
                                                    std::uint32_t repeat_count) noexcept {
        if (repeat_count == 0) {
            loop_enabled_ = false;
            loop_repeat_count_ = 0;
            return BreakpointEnvelopeStatus::ok;
        }
        if (first_point >= last_point || last_point >= point_count_)
            return BreakpointEnvelopeStatus::invalid_loop;
        loop_first_point_ = first_point;
        loop_last_point_ = last_point;
        loop_repeat_count_ = repeat_count;
        loop_enabled_ = true;
        return BreakpointEnvelopeStatus::ok;
    }

    void clear_loop() noexcept {
        loop_enabled_ = false;
        loop_repeat_count_ = 0;
    }

    void trigger() noexcept {
        active_ = true;
        pending_loop_restart_ = false;
        current_segment_ = 0;
        segment_position_ = 0;
        loops_completed_ = 0;
        value_ = points_[0].value;
        skip_zero_length_segments_();
    }

    void reset() noexcept {
        active_ = false;
        pending_loop_restart_ = false;
        current_segment_ = 0;
        segment_position_ = 0;
        loops_completed_ = 0;
        value_ = points_[0].value;
    }

    SampleType next() noexcept {
        if (!active_)
            return value_;
        if (pending_loop_restart_) {
            restart_loop_();
            skip_zero_length_segments_();
        }
        if (!active_)
            return value_;

        const std::uint64_t length = segment_length_samples_(current_segment_);
        ++segment_position_;
        const SampleType progress = length == 0 ? SampleType{1}
                                                : static_cast<SampleType>(segment_position_) /
                                                      static_cast<SampleType>(length);
        value_ = interpolate_modulation_curve(points_[current_segment_].value,
                                              points_[current_segment_ + 1].value, progress,
                                              points_[current_segment_].curve_to_next);
        if (segment_position_ >= length) {
            finish_segment_();
            if (active_ && !pending_loop_restart_)
                skip_zero_length_segments_();
        }
        return value_;
    }

    void process(std::span<SampleType> output) noexcept {
        for (auto& sample : output)
            sample = next();
    }

    SampleType current() const noexcept {
        return value_;
    }
    bool active() const noexcept {
        return active_;
    }
    std::size_t point_count() const noexcept {
        return point_count_;
    }
    std::size_t current_segment() const noexcept {
        return current_segment_;
    }
    std::uint64_t segment_position_samples() const noexcept {
        return segment_position_;
    }
    std::uint32_t loops_completed() const noexcept {
        return loops_completed_;
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }
    std::span<const Point> points() const noexcept {
        return {points_.data(), point_count_};
    }

  private:
    std::uint64_t time_to_samples_(double time_ms) const noexcept {
        const long double samples =
            static_cast<long double>(time_ms) * static_cast<long double>(sample_rate_) / 1000.0L;
        return static_cast<std::uint64_t>(std::llround(samples));
    }

    void rebuild_sample_positions_() noexcept {
        for (std::size_t i = 0; i < point_count_; ++i)
            point_samples_[i] = time_to_samples_(points_[i].time_ms);
    }

    std::uint64_t segment_length_samples_(std::size_t segment) const noexcept {
        return point_samples_[segment + 1] - point_samples_[segment];
    }

    bool should_repeat_loop_() const noexcept {
        return loop_enabled_ && current_segment_ + 1 == loop_last_point_ &&
               (loop_repeat_count_ == kLoopForever || loops_completed_ < loop_repeat_count_);
    }

    void finish_segment_() noexcept {
        if (should_repeat_loop_()) {
            if (loops_completed_ != kLoopForever)
                ++loops_completed_;
            pending_loop_restart_ = true;
            return;
        }

        ++current_segment_;
        segment_position_ = 0;
        if (current_segment_ + 1 >= point_count_) {
            active_ = false;
            current_segment_ = point_count_ - 1;
            return;
        }
    }

    void restart_loop_() noexcept {
        pending_loop_restart_ = false;
        current_segment_ = loop_first_point_;
        segment_position_ = 0;
        value_ = points_[loop_first_point_].value;
    }

    void skip_zero_length_segments_() noexcept {
        for (std::size_t visited = 0; active_ && visited < MaxPoints; ++visited) {
            if (current_segment_ + 1 >= point_count_) {
                active_ = false;
                current_segment_ = point_count_ - 1;
                value_ = points_[point_count_ - 1].value;
                return;
            }
            if (segment_length_samples_(current_segment_) != 0)
                return;
            value_ = points_[current_segment_ + 1].value;
            finish_segment_();
            if (pending_loop_restart_)
                restart_loop_();
        }
        if (active_) {
            // A zero-time infinite loop has no observable duration. Quiesce
            // rather than spinning or consuming unbounded callback work.
            active_ = false;
            pending_loop_restart_ = false;
        }
    }

    std::array<Point, MaxPoints> points_{};
    std::array<std::uint64_t, MaxPoints> point_samples_{};
    std::size_t point_count_ = 0;
    std::size_t current_segment_ = 0;
    std::size_t loop_first_point_ = 0;
    std::size_t loop_last_point_ = 1;
    std::uint64_t segment_position_ = 0;
    std::uint32_t loop_repeat_count_ = 0;
    std::uint32_t loops_completed_ = 0;
    double sample_rate_ = kDefaultSampleRate;
    SampleType value_ = SampleType{0};
    bool active_ = false;
    bool loop_enabled_ = false;
    bool pending_loop_restart_ = false;
};

using BreakpointEnvelope = BreakpointEnvelopeT<float>;
using BreakpointEnvelope64 = BreakpointEnvelopeT<double>;

} // namespace pulp::signal
