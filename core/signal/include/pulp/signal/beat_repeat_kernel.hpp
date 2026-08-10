#pragma once

/// @file beat_repeat_kernel.hpp
/// Tempo-map-quantized capture and bounded repeat gestures over the existing
/// FreezeLoopSampler history owner.
///
/// The archived design also proposed pitch-shifted playback. That feature is
/// deliberately not advertised here: FractionalDelayHistoryT owns a separate
/// history buffer and cannot read this immutable capture without duplicating
/// it. This kernel closes the currently admitted history/timing, repeat, gate,
/// reverse, seek, and transition contract while retaining one full-buffer
/// owner.
///
/// RT contract: prepare(), snapshot(), and restore() may allocate. Once
/// prepared, process(), reset(), and all event handling are bounded,
/// allocation-free, lock-free, and I/O-free. Events carry offsets inside the
/// current block; capture always occurs before that edge sample is written to
/// dry history, so the immutable source interval is exactly [edge-N, edge).

#include <pulp/signal/crossfade.hpp>
#include <pulp/signal/freeze_loop_sampler.hpp>
#include <pulp/timebase/beat_division.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

enum class BeatRepeatDirection : std::uint8_t {
    Forward,
    Reverse,
    Alternate,
};

enum class BeatRepeatState : std::uint8_t {
    Idle,
    Armed,
    Active,
    ActiveArmed,
    Releasing,
    ReleasingArmed,
};

enum class BeatRepeatError : std::uint8_t {
    None,
    NotPrepared,
    InvalidArgument,
    InvalidEventOrder,
    TempoMapRateMismatch,
    InsufficientHistory,
    PositionOverflow,
    NonFiniteInput,
};

struct BeatRepeatEvent {
    enum class Type : std::uint8_t {
        Trigger,
        Stop,
        Seek,
    };

    Type type = Type::Trigger;
    std::uint32_t frame_offset = 0;
    /// Forward-coordinate frame in the immutable capture. Direction affects
    /// subsequent stepping, never the coordinate accepted here.
    std::int64_t seek_frame = 0;
};

/// Events must be ordered by nondecreasing frame_offset. Events sharing one
/// frame are applied in span order, before capture/render/history for that
/// frame; callers can therefore express stop-then-trigger or trigger-then-stop
/// deliberately without an implicit precedence table.
struct BeatRepeatProcessResult {
    BeatRepeatError error = BeatRepeatError::None;
    std::size_t processed_frames = 0;
    std::size_t rejected_events = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == BeatRepeatError::None;
    }
};

template <typename SampleType = float> class BeatRepeatKernelT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    struct Snapshot {
        std::uint32_t version = 1;
        bool restorable = false;
        std::vector<SampleType> capture;
        std::vector<SampleType> last_wet;
        std::vector<SampleType> last_rendered;
        timebase::BeatDivision division = timebase::BeatDivision::Eighth;
        BeatRepeatDirection direction = BeatRepeatDirection::Forward;
        std::uint32_t repeat_count = 0;
        SampleType gate = SampleType{1};
        std::size_t transition_samples = 0;
        std::size_t phase = 0;
        std::uint32_t completed_cells = 0;
        bool active = false;
        bool capture_rejected = false;
        BeatRepeatError error = BeatRepeatError::None;
    };

    BeatRepeatKernelT() = default;
    BeatRepeatKernelT(const BeatRepeatKernelT&) = delete;
    BeatRepeatKernelT& operator=(const BeatRepeatKernelT&) = delete;
    BeatRepeatKernelT(BeatRepeatKernelT&&) noexcept = default;
    BeatRepeatKernelT& operator=(BeatRepeatKernelT&&) noexcept = default;

    /// Prepare one dry-history/capture owner and bounded transition scratch.
    /// Returns false atomically for invalid or unrepresentable bounds.
    [[nodiscard]] bool prepare(timebase::RationalRate sample_rate, std::size_t channels,
                               std::size_t maximum_history_frames,
                               std::size_t maximum_transition_frames) {
        const auto normalized = sample_rate.normalized();
        if (!normalized.valid() || channels == 0 || channels > 64 || maximum_history_frames == 0 ||
            maximum_history_frames > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            maximum_transition_frames > maximum_history_frames ||
            channels > std::numeric_limits<std::size_t>::max() / maximum_history_frames) {
            return false;
        }

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            std::vector<SampleType> replacement_last(channels, SampleType{});
            std::vector<SampleType> replacement_rendered(channels, SampleType{});
            std::vector<SampleType> replacement_from(channels, SampleType{});
            std::vector<SampleType> replacement_wrap(channels, SampleType{});
            std::vector<SampleType> replacement_frame(channels, SampleType{});

            FreezeLoopSamplerT<SampleType> replacement;
            replacement.prepare(static_cast<int>(channels),
                                static_cast<int>(maximum_history_frames), 0);
            if (replacement.capacity() != static_cast<int>(maximum_history_frames) ||
                replacement.channels() != static_cast<int>(channels))
                return false;

            history_ = std::move(replacement);
            last_wet_.swap(replacement_last);
            last_rendered_.swap(replacement_rendered);
            transition_from_.swap(replacement_from);
            wrap_from_.swap(replacement_wrap);
            frame_scratch_.swap(replacement_frame);
            sample_rate_ = normalized;
            channels_ = channels;
            maximum_transition_samples_ = maximum_transition_frames;
            transition_samples_ = maximum_transition_frames;
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

    void reset() noexcept {
        if (!prepared_)
            return;
        history_.reset();
        std::fill(last_wet_.begin(), last_wet_.end(), SampleType{});
        std::fill(last_rendered_.begin(), last_rendered_.end(), SampleType{});
        std::fill(transition_from_.begin(), transition_from_.end(), SampleType{});
        std::fill(wrap_from_.begin(), wrap_from_.end(), SampleType{});
        armed_ = false;
        active_ = false;
        releasing_ = false;
        capture_rejected_ = false;
        error_ = BeatRepeatError::None;
        capture_frames_ = 0;
        phase_ = 0;
        completed_cells_ = 0;
        transition_kind_ = TransitionKind::None;
        transition_position_ = 0;
        wrap_position_ = 0;
        epoch_known_ = false;
        stream_position_known_ = false;
    }

    void set_division(timebase::BeatDivision division) noexcept {
        if (timebase::division_ticks(division))
            division_ = division;
        else
            set_error(BeatRepeatError::InvalidArgument);
    }
    void set_repeat_count(std::uint32_t total_cells) noexcept {
        repeat_count_ = total_cells;
    }
    void set_gate(SampleType duty) noexcept {
        if (!std::isfinite(duty)) {
            set_error(BeatRepeatError::InvalidArgument);
            return;
        }
        gate_ = std::clamp(duty, SampleType{}, SampleType{1});
    }
    void set_direction(BeatRepeatDirection direction) noexcept {
        if (direction > BeatRepeatDirection::Alternate) {
            set_error(BeatRepeatError::InvalidArgument);
            return;
        }
        direction_ = direction;
    }
    void set_transition_samples(std::size_t samples) noexcept {
        transition_samples_ = std::min(samples, maximum_transition_samples_);
    }

    [[nodiscard]] timebase::BeatDivision division() const noexcept {
        return division_;
    }
    [[nodiscard]] std::uint32_t repeat_count() const noexcept {
        return repeat_count_;
    }
    [[nodiscard]] SampleType gate() const noexcept {
        return gate_;
    }
    [[nodiscard]] BeatRepeatDirection direction() const noexcept {
        return direction_;
    }
    [[nodiscard]] std::size_t transition_samples() const noexcept {
        return transition_samples_;
    }
    [[nodiscard]] BeatRepeatState state() const noexcept {
        if (releasing_)
            return armed_ ? BeatRepeatState::ReleasingArmed : BeatRepeatState::Releasing;
        if (active_)
            return armed_ ? BeatRepeatState::ActiveArmed : BeatRepeatState::Active;
        return armed_ ? BeatRepeatState::Armed : BeatRepeatState::Idle;
    }
    [[nodiscard]] BeatRepeatError last_error() const noexcept {
        return error_;
    }
    void clear_error() noexcept {
        error_ = BeatRepeatError::None;
    }
    [[nodiscard]] bool last_capture_rejected() const noexcept {
        return capture_rejected_;
    }
    [[nodiscard]] std::size_t captured_frames() const noexcept {
        return capture_frames_;
    }
    [[nodiscard]] std::size_t available_history() const noexcept {
        return static_cast<std::size_t>(std::max(0, history_.available_history()));
    }
    [[nodiscard]] constexpr int latency_samples() const noexcept {
        return 0;
    }
    [[nodiscard]] std::size_t max_tail_samples() const noexcept {
        return transition_samples_;
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return history_.retained_bytes() +
               (last_wet_.capacity() + last_rendered_.capacity() + transition_from_.capacity() +
                wrap_from_.capacity() + frame_scratch_.capacity()) *
                   sizeof(SampleType);
    }

    [[nodiscard]] BeatRepeatProcessResult
    process(const SampleType* const* input, SampleType* const* output, std::size_t frames,
            std::int64_t block_sample_start, std::uint64_t transport_epoch,
            const timebase::CompiledTempoMap& tempo_map,
            std::span<const BeatRepeatEvent> events = {}) noexcept {
        BeatRepeatProcessResult result;
        if (!prepared_)
            return fail_process(BeatRepeatError::NotPrepared);
        if ((frames != 0 && (input == nullptr || output == nullptr)) ||
            frames > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            return fail_process(BeatRepeatError::InvalidArgument);
        if (frames > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) ||
            block_sample_start >
                std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(frames)) {
            set_error(BeatRepeatError::PositionOverflow);
            return fail_process(BeatRepeatError::PositionOverflow);
        }
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            if (frames != 0 && (input[channel] == nullptr || output[channel] == nullptr))
                return fail_process(BeatRepeatError::InvalidArgument);
        }
        if (tempo_map.sample_rate().normalized() != sample_rate_) {
            set_error(BeatRepeatError::TempoMapRateMismatch);
            return fail_process(BeatRepeatError::TempoMapRateMismatch);
        }

        const bool discontinuity =
            (epoch_known_ && transport_epoch != transport_epoch_) ||
            (stream_position_known_ && block_sample_start != next_stream_sample_);
        if (discontinuity) {
            armed_ = false;
            history_.invalidate_history();
        }
        transport_epoch_ = transport_epoch;
        epoch_known_ = true;

        std::size_t event_index = 0;
        std::uint32_t previous_offset = 0;
        bool have_previous = false;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            while (event_index < events.size() && events[event_index].frame_offset == frame) {
                const auto& event = events[event_index];
                if ((have_previous && event.frame_offset < previous_offset) ||
                    event.frame_offset >= frames) {
                    set_error(BeatRepeatError::InvalidEventOrder);
                    ++result.rejected_events;
                } else {
                    handle_event(event, block_sample_start + static_cast<std::int64_t>(frame),
                                 transport_epoch, tempo_map, result);
                }
                previous_offset = event.frame_offset;
                have_previous = true;
                ++event_index;
            }

            const auto absolute_sample = block_sample_start + static_cast<std::int64_t>(frame);
            if (armed_ && armed_epoch_ == transport_epoch && absolute_sample == armed_sample_)
                commit_capture();

            for (std::size_t channel = 0; channel < channels_; ++channel) {
                const auto value = input[channel][frame];
                if (std::isfinite(value)) {
                    frame_scratch_[channel] = value;
                } else {
                    frame_scratch_[channel] = SampleType{};
                    set_error(BeatRepeatError::NonFiniteInput);
                }
            }
            render_frame(output, frame);
            history_.write_frame(frame_scratch_.data());
            advance_playback();
            ++result.processed_frames;
        }

        while (event_index < events.size()) {
            set_error(BeatRepeatError::InvalidEventOrder);
            ++result.rejected_events;
            ++event_index;
        }
        if (frames <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) &&
            block_sample_start <=
                std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(frames)) {
            next_stream_sample_ = block_sample_start + static_cast<std::int64_t>(frames);
            stream_position_known_ = true;
        } else {
            stream_position_known_ = false;
            set_error(BeatRepeatError::PositionOverflow);
        }
        result.error = error_;
        return result;
    }

    [[nodiscard]] Snapshot snapshot() const {
        Snapshot value;
        value.restorable = !armed_ && !releasing_ && transition_kind_ == TransitionKind::None &&
                           wrap_position_ == 0;
        if (!value.restorable)
            return value;
        value.capture = history_.snapshot();
        value.last_wet = last_wet_;
        value.last_rendered = last_rendered_;
        value.division = division_;
        value.direction = direction_;
        value.repeat_count = repeat_count_;
        value.gate = gate_;
        value.transition_samples = transition_samples_;
        value.phase = phase_;
        value.completed_cells = completed_cells_;
        value.active = active_ && capture_frames_ != 0;
        value.capture_rejected = capture_rejected_;
        value.error = error_;
        return value;
    }

    [[nodiscard]] bool restore(const Snapshot& value) {
        if (!prepared_ || value.version != 1 || !value.restorable ||
            !timebase::division_ticks(value.division) ||
            value.direction > BeatRepeatDirection::Alternate || !std::isfinite(value.gate) ||
            value.gate < SampleType{} || value.gate > SampleType{1} ||
            value.transition_samples > maximum_transition_samples_ ||
            value.last_wet.size() != channels_ || value.last_rendered.size() != channels_ ||
            value.error > BeatRepeatError::NonFiniteInput ||
            (!value.active && !value.capture.empty()) ||
            !std::ranges::all_of(value.capture,
                                 [](SampleType sample) { return std::isfinite(sample); }) ||
            !std::ranges::all_of(value.last_wet,
                                 [](SampleType sample) { return std::isfinite(sample); }) ||
            !std::ranges::all_of(value.last_rendered,
                                 [](SampleType sample) { return std::isfinite(sample); }))
            return false;

        FreezeLoopSamplerT<SampleType> replacement;
        replacement.prepare(static_cast<int>(channels_), history_.capacity(), 0);
        if (value.active && !replacement.restore(value.capture))
            return false;

        history_ = std::move(replacement);
        division_ = value.division;
        direction_ = value.direction;
        repeat_count_ = value.repeat_count;
        gate_ = value.gate;
        transition_samples_ = value.transition_samples;
        active_ = value.active;
        armed_ = false;
        releasing_ = false;
        capture_frames_ = active_ ? static_cast<std::size_t>(history_.loop_length()) : 0;
        phase_ = capture_frames_ == 0 ? 0 : value.phase % capture_frames_;
        completed_cells_ = value.completed_cells;
        std::copy(value.last_wet.begin(), value.last_wet.end(), last_wet_.begin());
        std::copy(value.last_rendered.begin(), value.last_rendered.end(), last_rendered_.begin());
        capture_rejected_ = value.capture_rejected;
        error_ = value.error;
        transition_kind_ = TransitionKind::None;
        transition_position_ = 0;
        wrap_position_ = 0;
        return true;
    }

  private:
    enum class TransitionKind : std::uint8_t {
        None,
        DryToWet,
        WetToDry,
        WetToWet,
    };

    static BeatRepeatProcessResult fail_process(BeatRepeatError error) noexcept {
        return {error, 0, 0};
    }

    void set_error(BeatRepeatError error) noexcept {
        if (error_ == BeatRepeatError::None)
            error_ = error;
    }

    void handle_event(const BeatRepeatEvent& event, std::int64_t absolute_sample,
                      std::uint64_t epoch, const timebase::CompiledTempoMap& tempo_map,
                      BeatRepeatProcessResult& result) noexcept {
        switch (event.type) {
        case BeatRepeatEvent::Type::Trigger:
            if (!arm(absolute_sample, epoch, tempo_map))
                ++result.rejected_events;
            break;
        case BeatRepeatEvent::Type::Stop:
            armed_ = false;
            if (active_)
                begin_release();
            break;
        case BeatRepeatEvent::Type::Seek:
            if (!active_ || capture_frames_ == 0) {
                ++result.rejected_events;
                set_error(BeatRepeatError::InvalidArgument);
                break;
            }
            begin_wet_transition();
            seek_forward_coordinate(event.seek_frame);
            break;
        default:
            ++result.rejected_events;
            set_error(BeatRepeatError::InvalidArgument);
            break;
        }
    }

    [[nodiscard]] bool arm(std::int64_t trigger_sample, std::uint64_t epoch,
                           const timebase::CompiledTempoMap& tempo_map) noexcept {
        const auto duration_result = timebase::division_ticks(division_);
        if (!duration_result) {
            set_error(BeatRepeatError::InvalidArgument);
            return false;
        }
        const auto duration = duration_result.value().value;
        const auto trigger_tick =
            tempo_map.fractional_samples_to_ticks(static_cast<long double>(trigger_sample));
        if (!std::isfinite(trigger_tick)) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        const auto quotient_value = std::floor(trigger_tick / static_cast<long double>(duration));
        if (quotient_value < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
            quotient_value > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        const auto quotient = static_cast<std::int64_t>(quotient_value);
        if (quotient == std::numeric_limits<std::int64_t>::max()) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        const auto next_quotient = quotient + 1;
        if (next_quotient > std::numeric_limits<std::int64_t>::max() / duration ||
            next_quotient < std::numeric_limits<std::int64_t>::min() / duration) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        auto target_tick_value = next_quotient * duration;
        auto target_sample = tempo_map.ticks_to_samples({target_tick_value}).value;
        // Sparse tick grids can resolve the trigger to the nearest tick on the
        // far side. The event contract is strict: the armed edge is > trigger.
        if (target_sample <= trigger_sample) {
            if (target_tick_value > std::numeric_limits<std::int64_t>::max() - duration) {
                set_error(BeatRepeatError::PositionOverflow);
                return false;
            }
            target_tick_value += duration;
            target_sample = tempo_map.ticks_to_samples({target_tick_value}).value;
        }
        if (target_sample <= trigger_sample ||
            target_tick_value < std::numeric_limits<std::int64_t>::min() + duration) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        const auto begin_sample = tempo_map.ticks_to_samples({target_tick_value - duration}).value;
        if (begin_sample >= target_sample) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        const auto difference =
            static_cast<std::uint64_t>(target_sample) - static_cast<std::uint64_t>(begin_sample);
        if (difference == 0 ||
            difference > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            set_error(BeatRepeatError::PositionOverflow);
            return false;
        }
        armed_ = true;
        armed_epoch_ = epoch;
        armed_sample_ = target_sample;
        armed_frames_ = static_cast<std::size_t>(difference);
        return true;
    }

    void commit_capture() noexcept {
        armed_ = false;
        if (armed_frames_ > static_cast<std::size_t>(history_.capacity()) ||
            !history_.capture_recent_exact(static_cast<int>(armed_frames_))) {
            capture_rejected_ = true;
            set_error(BeatRepeatError::InsufficientHistory);
            return;
        }

        const bool replacing = active_;
        if (replacing)
            begin_wet_transition();
        capture_frames_ = armed_frames_;
        phase_ = 0;
        completed_cells_ = 0;
        wrap_position_ = 0;
        active_ = true;
        releasing_ = false;
        capture_rejected_ = false;
        if (!replacing) {
            transition_kind_ = TransitionKind::DryToWet;
            transition_position_ = 0;
        }
    }

    void begin_wet_transition() noexcept {
        std::copy(last_rendered_.begin(), last_rendered_.end(), transition_from_.begin());
        releasing_ = false;
        transition_kind_ = TransitionKind::WetToWet;
        transition_position_ = 0;
    }

    void begin_release() noexcept {
        std::copy(last_rendered_.begin(), last_rendered_.end(), transition_from_.begin());
        releasing_ = true;
        transition_kind_ = TransitionKind::WetToDry;
        transition_position_ = 0;
    }

    [[nodiscard]] std::size_t bounded_transition() const noexcept {
        if (capture_frames_ == 0)
            return 0;
        return std::min(transition_samples_, capture_frames_ / 2);
    }

    [[nodiscard]] bool reverse_for_cell() const noexcept {
        return direction_ == BeatRepeatDirection::Reverse ||
               (direction_ == BeatRepeatDirection::Alternate && (completed_cells_ & 1u) != 0u);
    }

    void seek_forward_coordinate(std::int64_t coordinate) noexcept {
        const auto clamped =
            std::clamp<std::int64_t>(coordinate, 0, static_cast<std::int64_t>(capture_frames_ - 1));
        const auto forward = static_cast<std::size_t>(clamped);
        phase_ = reverse_for_cell() ? capture_frames_ - 1 - forward : forward;
        wrap_position_ = 0;
    }

    [[nodiscard]] SampleType gate_gain(std::size_t phase) const noexcept {
        if (gate_ >= SampleType{1})
            return SampleType{1};
        const auto audible =
            std::min(capture_frames_, static_cast<std::size_t>(std::llround(
                                          static_cast<long double>(gate_) * capture_frames_)));
        if (audible == 0 || phase >= audible)
            return SampleType{};
        const auto fade = std::min({bounded_transition(), audible, capture_frames_ - audible});
        if (fade == 0)
            return SampleType{1};
        SampleType gain = SampleType{1};
        if (phase < fade) {
            gain = crossfade_smoothstep(static_cast<SampleType>(phase + 1) /
                                        static_cast<SampleType>(fade));
        }
        if (phase + fade >= audible) {
            const auto remaining = audible - phase;
            gain = std::min(gain, crossfade_smoothstep(static_cast<SampleType>(remaining) /
                                                       static_cast<SampleType>(fade)));
        }
        return gain;
    }

    [[nodiscard]] SampleType wet_sample(std::size_t channel) const noexcept {
        if (!active_ || capture_frames_ == 0)
            return SampleType{};
        const auto capture = history_.captured_channel(static_cast<int>(channel));
        if (capture.size() != capture_frames_)
            return SampleType{};
        const auto index = reverse_for_cell() ? capture_frames_ - 1 - phase_ : phase_;
        return capture[index] * gate_gain(phase_);
    }

    void render_frame(SampleType* const* output, std::size_t frame) noexcept {
        const auto fade = bounded_transition();
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            const auto dry = frame_scratch_[channel];
            auto wet = wet_sample(channel);
            if (wrap_position_ != 0 && fade != 0 && wrap_position_ <= fade) {
                const auto u = crossfade_smoothstep(static_cast<SampleType>(wrap_position_) /
                                                    static_cast<SampleType>(fade));
                SampleType old_gain{};
                SampleType new_gain{};
                crossfade_gains(u, CrossfadeGainLaw::EqualGain, old_gain, new_gain);
                wet = wrap_from_[channel] * old_gain + wet * new_gain;
            }

            auto rendered = active_ ? wet : dry;
            if (transition_kind_ != TransitionKind::None && fade != 0) {
                const auto u = crossfade_smoothstep(
                    static_cast<SampleType>(std::min(transition_position_ + 1, fade)) /
                    static_cast<SampleType>(fade));
                SampleType old_gain{};
                SampleType new_gain{};
                crossfade_gains(u, CrossfadeGainLaw::EqualGain, old_gain, new_gain);
                if (transition_kind_ == TransitionKind::DryToWet)
                    rendered = dry * old_gain + wet * new_gain;
                else if (transition_kind_ == TransitionKind::WetToDry)
                    rendered = transition_from_[channel] * old_gain + dry * new_gain;
                else
                    rendered = transition_from_[channel] * old_gain + wet * new_gain;
            } else if (transition_kind_ == TransitionKind::WetToDry && fade == 0) {
                rendered = dry;
            }
            output[channel][frame] = std::isfinite(rendered) ? rendered : SampleType{};
            last_rendered_[channel] = output[channel][frame];
            last_wet_[channel] = wet;
        }
    }

    void advance_playback() noexcept {
        if (!active_ || capture_frames_ == 0)
            return;
        const auto fade = bounded_transition();
        if (transition_kind_ != TransitionKind::None) {
            if (fade == 0 || ++transition_position_ >= fade) {
                const bool finished_release = transition_kind_ == TransitionKind::WetToDry;
                transition_kind_ = TransitionKind::None;
                transition_position_ = 0;
                if (finished_release) {
                    releasing_ = false;
                    active_ = false;
                    capture_frames_ = 0;
                    history_.release();
                    return;
                }
            }
        }
        if (wrap_position_ != 0 && (fade == 0 || ++wrap_position_ > fade))
            wrap_position_ = 0;

        if (++phase_ < capture_frames_)
            return;
        phase_ = 0;
        ++completed_cells_;
        if (repeat_count_ != 0 && completed_cells_ >= repeat_count_) {
            begin_release();
            return;
        }
        std::copy(last_wet_.begin(), last_wet_.end(), wrap_from_.begin());
        wrap_position_ = fade == 0 ? 0 : 1;
    }

    FreezeLoopSamplerT<SampleType> history_;
    std::vector<SampleType> last_wet_;
    std::vector<SampleType> last_rendered_;
    std::vector<SampleType> transition_from_;
    std::vector<SampleType> wrap_from_;
    std::vector<SampleType> frame_scratch_;
    timebase::RationalRate sample_rate_{};
    std::size_t channels_ = 0;
    std::size_t maximum_transition_samples_ = 0;
    std::size_t transition_samples_ = 0;
    timebase::BeatDivision division_ = timebase::BeatDivision::Eighth;
    BeatRepeatDirection direction_ = BeatRepeatDirection::Forward;
    std::uint32_t repeat_count_ = 0;
    SampleType gate_ = SampleType{1};
    bool prepared_ = false;
    bool armed_ = false;
    bool active_ = false;
    bool releasing_ = false;
    bool capture_rejected_ = false;
    BeatRepeatError error_ = BeatRepeatError::None;
    std::uint64_t transport_epoch_ = 0;
    std::uint64_t armed_epoch_ = 0;
    bool epoch_known_ = false;
    bool stream_position_known_ = false;
    std::int64_t next_stream_sample_ = 0;
    std::int64_t armed_sample_ = 0;
    std::size_t armed_frames_ = 0;
    std::size_t capture_frames_ = 0;
    std::size_t phase_ = 0;
    std::uint32_t completed_cells_ = 0;
    TransitionKind transition_kind_ = TransitionKind::None;
    std::size_t transition_position_ = 0;
    std::size_t wrap_position_ = 0;
};

using BeatRepeatKernel = BeatRepeatKernelT<float>;
using BeatRepeatKernel64 = BeatRepeatKernelT<double>;

} // namespace pulp::signal
