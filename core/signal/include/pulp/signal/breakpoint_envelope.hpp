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
/// ## Sustain and release
///
/// `set_sustain_point(i)` marks point `i` as the sustain point. While the gate
/// is on (from `trigger()` or `trigger_legato()` until `release()`), the
/// envelope plays to point `i` and then holds that point's value indefinitely.
/// `release()` ends the hold and plays on through the remaining segments, so
/// the release starts exactly at the held level with the authored times and
/// curves of the segments after the sustain point. A sustain point on the last
/// point holds until release and then finishes. A sustain point of 0 holds the
/// first point's value as soon as the gate opens. With no sustain point the
/// envelope runs to its end as before, and `release()` only stops any loop.
///
/// `release()` before the sustain point is reached cancels the hold: the
/// envelope keeps following its authored segments with no jump and passes
/// through the sustain point without stopping.
///
/// ## Loop and sustain together
///
/// A loop repeats only while the gate is on; `release()` suppresses further
/// repeats, so the current pass finishes and the envelope continues past the
/// loop end into the following segments. The sustain hold is checked first when
/// a segment ends:
///  - sustain point after the loop end: the loop runs its repeats, then the
///    envelope holds. An endless loop never reaches the sustain point while the
///    gate is on; on release it plays out its pass and continues past it
///    without holding.
///  - sustain point at the loop end, or inside the loop: the hold wins the
///    first time that point is reached, so the loop does not repeat while the
///    gate is on. On release the envelope continues from the held point, and no
///    further repeats happen.
///  - sustain point before the loop start: the envelope holds there and the
///    loop is reached only after release, where (the gate being off) it does
///    not repeat.
/// An endless loop with no sustain point is therefore a "loop while held":
/// it repeats until `release()`, then plays out.
///
/// `trigger_legato()` starts a new note without a jump. If the envelope is
/// already gated (rising, looping or holding) it keeps going untouched; if it
/// is releasing or finished, the first segment is replayed from the current
/// value, over its authored duration, to point 1.
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
    invalid_sustain,
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
    static constexpr std::size_t kNoSustain = std::numeric_limits<std::size_t>::max();

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
        sustain_point_ = kNoSustain;
        rebuild_sample_positions_();
        reset();
        return BreakpointEnvelopeStatus::ok;
    }

    /// Holds the envelope at point `index` while the gate is on; see the file
    /// comment. If the envelope is holding at another point, the hold ends and
    /// playback resumes.
    [[nodiscard]] BreakpointEnvelopeStatus set_sustain_point(std::size_t index) noexcept {
        if (index >= point_count_)
            return BreakpointEnvelopeStatus::invalid_sustain;
        sustain_point_ = index;
        if (holding_ && current_segment_ != index)
            resume_after_hold_();
        return BreakpointEnvelopeStatus::ok;
    }

    /// Removes the sustain point. A hold in progress ends and playback resumes.
    void clear_sustain_point() noexcept {
        sustain_point_ = kNoSustain;
        if (holding_)
            resume_after_hold_();
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

    /// Opens the gate and starts at point zero.
    void trigger() noexcept {
        start_(false, SampleType{0});
    }

    /// Opens the gate without a jump. An envelope that is already gated is left
    /// running; one that is releasing or finished replays its first segment from
    /// the current value.
    void trigger_legato() noexcept {
        if (active_ && gate_)
            return;
        start_(true, value_);
    }

    /// Closes the gate: a sustain hold ends and playback continues through the
    /// segments after it; loops stop repeating after the current pass.
    void release() noexcept {
        gate_ = false;
        if (holding_) {
            resume_after_hold_();
        } else if (pending_loop_restart_) {
            pending_loop_restart_ = false;
            ++current_segment_;
            segment_position_ = 0;
            skip_zero_length_segments_();
        }
    }

    void reset() noexcept {
        active_ = false;
        gate_ = false;
        holding_ = false;
        legato_active_ = false;
        pending_loop_restart_ = false;
        current_segment_ = 0;
        segment_position_ = 0;
        loops_completed_ = 0;
        value_ = points_[0].value;
    }

    SampleType next() noexcept {
        if (!active_ || holding_)
            return value_;
        if (pending_loop_restart_) {
            restart_loop_();
            skip_zero_length_segments_();
        }
        if (!active_ || holding_)
            return value_;

        const std::uint64_t length = segment_length_samples_(current_segment_);
        ++segment_position_;
        const SampleType progress = length == 0 ? SampleType{1}
                                                : static_cast<SampleType>(segment_position_) /
                                                      static_cast<SampleType>(length);
        const SampleType from = legato_active_ && current_segment_ == 0
                                    ? legato_start_value_
                                    : points_[current_segment_].value;
        value_ = interpolate_modulation_curve(from, points_[current_segment_ + 1].value, progress,
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
    /// True while the envelope is parked at its sustain point.
    bool holding() const noexcept {
        return holding_;
    }
    /// True from `trigger()` / `trigger_legato()` until `release()`.
    bool gate() const noexcept {
        return gate_;
    }
    /// The sustain point, or kNoSustain.
    std::size_t sustain_point() const noexcept {
        return sustain_point_;
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
        return loop_enabled_ && gate_ && current_segment_ + 1 == loop_last_point_ &&
               (loop_repeat_count_ == kLoopForever || loops_completed_ < loop_repeat_count_);
    }

    void start_(bool legato, SampleType from) noexcept {
        active_ = true;
        gate_ = true;
        holding_ = false;
        legato_active_ = false;
        pending_loop_restart_ = false;
        current_segment_ = 0;
        segment_position_ = 0;
        loops_completed_ = 0;
        value_ = points_[0].value;
        if (sustain_point_ == 0) {
            holding_ = true;
            return;
        }
        skip_zero_length_segments_();
        if (legato && active_ && !holding_ && current_segment_ == 0) {
            legato_active_ = true;
            legato_start_value_ = from;
            value_ = from;
        }
    }

    void resume_after_hold_() noexcept {
        holding_ = false;
        skip_zero_length_segments_();
    }

    void finish_segment_() noexcept {
        if (gate_ && sustain_point_ == current_segment_ + 1) {
            // Arrived at the sustain point: park there. This is checked before
            // the loop so a hold at or inside a loop wins.
            ++current_segment_;
            segment_position_ = 0;
            holding_ = true;
            legato_active_ = false;
            return;
        }
        if (should_repeat_loop_()) {
            if (loops_completed_ != kLoopForever)
                ++loops_completed_;
            pending_loop_restart_ = true;
            return;
        }

        ++current_segment_;
        segment_position_ = 0;
        legato_active_ = false;
        if (current_segment_ + 1 >= point_count_) {
            active_ = false;
            current_segment_ = point_count_ - 1;
            return;
        }
    }

    void restart_loop_() noexcept {
        pending_loop_restart_ = false;
        legato_active_ = false;
        current_segment_ = loop_first_point_;
        segment_position_ = 0;
        value_ = points_[loop_first_point_].value;
    }

    void skip_zero_length_segments_() noexcept {
        for (std::size_t visited = 0; active_ && !holding_ && visited < MaxPoints; ++visited) {
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
        if (active_ && !holding_) {
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
    std::size_t sustain_point_ = kNoSustain;
    std::uint64_t segment_position_ = 0;
    std::uint32_t loop_repeat_count_ = 0;
    std::uint32_t loops_completed_ = 0;
    double sample_rate_ = kDefaultSampleRate;
    SampleType value_ = SampleType{0};
    SampleType legato_start_value_ = SampleType{0};
    bool active_ = false;
    bool gate_ = false;
    bool holding_ = false;
    bool legato_active_ = false;
    bool loop_enabled_ = false;
    bool pending_loop_restart_ = false;
};

using BreakpointEnvelope = BreakpointEnvelopeT<float>;
using BreakpointEnvelope64 = BreakpointEnvelopeT<double>;

} // namespace pulp::signal
