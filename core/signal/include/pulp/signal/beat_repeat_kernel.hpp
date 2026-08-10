#pragma once

/// @file beat_repeat_kernel.hpp
/// Quantized capture and bounded repeat gestures over FreezeLoopSamplerT.
///
/// Capture timing is lowered from CompiledTempoMap and the canonical
/// BeatDivision tick lattice. The kernel therefore has no independent BPM or
/// floating-point transport clock. It records dry input only, captures an
/// exact recent interval without clamping, and layers finite/infinite repeat,
/// gate, reverse, seek, retrigger, and click-safe transitions over the reused
/// sampler storage.
///
/// RT contract: prepare(), snapshot(), and restore() may allocate. After a
/// successful prepare(), trigger(), stop(), seek(), reset(),
/// transport_discontinuity(), parameter setters, and process() allocate
/// nothing and take no locks.

#include <pulp/signal/freeze_loop_sampler.hpp>
#include <pulp/signal/transition_mixer.hpp>
#include <pulp/timebase/beat_division.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pulp::signal {

enum class BeatRepeatPlanError {
    None,
    InvalidDivision,
    TickRangeExceeded,
    SampleRangeExceeded,
    CaptureRangeExceeded,
    HistoryCapacityExceeded,
    Unprepared,
    SampleRateMismatch,
    ReleaseInProgress,
    StaleRequest,
};

struct BeatRepeatCapturePlan {
    timebase::SamplePosition requested_sample{};
    timebase::SamplePosition edge_sample{};
    timebase::TickPosition edge_tick{};
    int capture_frames = 0;
    constexpr auto operator<=>(const BeatRepeatCapturePlan&) const = default;
};

struct BeatRepeatPlanResult {
    BeatRepeatCapturePlan plan{};
    BeatRepeatPlanError error = BeatRepeatPlanError::None;
    constexpr explicit operator bool() const noexcept {
        return error == BeatRepeatPlanError::None;
    }
};

namespace detail {

inline bool beat_repeat_checked_add(std::int64_t lhs, std::int64_t rhs,
                                    std::int64_t& result) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((rhs > 0 && lhs > maximum - rhs) || (rhs < 0 && lhs < minimum - rhs))
        return false;
    result = lhs + rhs;
    return true;
}

inline bool beat_repeat_checked_subtract(std::int64_t lhs, std::int64_t rhs,
                                         std::int64_t& result) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((rhs > 0 && lhs < minimum + rhs) || (rhs < 0 && lhs > maximum + rhs))
        return false;
    result = lhs - rhs;
    return true;
}

inline bool beat_repeat_ceil_tick(std::int64_t value, std::int64_t quantum,
                                  std::int64_t& result) noexcept {
    if (quantum <= 0)
        return false;
    const auto remainder = value % quantum;
    if (remainder == 0) {
        result = value;
        return true;
    }
    const auto delta = remainder < 0 ? -remainder : quantum - remainder;
    return beat_repeat_checked_add(value, delta, result);
}

} // namespace detail

/// Lower the first grid edge strictly after `requested_sample` and the exact
/// preceding grid interval into integer host samples. Tempo ramps and segment
/// anchors are resolved by CompiledTempoMap itself.
inline BeatRepeatPlanResult
lower_beat_repeat_capture(const timebase::CompiledTempoMap& tempo, timebase::BeatDivision division,
                          timebase::SamplePosition requested_sample) noexcept {
    const auto quantum_result = timebase::division_ticks(division);
    if (!quantum_result)
        return {{}, BeatRepeatPlanError::InvalidDivision};
    const auto quantum = quantum_result.value().value;
    auto candidate_source = tempo.samples_to_ticks(requested_sample).value;
    std::int64_t candidate = 0;
    if (!detail::beat_repeat_ceil_tick(candidate_source, quantum, candidate))
        return {{}, BeatRepeatPlanError::TickRangeExceeded};

    auto edge = tempo.ticks_to_samples({candidate});
    if (edge.value == std::numeric_limits<std::int64_t>::min() ||
        edge.value == std::numeric_limits<std::int64_t>::max())
        return {{}, BeatRepeatPlanError::SampleRangeExceeded};
    if (edge.value <= requested_sample.value) {
        const auto candidate_bits = static_cast<std::uint64_t>(candidate);
        const auto maximum_bits =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto maximum_steps =
            (maximum_bits - candidate_bits) / static_cast<std::uint64_t>(quantum);
        if (maximum_steps == 0)
            return {{}, BeatRepeatPlanError::TickRangeExceeded};
        const auto tick_at_step = [&](std::uint64_t step) noexcept {
            const auto offset = step * static_cast<std::uint64_t>(quantum);
            return static_cast<std::int64_t>(candidate_bits + offset);
        };

        std::uint64_t lower_step = 0;
        std::uint64_t upper_step = 1;
        for (;;) {
            upper_step = std::min(upper_step, maximum_steps);
            const auto probe = tick_at_step(upper_step);
            edge = tempo.ticks_to_samples({probe});
            if (edge.value > requested_sample.value)
                break;
            if (upper_step == maximum_steps)
                return {{}, BeatRepeatPlanError::SampleRangeExceeded};
            lower_step = upper_step;
            upper_step = upper_step > maximum_steps / 2 ? maximum_steps : upper_step * 2;
        }
        while (upper_step - lower_step > 1) {
            const auto middle_step = lower_step + (upper_step - lower_step) / 2;
            const auto middle = tick_at_step(middle_step);
            if (tempo.ticks_to_samples({middle}).value <= requested_sample.value)
                lower_step = middle_step;
            else
                upper_step = middle_step;
        }
        candidate = tick_at_step(upper_step);
        edge = tempo.ticks_to_samples({candidate});
    }

    if (edge.value == std::numeric_limits<std::int64_t>::min() ||
        edge.value == std::numeric_limits<std::int64_t>::max())
        return {{}, BeatRepeatPlanError::SampleRangeExceeded};

    std::int64_t previous_tick = 0;
    if (!detail::beat_repeat_checked_subtract(candidate, quantum, previous_tick))
        return {{}, BeatRepeatPlanError::TickRangeExceeded};
    const auto previous_sample = tempo.ticks_to_samples({previous_tick});
    if (previous_sample.value == std::numeric_limits<std::int64_t>::min() ||
        previous_sample.value == std::numeric_limits<std::int64_t>::max())
        return {{}, BeatRepeatPlanError::SampleRangeExceeded};
    std::int64_t capture_frames = 0;
    if (!detail::beat_repeat_checked_subtract(edge.value, previous_sample.value, capture_frames) ||
        capture_frames <= 0 || capture_frames > std::numeric_limits<int>::max())
        return {{}, BeatRepeatPlanError::CaptureRangeExceeded};
    return {{requested_sample, edge, {candidate}, static_cast<int>(capture_frames)},
            BeatRepeatPlanError::None};
}

template <typename SampleType = float> class BeatRepeatKernelT {
  public:
    enum class ReverseMode {
        Off,
        On,
        Alternate,
    };

    enum class Status {
        Idle,
        Armed,
        CaptureRejectedInsufficientHistory,
        Active,
        Releasing,
    };

    enum class TransitionKind {
        None,
        DryToWet,
        HeldToWet,
        HeldToDry,
    };

    struct Config {
        int channels = 2;
        int history_capacity_frames = 192'000;
        int transition_frames = 144;
        timebase::RationalRate sample_rate{48'000, 1};
    };

    struct Snapshot {
        bool resumable = true;
        timebase::RationalRate sample_rate{48'000, 1};
        int repeat_count = 4;
        SampleType gate = SampleType{1};
        ReverseMode reverse = ReverseMode::Off;
        int loop_position = 0;
        int completed_repeats = 0;
        bool alternate_repeat = false;
        bool active = false;
        bool last_capture_rejected = false;
        TransitionKind transition = TransitionKind::None;
        int transition_frames = 0;
        int transition_position = 0;
        int transition_length = 0;
        std::vector<SampleType> sampler;
        std::vector<SampleType> last_output;
        std::vector<SampleType> transition_old;
    };

    BeatRepeatKernelT() = default;
    BeatRepeatKernelT(const BeatRepeatKernelT&) = delete;
    BeatRepeatKernelT& operator=(const BeatRepeatKernelT&) = delete;
    BeatRepeatKernelT(BeatRepeatKernelT&& other) noexcept {
        swap(other);
    }
    BeatRepeatKernelT& operator=(BeatRepeatKernelT&& other) noexcept {
        if (this != &other) {
            BeatRepeatKernelT replacement(std::move(other));
            swap(replacement);
        }
        return *this;
    }

    bool prepare(const Config& config) {
        if (config.channels <= 0 || config.history_capacity_frames <= 0 ||
            config.transition_frames < 0 || !config.sample_rate.valid())
            return false;
        const auto normalized_rate = config.sample_rate.normalized();

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            FreezeLoopSamplerT<SampleType> replacement_sampler;
            replacement_sampler.prepare(config.channels, config.history_capacity_frames, 0);
            if (replacement_sampler.channels() != config.channels ||
                replacement_sampler.capacity() != config.history_capacity_frames)
                return false;
            std::vector<const SampleType*> replacement_inputs(
                static_cast<std::size_t>(config.channels), nullptr);
            std::vector<SampleType> replacement_last(static_cast<std::size_t>(config.channels),
                                                     SampleType{});
            std::vector<SampleType> replacement_transition(
                static_cast<std::size_t>(config.channels), SampleType{});

            sampler_ = std::move(replacement_sampler);
            history_inputs_.swap(replacement_inputs);
            last_output_.swap(replacement_last);
            transition_old_.swap(replacement_transition);
            transition_frames_ = config.transition_frames;
            sample_rate_ = normalized_rate;
            prepared_ = true;
            reset();
            return true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    int channels() const noexcept {
        return sampler_.channels();
    }
    int history_capacity_frames() const noexcept {
        return sampler_.capacity();
    }
    int available_history_frames() const noexcept {
        return sampler_.available_history();
    }
    Status status() const noexcept {
        return status_;
    }
    bool last_capture_rejected() const noexcept {
        return last_capture_rejected_;
    }
    bool can_snapshot() const noexcept {
        return !pending_capture_;
    }
    int loop_length() const noexcept {
        return active_loop_ ? sampler_.loop_length() : 0;
    }
    int loop_position() const noexcept {
        return loop_position_;
    }
    int completed_repeats() const noexcept {
        return completed_repeats_;
    }
    int latency_samples() const noexcept {
        return 0;
    }
    int max_tail_samples() const noexcept {
        return active_transition_frames();
    }
    std::size_t retained_bytes() const noexcept {
        return sampler_.retained_bytes() + (history_inputs_.capacity() * sizeof(const SampleType*) +
                                            last_output_.capacity() * sizeof(SampleType) +
                                            transition_old_.capacity() * sizeof(SampleType));
    }

    bool set_repeat_count(int count) noexcept {
        if (count < 0)
            return false;
        repeat_count_ = count;
        if (active_loop_ && status_ != Status::Releasing && count > 0 &&
            completed_repeats_ >= count) {
            pending_capture_ = false;
            status_ = Status::Releasing;
            begin_held_transition(TransitionKind::HeldToDry);
        }
        return true;
    }

    bool set_gate(SampleType duty) noexcept {
        if (!std::isfinite(static_cast<double>(duty)))
            return false;
        gate_ = std::clamp(duty, SampleType{}, SampleType{1});
        return true;
    }

    bool set_reverse(ReverseMode mode) noexcept {
        if (mode != ReverseMode::Off && mode != ReverseMode::On && mode != ReverseMode::Alternate)
            return false;
        if (mode != reverse_ && active_loop_ && status_ != Status::Releasing)
            begin_held_transition(TransitionKind::HeldToWet);
        reverse_ = mode;
        return true;
    }

    bool set_transition_frames(int frames) noexcept {
        if (frames < 0)
            return false;
        if (frames != transition_frames_ && transition_kind_ != TransitionKind::None)
            return false;
        transition_frames_ = frames;
        return true;
    }

    BeatRepeatPlanResult trigger(const timebase::CompiledTempoMap& tempo,
                                 timebase::BeatDivision division,
                                 timebase::SamplePosition requested_sample) noexcept {
        if (!prepared_)
            return {{}, BeatRepeatPlanError::Unprepared};
        if (tempo.sample_rate().normalized() != sample_rate_)
            return {{}, BeatRepeatPlanError::SampleRateMismatch};
        if (status_ == Status::Releasing)
            return {{}, BeatRepeatPlanError::ReleaseInProgress};
        const auto lowered = lower_beat_repeat_capture(tempo, division, requested_sample);
        if (!lowered)
            return lowered;
        if (lowered.plan.capture_frames > sampler_.capacity())
            return {{}, BeatRepeatPlanError::HistoryCapacityExceeded};
        if (have_expected_sample_ && lowered.plan.edge_sample.value < expected_sample_)
            return {{}, BeatRepeatPlanError::StaleRequest};
        pending_plan_ = lowered.plan;
        pending_capture_ = true;
        last_capture_rejected_ = false;
        status_ = Status::Armed;
        return lowered;
    }

    void stop() noexcept {
        pending_capture_ = false;
        if (!active_loop_) {
            status_ = Status::Idle;
            transition_kind_ = TransitionKind::None;
            return;
        }
        if (transition_kind_ == TransitionKind::HeldToDry)
            return;
        status_ = Status::Releasing;
        begin_held_transition(TransitionKind::HeldToDry);
    }

    bool seek(int captured_frame) noexcept {
        if (!active_loop_ || status_ == Status::Releasing || sampler_.loop_length() <= 0)
            return false;
        const auto clamped = std::clamp(captured_frame, 0, sampler_.loop_length() - 1);
        loop_position_ =
            reverse_for_current_repeat() ? sampler_.loop_length() - 1 - clamped : clamped;
        begin_held_transition(TransitionKind::HeldToWet);
        return true;
    }

    void reset() noexcept {
        sampler_.reset();
        status_ = Status::Idle;
        pending_capture_ = false;
        active_loop_ = false;
        last_capture_rejected_ = false;
        loop_position_ = 0;
        completed_repeats_ = 0;
        alternate_repeat_ = false;
        transition_kind_ = TransitionKind::None;
        transition_position_ = 0;
        have_expected_sample_ = false;
        std::fill(last_output_.begin(), last_output_.end(), SampleType{});
        std::fill(transition_old_.begin(), transition_old_.end(), SampleType{});
    }

    /// A seek, loop jump, or clock-domain replacement invalidates both pending
    /// grid ownership and the continuity claim of the rolling history.
    void transport_discontinuity() noexcept {
        reset();
    }

    void process(const SampleType* const* input, SampleType* const* output, int frames,
                 timebase::SamplePosition block_start) noexcept {
        if (!prepared_ || frames <= 0 || input == nullptr || output == nullptr)
            return;
        for (int channel = 0; channel < sampler_.channels(); ++channel) {
            if (input[channel] == nullptr || output[channel] == nullptr)
                return;
        }
        std::int64_t block_end = 0;
        if (!detail::beat_repeat_checked_add(block_start.value, frames, block_end)) {
            transport_discontinuity();
            copy_dry(input, output, 0, frames);
            return;
        }
        if (have_expected_sample_ && block_start.value != expected_sample_) {
            transport_discontinuity();
        }

        int capture_offset = -1;
        if (pending_capture_ && pending_plan_.edge_sample.value >= block_start.value &&
            pending_plan_.edge_sample.value < block_end) {
            capture_offset = static_cast<int>(pending_plan_.edge_sample.value - block_start.value);
        } else if (pending_capture_ && pending_plan_.edge_sample.value < block_start.value) {
            pending_capture_ = false;
            status_ = active_loop_ ? Status::Active : Status::Idle;
        }

        if (capture_offset >= 0) {
            write_history(input, 0, capture_offset);
            render_segment(input, output, 0, capture_offset);
            if (pending_capture_)
                capture_pending();
            write_history(input, capture_offset, frames - capture_offset);
            render_segment(input, output, capture_offset, frames - capture_offset);
        } else {
            write_history(input, 0, frames);
            render_segment(input, output, 0, frames);
        }
        expected_sample_ = block_end;
        have_expected_sample_ = true;
    }

    Snapshot snapshot() const {
        Snapshot result;
        result.resumable = can_snapshot();
        result.sample_rate = sample_rate_;
        result.repeat_count = repeat_count_;
        result.gate = gate_;
        result.reverse = reverse_;
        // A release is a transient route back to dry audio, not persistent
        // gesture state. Serializing it as inactive prevents state recall from
        // reviving a gesture that was already stopped.
        result.active = active_loop_ && status_ != Status::Releasing;
        result.alternate_repeat = result.active && alternate_repeat_;
        result.last_capture_rejected = last_capture_rejected_;
        result.transition_frames = transition_frames_;
        if (result.active) {
            result.loop_position = loop_position_;
            result.completed_repeats = completed_repeats_;
            result.sampler = sampler_.snapshot();
            result.transition = transition_kind_;
            result.transition_position = transition_position_;
            result.transition_length = active_transition_frames();
            result.transition_old = transition_old_;
            result.last_output = last_output_;
        } else {
            result.last_output.assign(last_output_.size(), SampleType{});
        }
        return result;
    }

    bool restore(const Snapshot& value) {
        if (!prepared_ || !value.resumable || value.repeat_count < 0 ||
            !value.sample_rate.valid() || value.sample_rate.normalized() != sample_rate_ ||
            value.transition_frames < 0 || !std::isfinite(static_cast<double>(value.gate)) ||
            value.gate < SampleType{} || value.gate > SampleType{1} ||
            (value.reverse != ReverseMode::Off && value.reverse != ReverseMode::On &&
             value.reverse != ReverseMode::Alternate) ||
            value.last_output.size() != last_output_.size())
            return false;
        if (!value.active &&
            (!value.sampler.empty() || value.loop_position != 0 || value.completed_repeats != 0 ||
             value.alternate_repeat || value.transition != TransitionKind::None ||
             value.transition_position != 0 || value.transition_length != 0 ||
             !value.transition_old.empty()))
            return false;
        for (const auto sample : value.last_output) {
            if (!std::isfinite(static_cast<double>(sample)))
                return false;
        }
        if (value.active) {
            if (value.sampler.size() < 4 || !std::isfinite(static_cast<double>(value.sampler[0])) ||
                !std::isfinite(static_cast<double>(value.sampler[1])) ||
                value.sampler[0] != static_cast<SampleType>(sampler_.channels()) ||
                value.sampler[1] < SampleType{1} ||
                value.sampler[1] > static_cast<SampleType>(sampler_.capacity()) ||
                std::trunc(value.sampler[1]) != value.sampler[1] || value.loop_position < 0 ||
                value.loop_position >= static_cast<int>(value.sampler[1]) ||
                value.completed_repeats < 0 ||
                (value.completed_repeats < std::numeric_limits<int>::max() &&
                 value.alternate_repeat != (value.completed_repeats % 2 != 0)) ||
                (value.repeat_count > 0 && value.completed_repeats >= value.repeat_count))
                return false;
            const auto loop_length = static_cast<int>(value.sampler[1]);
            const auto expected_size =
                static_cast<std::size_t>(4) + static_cast<std::size_t>(sampler_.channels()) *
                                                  static_cast<std::size_t>(loop_length);
            if (value.sampler.size() != expected_size ||
                !std::isfinite(static_cast<double>(value.sampler[3])) ||
                std::trunc(value.sampler[3]) != value.sampler[3] ||
                value.sampler[3] < SampleType{} ||
                value.sampler[3] >= static_cast<SampleType>(loop_length))
                return false;
            const auto transition_length = std::min(value.transition_frames, loop_length / 2);
            if ((value.transition != TransitionKind::None &&
                 value.transition != TransitionKind::DryToWet &&
                 value.transition != TransitionKind::HeldToWet) ||
                value.transition_old.size() != transition_old_.size() ||
                value.transition_position < 0 || value.transition_length < 0 ||
                value.transition_length != transition_length ||
                (value.transition == TransitionKind::None && value.transition_position != 0) ||
                (value.transition != TransitionKind::None &&
                 (transition_length == 0 || value.transition_position >= transition_length)))
                return false;
            for (const auto sample : value.transition_old) {
                if (!std::isfinite(static_cast<double>(sample)))
                    return false;
            }
            for (const auto sample : value.sampler) {
                if (!std::isfinite(static_cast<double>(sample)))
                    return false;
            }
            if (!sampler_.restore(value.sampler))
                return false;
        } else {
            sampler_.release();
        }
        sampler_.clear_history();
        repeat_count_ = value.repeat_count;
        gate_ = value.gate;
        reverse_ = value.reverse;
        transition_frames_ = value.transition_frames;
        loop_position_ = value.active ? value.loop_position : 0;
        completed_repeats_ = value.active ? value.completed_repeats : 0;
        alternate_repeat_ = value.active && value.alternate_repeat;
        active_loop_ = value.active;
        last_capture_rejected_ = value.last_capture_rejected;
        last_output_ = value.last_output;
        transition_kind_ = value.active ? value.transition : TransitionKind::None;
        transition_position_ = value.active ? value.transition_position : 0;
        if (value.active)
            transition_old_ = value.transition_old;
        else
            std::fill(transition_old_.begin(), transition_old_.end(), SampleType{});
        pending_capture_ = false;
        status_ = last_capture_rejected_ ? Status::CaptureRejectedInsufficientHistory
                                         : (active_loop_ ? Status::Active : Status::Idle);
        have_expected_sample_ = false;
        return true;
    }

  private:
    void swap(BeatRepeatKernelT& other) noexcept {
        using std::swap;
        swap(sampler_, other.sampler_);
        swap(sample_rate_, other.sample_rate_);
        swap(history_inputs_, other.history_inputs_);
        swap(last_output_, other.last_output_);
        swap(transition_old_, other.transition_old_);
        swap(pending_plan_, other.pending_plan_);
        swap(repeat_count_, other.repeat_count_);
        swap(gate_, other.gate_);
        swap(reverse_, other.reverse_);
        swap(status_, other.status_);
        swap(transition_kind_, other.transition_kind_);
        swap(transition_frames_, other.transition_frames_);
        swap(transition_position_, other.transition_position_);
        swap(loop_position_, other.loop_position_);
        swap(completed_repeats_, other.completed_repeats_);
        swap(expected_sample_, other.expected_sample_);
        swap(prepared_, other.prepared_);
        swap(pending_capture_, other.pending_capture_);
        swap(active_loop_, other.active_loop_);
        swap(alternate_repeat_, other.alternate_repeat_);
        swap(last_capture_rejected_, other.last_capture_rejected_);
        swap(have_expected_sample_, other.have_expected_sample_);
    }

    int active_transition_frames() const noexcept {
        if (!active_loop_ || sampler_.loop_length() <= 0)
            return transition_frames_;
        return std::min(transition_frames_, sampler_.loop_length() / 2);
    }

    void begin_held_transition(TransitionKind kind) noexcept {
        std::copy(last_output_.begin(), last_output_.end(), transition_old_.begin());
        transition_kind_ = active_transition_frames() == 0 ? TransitionKind::None : kind;
        transition_position_ = 0;
        if (transition_kind_ == TransitionKind::None && kind == TransitionKind::HeldToDry)
            finish_release();
    }

    void capture_pending() noexcept {
        pending_capture_ = false;
        const auto capture = sampler_.try_freeze_recent(pending_plan_.capture_frames);
        if (capture != FreezeLoopSamplerT<SampleType>::CaptureResult::Success) {
            last_capture_rejected_ = true;
            status_ = Status::CaptureRejectedInsufficientHistory;
            return;
        }
        const bool replacing = active_loop_;
        active_loop_ = true;
        loop_position_ = 0;
        completed_repeats_ = 0;
        alternate_repeat_ = false;
        status_ = Status::Active;
        last_capture_rejected_ = false;
        if (replacing) {
            begin_held_transition(TransitionKind::HeldToWet);
        } else {
            transition_kind_ =
                active_transition_frames() == 0 ? TransitionKind::None : TransitionKind::DryToWet;
            transition_position_ = 0;
        }
    }

    void write_history(const SampleType* const* input, int offset, int frames) noexcept {
        if (frames <= 0)
            return;
        for (int channel = 0; channel < sampler_.channels(); ++channel)
            history_inputs_[static_cast<std::size_t>(channel)] = input[channel] + offset;
        sampler_.write(history_inputs_.data(), frames);
    }

    void copy_dry(const SampleType* const* input, SampleType* const* output, int offset,
                  int frames) noexcept {
        for (int channel = 0; channel < sampler_.channels(); ++channel) {
            for (int frame = 0; frame < frames; ++frame)
                output[channel][offset + frame] = finite_sample(input[channel][offset + frame]);
        }
    }

    bool reverse_for_current_repeat() const noexcept {
        return reverse_ == ReverseMode::On ||
               (reverse_ == ReverseMode::Alternate && alternate_repeat_);
    }

    SampleType gate_gain() const noexcept {
        const int length = sampler_.loop_length();
        if (length <= 0 || gate_ <= SampleType{})
            return SampleType{};
        if (gate_ >= SampleType{1})
            return SampleType{1};
        const int audible = std::clamp(
            static_cast<int>(std::llround(static_cast<double>(gate_) * length)), 0, length);
        if (audible <= 0 || loop_position_ >= audible)
            return SampleType{};
        const int fade = std::min(active_transition_frames(), audible / 2);
        if (fade <= 1)
            return SampleType{1};
        if (loop_position_ < fade) {
            const auto t =
                static_cast<SampleType>(loop_position_) / static_cast<SampleType>(fade - 1);
            SampleType old_gain{};
            SampleType new_gain{};
            crossfade_gains(crossfade_smoothstep(t), CrossfadeGainLaw::EqualGain, old_gain,
                            new_gain);
            return new_gain;
        }
        if (loop_position_ >= audible - fade) {
            const auto t = static_cast<SampleType>(loop_position_ - (audible - fade)) /
                           static_cast<SampleType>(fade - 1);
            SampleType old_gain{};
            SampleType new_gain{};
            crossfade_gains(crossfade_smoothstep(t), CrossfadeGainLaw::EqualGain, old_gain,
                            new_gain);
            return old_gain;
        }
        return SampleType{1};
    }

    SampleType wet_sample(int channel) const noexcept {
        const int length = sampler_.loop_length();
        if (!active_loop_ || length <= 0)
            return SampleType{};
        const int index =
            reverse_for_current_repeat() ? length - 1 - loop_position_ : loop_position_;
        return sampler_.loop_sample(channel, index) * gate_gain();
    }

    void render_segment(const SampleType* const* input, SampleType* const* output, int offset,
                        int frames) noexcept {
        for (int frame = 0; frame < frames; ++frame) {
            SampleType old_gain{};
            SampleType new_gain{SampleType{1}};
            if (transition_kind_ != TransitionKind::None) {
                const auto length = active_transition_frames();
                const auto t = length <= 1 ? SampleType{1}
                                           : static_cast<SampleType>(transition_position_) /
                                                 static_cast<SampleType>(length - 1);
                crossfade_gains(crossfade_smoothstep(t), CrossfadeGainLaw::EqualGain, old_gain,
                                new_gain);
            }

            for (int channel = 0; channel < sampler_.channels(); ++channel) {
                const auto dry = finite_sample(input[channel][offset + frame]);
                const auto wet = wet_sample(channel);
                SampleType rendered = active_loop_ ? wet : dry;
                switch (transition_kind_) {
                case TransitionKind::None:
                    break;
                case TransitionKind::DryToWet:
                    rendered = dry * old_gain + wet * new_gain;
                    break;
                case TransitionKind::HeldToWet:
                    rendered = transition_old_[static_cast<std::size_t>(channel)] * old_gain +
                               wet * new_gain;
                    break;
                case TransitionKind::HeldToDry:
                    rendered = transition_old_[static_cast<std::size_t>(channel)] * old_gain +
                               dry * new_gain;
                    break;
                }
                rendered = finite_sample(rendered);
                output[channel][offset + frame] = rendered;
                last_output_[static_cast<std::size_t>(channel)] = rendered;
            }

            advance_transition();
            if (active_loop_)
                advance_loop();
        }
    }

    void advance_transition() noexcept {
        if (transition_kind_ == TransitionKind::None)
            return;
        const auto completed_kind = transition_kind_;
        if (++transition_position_ < active_transition_frames())
            return;
        transition_kind_ = TransitionKind::None;
        transition_position_ = 0;
        if (completed_kind == TransitionKind::HeldToDry)
            finish_release();
    }

    static SampleType finite_sample(SampleType value) noexcept {
        return std::isfinite(static_cast<double>(value)) ? value : SampleType{};
    }

    void advance_loop() noexcept {
        if (++loop_position_ < sampler_.loop_length())
            return;
        loop_position_ = 0;
        if (completed_repeats_ < std::numeric_limits<int>::max())
            ++completed_repeats_;
        alternate_repeat_ = !alternate_repeat_;
        if (repeat_count_ > 0 && completed_repeats_ >= repeat_count_) {
            pending_capture_ = false;
            status_ = Status::Releasing;
            if (transition_kind_ != TransitionKind::HeldToDry)
                begin_held_transition(TransitionKind::HeldToDry);
            return;
        }
        if (transition_kind_ == TransitionKind::None)
            begin_held_transition(TransitionKind::HeldToWet);
    }

    void finish_release() noexcept {
        active_loop_ = false;
        sampler_.release();
        status_ = pending_capture_ ? Status::Armed : Status::Idle;
        loop_position_ = 0;
        completed_repeats_ = 0;
        alternate_repeat_ = false;
    }

    FreezeLoopSamplerT<SampleType> sampler_;
    timebase::RationalRate sample_rate_{48'000, 1};
    std::vector<const SampleType*> history_inputs_;
    std::vector<SampleType> last_output_;
    std::vector<SampleType> transition_old_;
    BeatRepeatCapturePlan pending_plan_{};
    int repeat_count_ = 4;
    SampleType gate_ = SampleType{1};
    ReverseMode reverse_ = ReverseMode::Off;
    Status status_ = Status::Idle;
    TransitionKind transition_kind_ = TransitionKind::None;
    int transition_frames_ = 144;
    int transition_position_ = 0;
    int loop_position_ = 0;
    int completed_repeats_ = 0;
    std::int64_t expected_sample_ = 0;
    bool prepared_ = false;
    bool pending_capture_ = false;
    bool active_loop_ = false;
    bool alternate_repeat_ = false;
    bool last_capture_rejected_ = false;
    bool have_expected_sample_ = false;
};

using BeatRepeatKernel = BeatRepeatKernelT<float>;
using BeatRepeatKernel64 = BeatRepeatKernelT<double>;

} // namespace pulp::signal
