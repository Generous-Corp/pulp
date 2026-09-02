#pragma once

#include <pulp/midi/utility_contract.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/coordinate_random.hpp>
#include <pulp/timebase/quantize.hpp>
#include <pulp/timebase/ratchet.hpp>
#include <pulp/timebase/tick.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace pulp::midi {

enum class StepPlayerDirection : std::uint8_t {
    Forward,
    Reverse,
    PingPong,
    Random,
};

enum class StepPlayerTransportEvent : std::uint8_t {
    Continuous,
    Started,
    Stopped,
    Seeked,
    LoopWrapped,
};

enum class StepPlayerError : std::uint8_t {
    None,
    NotConfigured,
    InvalidLane,
    InvalidStepIndex,
    InvalidVelocity,
    InvalidGate,
    InvalidProbability,
    InvalidRatchetCount,
    InvalidPitchOffset,
    InvalidTimingOffset,
};

/// The authored data at one lane and step coordinate.
///
/// `gate_percent` scales the step interval into the note's held portion.
/// `tie` extends a still-sounding note of the same pitch instead of
/// retriggering it; `slide` lets a sounding note of a different pitch overlap
/// the new attack by one tick so a downstream voice can glide. Both are
/// continuity policies over gate and pitch, not separate event kinds.
struct StepPlayerStep {
    bool on = false;
    std::int8_t pitch_offset = 0;
    std::uint8_t velocity = 100;
    std::uint8_t gate_percent = 100;
    std::uint8_t probability_percent = 100;
    std::uint8_t ratchet_count = 1;
    /// Micro-timing nudge in ticks, applied to this step's hits and to their
    /// releases together so the gate length is preserved.
    ///
    /// This is what lets a caller place a hit off the grid. Swing, a groove
    /// table and a per-step nudge are all deterministic functions of the step's
    /// position, so a caller folds them into one offset and the kernel keeps
    /// owning the clocks, the sounding-note ledger and the releases. Without it
    /// a caller wanting swing has to run its own scheduler, which is the
    /// duplication this kernel exists to remove.
    ///
    /// The nudge is late-only, and a negative value is refused rather than
    /// honoured. A step is discovered when its own interval opens, so pulling
    /// it earlier would ask it to sound before the block that found it —
    /// whether it survived would then depend on where the host happened to put
    /// its callback boundaries, and the kernel's whole timing contract is that
    /// it does not. A caller wanting a symmetric groove biases the lane late
    /// and varies around that bias, which costs one constant and stays exact.
    ///
    /// A nudge is held inside the step's own interval at fire time, so it can
    /// never reach the next step's grid position and the sequence cannot be
    /// reordered against itself.
    std::int32_t timing_offset_ticks = 0;
    bool tie = false;
    bool slide = false;
    constexpr auto operator<=>(const StepPlayerStep&) const = default;
};

struct StepPlayerLaneSpec {
    std::uint8_t channel = 0;
    std::uint8_t base_note = 60;
    std::uint8_t step_count = 0;
    timebase::TickDuration step_duration{timebase::kTicksPerQuarter / 4};
    StepPlayerDirection direction = StepPlayerDirection::Forward;
    std::uint8_t choke_group = 0;
    bool enabled = true;
    constexpr auto operator<=>(const StepPlayerLaneSpec&) const = default;
};

/// Block timing for one sample-accurate step-player call.
///
/// `tick_start` and `sample_start` describe the same block boundary. A compiled
/// tempo map gives the exact tick-to-sample projection; callers without one can
/// leave it null and provide the block's constant tempo and sample rate.
/// `transport_event` is explicit because a loop wrap can keep the sample clock
/// monotonic while the musical clock jumps.
struct StepPlayerBlock {
    timebase::SamplePosition sample_start{};
    timebase::TickPosition tick_start{};
    std::int32_t sample_count = 0;
    timebase::RationalRate sample_rate{};
    double tempo_bpm = 120.0;
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    bool playing = true;
    StepPlayerTransportEvent transport_event = StepPlayerTransportEvent::Continuous;
};

struct StepPlayerCapacityContract {
    std::size_t maximum_lanes = 0;
    std::size_t maximum_steps_per_lane = 0;
    std::size_t maximum_steps_per_block = 0;
    std::size_t maximum_generated_events_per_block = 0;
    MidiUtilityOverflowPolicy overflow = MidiUtilityOverflowPolicy::DropUnstarted;
    MidiUtilitySameSampleOrder same_sample_order = MidiUtilitySameSampleOrder::ReleaseBeforeAttack;
    MidiUtilityTransportRequirement transport =
        MidiUtilityTransportRequirement::FlushOnDiscontinuity;
    bool requires_reserved_capacity_limited_output = true;
    bool requires_distinct_input_output = true;

    constexpr std::size_t required_output_events(std::size_t input_events) const noexcept {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        return input_events > maximum - maximum_generated_events_per_block
                   ? maximum
                   : input_events + maximum_generated_events_per_block;
    }
};

namespace step_player_detail {

constexpr std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) noexcept {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    return remainder != 0 && numerator < 0 ? quotient - 1 : quotient;
}

constexpr std::int64_t saturating_multiply(std::int64_t lhs, std::int64_t rhs) noexcept {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (lhs == 0 || rhs == 0)
        return 0;
    if (lhs == -1)
        return rhs == minimum ? maximum : -rhs;
    if (rhs == -1)
        return lhs == minimum ? maximum : -lhs;
    if (lhs > 0) {
        if (rhs > 0 && lhs > maximum / rhs)
            return maximum;
        if (rhs < 0 && rhs < minimum / lhs)
            return minimum;
    } else {
        if (rhs > 0 && lhs < minimum / rhs)
            return minimum;
        if (rhs < 0 && lhs < maximum / rhs)
            return maximum;
    }
    return lhs * rhs;
}

constexpr std::size_t positive_mod(std::int64_t value, std::size_t modulus) noexcept {
    auto result = value % static_cast<std::int64_t>(modulus);
    if (result < 0)
        result += static_cast<std::int64_t>(modulus);
    return static_cast<std::size_t>(result);
}

constexpr bool distance_exceeds(std::int64_t lhs, std::int64_t rhs,
                                std::uint64_t tolerance) noexcept {
    const auto magnitude = [](std::int64_t value) constexpr {
        return value >= 0 ? static_cast<std::uint64_t>(value)
                          : static_cast<std::uint64_t>(-(value + 1)) + 1;
    };
    if ((lhs < 0) == (rhs < 0)) {
        const auto left = magnitude(lhs);
        const auto right = magnitude(rhs);
        return (left >= right ? left - right : right - left) > tolerance;
    }
    const auto left = magnitude(lhs);
    const auto right = magnitude(rhs);
    return left > std::numeric_limits<std::uint64_t>::max() - right ||
           left + right > tolerance;
}

constexpr long double difference_as_long_double(std::int64_t lhs, std::int64_t rhs) noexcept {
    // Subtract before conversion when both values have the same sign. Their
    // difference is representable, and this preserves adjacent ticks at the
    // int64 limits on platforms where long double has only 53 mantissa bits.
    if ((lhs < 0) == (rhs < 0))
        return static_cast<long double>(lhs - rhs);
    return static_cast<long double>(lhs) - static_cast<long double>(rhs);
}

inline std::int64_t saturating_round(long double value) noexcept {
    constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (!std::isfinite(value) || value >= maximum)
        return value < 0 ? std::numeric_limits<std::int64_t>::min()
                         : std::numeric_limits<std::int64_t>::max();
    if (value <= minimum)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(std::llround(value));
}

constexpr bool valid_direction(StepPlayerDirection direction) noexcept {
    return direction >= StepPlayerDirection::Forward && direction <= StepPlayerDirection::Random;
}

} // namespace step_player_detail

/// A lane's authored coordinate at one grid tick.
///
/// `ordinal` counts step intervals from the tick origin, `cycle` counts
/// completed passes over the lane, and `index` is the step the lane's walk
/// selects. All three are pure functions of the lane's immutable spec and the
/// tick, so a caller can ask which cell is about to speak and author it before
/// `process()` rather than mirroring the walk and drifting from it.
struct StepPlayerCoordinate {
    timebase::TickPosition step_tick{};
    timebase::TickPosition following_tick{};
    std::int64_t ordinal = 0;
    std::uint64_t cycle = 0;
    std::size_t index = 0;
    constexpr auto operator<=>(const StepPlayerCoordinate&) const = default;
};

/// Fixed-capacity, sample-accurate multi-lane linear step player.
///
/// Each lane is an independent loop of up to `MaxSteps` authored steps over one
/// shared transport: its own length, step duration, direction walk, and choke
/// group. Lanes with different lengths or divisions realign at the common
/// multiple of their grid ordinals, so polymeter falls out of the exact tick
/// arithmetic rather than a dedicated mechanism.
///
/// The player is a generator: input passes through unchanged and generated
/// notes are merged in sample order. Every emitted attack owns exactly one
/// later release in the fixed sounding ledger; an attack that cannot secure a
/// ledger slot is suppressed rather than stranded, and a release that cannot
/// be emitted is retained as debt and retried. Step selection, probability,
/// and random walks are pure functions of the grid coordinate, so the event
/// stream is identical for any block partition of the same transport span.
/// A transport discontinuity is a flush plus a resync: every owned note is
/// released and every lane clock realigns to the grid at the new position.
template <std::size_t MaxLanes = 8, std::size_t MaxSteps = 32, std::size_t MaxRatchetHits = 8,
          std::size_t MaximumStepsPerBlock = 128>
class StepPlayer {
  public:
    static_assert(MaxLanes > 0);
    static_assert(MaxSteps > 0);
    static_assert(MaxRatchetHits > 0);
    static_assert(MaximumStepsPerBlock > 0);
    static_assert(MaxLanes <= std::numeric_limits<std::size_t>::max() / MaxSteps);
    static_assert(MaxLanes <= std::numeric_limits<std::size_t>::max() / (MaxRatchetHits + 1));
    static_assert(MaxLanes <= 32, "lane parking uses a 32-bit mask");

    static constexpr std::size_t kMaximumCells = MaxLanes * MaxSteps;
    /// A lane can hold one ratchet burst plus one note kept overlapping by a
    /// slide, so the ledger never needs more than this to avoid stranding.
    static constexpr std::size_t kMaximumSoundingNotes = MaxLanes * (MaxRatchetHits + 1);
    /// A micro-timing nudge is bounded to one quarter note: far wider than any
    /// swing or groove needs, and narrow enough that a value which would
    /// reorder the grid wholesale is refused rather than silently honoured.
    ///
    /// The field is 32-bit deliberately. A quarter is 705'600 ticks, so a
    /// 16-bit offset would cap at 32'767 — under a fifth of a single sixteenth
    /// step, which cannot express swing at all.
    static constexpr std::int32_t kMaximumTimingOffsetTicks =
        static_cast<std::int32_t>(timebase::kTicksPerQuarter);

    struct Spec {
        std::size_t lane_count = 0;
        std::array<StepPlayerLaneSpec, MaxLanes> lanes{};
        std::uint64_t random_seed = 0;
        constexpr auto operator<=>(const Spec&) const = default;
    };

    static constexpr StepPlayerCapacityContract contract() noexcept {
        constexpr auto burst_events = 2 * MaxRatchetHits;
        constexpr auto generated =
            MaximumStepsPerBlock > (std::numeric_limits<std::size_t>::max() -
                                    2 * kMaximumSoundingNotes) /
                                       burst_events
                ? std::numeric_limits<std::size_t>::max()
                : MaximumStepsPerBlock * burst_events + 2 * kMaximumSoundingNotes;
        return {MaxLanes,
                MaxSteps,
                MaximumStepsPerBlock,
                generated,
                MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity,
                true,
                true};
    }

    static constexpr bool valid_lane_spec(const StepPlayerLaneSpec& lane) noexcept {
        constexpr auto kMaximumStepDuration = timebase::kTicksPerQuarter * 64;
        return lane.channel <= 15 && lane.base_note <= 127 && lane.step_count >= 1 &&
               lane.step_count <= MaxSteps && lane.step_duration.value > 0 &&
               lane.step_duration.value <= kMaximumStepDuration &&
               step_player_detail::valid_direction(lane.direction);
    }

    static constexpr bool valid_spec(const Spec& spec) noexcept {
        if (spec.lane_count < 1 || spec.lane_count > MaxLanes)
            return false;
        for (std::size_t lane = 0; lane < spec.lane_count; ++lane)
            if (!valid_lane_spec(spec.lanes[lane]))
                return false;
        return true;
    }

    explicit constexpr StepPlayer(Spec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr const Spec& spec() const noexcept {
        return spec_;
    }

    static constexpr StepPlayerError validate_step(const StepPlayerStep& step) noexcept {
        if (step.velocity < 1 || step.velocity > 127)
            return StepPlayerError::InvalidVelocity;
        if (step.gate_percent < 1 || step.gate_percent > 100)
            return StepPlayerError::InvalidGate;
        if (step.probability_percent > 100)
            return StepPlayerError::InvalidProbability;
        if (step.ratchet_count < 1 || step.ratchet_count > MaxRatchetHits)
            return StepPlayerError::InvalidRatchetCount;
        if (step.pitch_offset < -48 || step.pitch_offset > 48)
            return StepPlayerError::InvalidPitchOffset;
        if (step.timing_offset_ticks < 0 || step.timing_offset_ticks > kMaximumTimingOffsetTicks)
            return StepPlayerError::InvalidTimingOffset;
        return StepPlayerError::None;
    }

    constexpr StepPlayerError set_step(std::size_t lane, std::size_t index,
                                       StepPlayerStep step) noexcept {
        if (!valid_)
            return StepPlayerError::NotConfigured;
        if (lane >= spec_.lane_count)
            return StepPlayerError::InvalidLane;
        if (index >= spec_.lanes[lane].step_count)
            return StepPlayerError::InvalidStepIndex;
        if (const auto error = validate_step(step); error != StepPlayerError::None)
            return error;
        steps_[lane * MaxSteps + index] = step;
        return StepPlayerError::None;
    }

    constexpr const StepPlayerStep* get_step(std::size_t lane, std::size_t index) const noexcept {
        if (!valid_ || lane >= spec_.lane_count || index >= spec_.lanes[lane].step_count)
            return nullptr;
        return &steps_[lane * MaxSteps + index];
    }

    /// Authored-step edits are control-thread work; playback state is untouched
    /// so an in-flight note still receives its owned release.
    constexpr void clear_steps() noexcept {
        for (auto& step : steps_)
            step = {};
    }

    std::size_t sounding_note_count() const noexcept {
        std::size_t count = 0;
        for (const auto& note : sounding_)
            if (note.active)
                ++count;
        return count;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output,
                                     const StepPlayerBlock& block) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!ordered(input)) {
            // Generation interleaves by sample offset, so unordered input
            // disables it; the input itself still passes through.
            passthrough(input, output, report);
            output.sort();
            ++report.dropped;
            report.complete = false;
            finish_block_without_generation();
            return report;
        }
        if (!valid_) {
            // An unconfigured generator is a bypass, not a broken unit: input
            // passes through and nothing is generated.
            passthrough(input, output, report);
            output.sort();
            return report;
        }
        if (!valid_block(block) || !utility_detail::ready(output)) {
            passthrough(input, output, report);
            output.sort();
            report.complete = false;
            return report;
        }

        current_block_ = &block;
        update_anchor(block);
        steps_this_block_ = 0;
        const bool discontinuity = detects_discontinuity(block);
        if (discontinuity) {
            for (auto& clock : clocks_)
                clock = {};
            if (!release_all(output, report, 0)) {
                passthrough(input, output, report);
                output.sort();
                finish_block(block);
                return report;
            }
        }

        const bool should_play =
            block.playing && block.transport_event != StepPlayerTransportEvent::Stopped;
        if (!should_play) {
            if (!release_all(output, report, 0)) {
                passthrough(input, output, report);
                output.sort();
                finish_block(block);
                return report;
            }
            for (auto& clock : clocks_)
                clock = {};
        } else {
            align_clocks(block.tick_start);
        }

        for (const auto& event : input) {
            const auto offset = event.sample_offset;
            if (offset < 0 || offset >= block.sample_count) {
                ++report.dropped;
                report.complete = false;
                continue;
            }
            if (!render_until(offset, false, should_play, output, report)) {
                finish_block(block);
                return report;
            }
            if (!utility_detail::emit(output, event, report)) {
                finish_block(block);
                return report;
            }
        }

        if (!render_until(block.sample_count, false, should_play, output, report)) {
            finish_block(block);
            return report;
        }
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        finish_block(block);
        return report;
    }

    /// Release every owned note and resync lane clocks. Use this for a
    /// transport stop, seek, or loop boundary handled outside process().
    MidiUtilityProcessReport flush_transport(MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, 0, sounding_note_count(), false};
        release_all(output, report, 0);
        for (auto& clock : clocks_)
            clock = {};
        output.sort();
        return report;
    }

    /// Release every owned note and discard all playback state.
    MidiUtilityProcessReport reset(MidiBuffer& output) noexcept {
        auto report = flush_transport(output);
        for (auto& note : sounding_)
            note = {};
        last_block_valid_ = false;
        return report;
    }

    /// The coordinate a lane's grid selects at `tick`.
    ///
    /// Pure in the lane's authored spec and the tick: it consults no playback
    /// state and advances nothing, so a caller may ask ahead of `process()`
    /// which cell is about to speak. `std::nullopt` means the lane does not
    /// exist or carries no steps.
    std::optional<StepPlayerCoordinate> coordinate_at(std::size_t lane,
                                                      timebase::TickPosition tick) const noexcept {
        if (!valid_ || lane >= spec_.lane_count || spec_.lanes[lane].step_count == 0 ||
            spec_.lanes[lane].step_duration.value <= 0)
            return std::nullopt;
        return coordinate_for(spec_.lanes[lane], lane, tick);
    }

    /// The coordinate a lane will fire next, or `std::nullopt` before the
    /// lane's clock has been placed by a transport block.
    std::optional<StepPlayerCoordinate> upcoming_coordinate(std::size_t lane) const noexcept {
        if (!valid_ || lane >= spec_.lane_count || spec_.lanes[lane].step_count == 0 ||
            spec_.lanes[lane].step_duration.value <= 0 || !clocks_[lane].initialized)
            return std::nullopt;
        return coordinate_for(spec_.lanes[lane], lane, clocks_[lane].next_tick);
    }

  private:
    struct LaneClock {
        timebase::TickPosition next_tick{};
        // Hits of the in-flight step already emitted. A ratchet burst can span
        // a block or render boundary; the remaining hits are re-projected from
        // the step's tick, never carried as sample state.
        std::size_t hits_done = 0;
        bool initialized = false;
    };

    struct SoundingNote {
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        std::uint8_t lane = 0;
        std::int64_t release_sample = 0;
        bool active = false;
    };

    static bool valid_block(const StepPlayerBlock& block) noexcept {
        if (block.sample_count < 0)
            return false;
        if (block.tempo_map != nullptr)
            return block.tempo_map->sample_rate().valid();
        return block.sample_rate.valid() && timebase::valid_tempo(block.tempo_bpm);
    }

    static bool ordered(const MidiBuffer& input) noexcept {
        std::int32_t previous = std::numeric_limits<std::int32_t>::min();
        for (const auto& event : input) {
            if (event.sample_offset < previous)
                return false;
            previous = event.sample_offset;
        }
        return true;
    }

    void passthrough(const MidiBuffer& input, MidiBuffer& output,
                     MidiUtilityProcessReport& report) noexcept {
        for (const auto& event : input)
            if (!utility_detail::emit(output, event, report))
                return;
    }

    void finish_block_without_generation() noexcept {
        last_block_valid_ = false;
    }

    bool detects_discontinuity(const StepPlayerBlock& block) const noexcept {
        if (block.transport_event != StepPlayerTransportEvent::Continuous)
            return true;
        if (!last_block_valid_)
            return false;
        if (block.sample_start.value != last_sample_end_ || block.playing != last_playing_)
            return true;
        return step_player_detail::distance_exceeds(block.tick_start.value, projected_tick_end_,
                                                    2);
    }

    void finish_block(const StepPlayerBlock& block) noexcept {
        last_sample_end_ =
            utility_detail::saturating_sample_add(block.sample_start.value, block.sample_count);
        if (block.tempo_map != nullptr) {
            const auto anchor = block.tempo_map->fractional_ticks_to_samples(
                static_cast<long double>(block.tick_start.value));
            projected_tick_end_ = step_player_detail::saturating_round(
                block.tempo_map->fractional_samples_to_ticks(anchor +
                                                             static_cast<long double>(
                                                                 block.sample_count)));
        } else {
            const auto ticks = static_cast<long double>(block.sample_count) *
                               static_cast<long double>(block.tempo_bpm) *
                               static_cast<long double>(timebase::kTicksPerQuarter) /
                               (60.0L * block.sample_rate.as_long_double());
            projected_tick_end_ = utility_detail::saturating_sample_add(
                block.tick_start.value, step_player_detail::saturating_round(ticks));
        }
        last_playing_ = block.playing;
        last_block_valid_ = true;
        current_block_ = nullptr;
    }

    void align_clocks(timebase::TickPosition tick) noexcept {
        for (std::size_t lane = 0; lane < spec_.lane_count; ++lane) {
            auto& clock = clocks_[lane];
            if (clock.initialized || !spec_.lanes[lane].enabled)
                continue;
            const auto duration = spec_.lanes[lane].step_duration.value;
            auto ordinal = step_player_detail::floor_div(tick.value, duration);
            auto candidate = step_player_detail::saturating_multiply(ordinal, duration);
            if (candidate < tick.value && ordinal != std::numeric_limits<std::int64_t>::max())
                candidate = step_player_detail::saturating_multiply(ordinal + 1, duration);
            clock.next_tick = {candidate};
            clock.initialized = true;
        }
    }

    void advance_clock(std::size_t lane) noexcept {
        auto& clock = clocks_[lane];
        clock.next_tick = {utility_detail::saturating_sample_add(
            clock.next_tick.value, spec_.lanes[lane].step_duration.value)};
        clock.hits_done = 0;
    }

    std::int64_t sample_for_tick(timebase::TickPosition tick) const noexcept {
        return saturating_sample_of(static_cast<long double>(anchor_sample_) +
                                    (tick_axis(*current_block_, tick.value) - anchor_axis_));
    }

    /// A tick's position along the transport's sample axis, in fractional
    /// samples. Only differences between two calls are meaningful; the origin
    /// is whatever the tempo map or the constant-tempo formula uses.
    long double tick_axis(const StepPlayerBlock& block, std::int64_t tick) const noexcept {
        if (block.tempo_map != nullptr)
            return block.tempo_map->fractional_ticks_to_samples(static_cast<long double>(tick));
        return step_player_detail::difference_as_long_double(tick, 0) *
               block.sample_rate.as_long_double() * 60.0L /
               (static_cast<long double>(block.tempo_bpm) *
                static_cast<long double>(timebase::kTicksPerQuarter));
    }

    /// Pins tick→sample projection to one point per transport segment.
    ///
    /// A block's `tick_start` does not name the instant the block begins. A
    /// tick is finer than a sample, so a whole run of ticks maps to each
    /// sample and a host reports the first of them; the reported start sits up
    /// to half a sample early, by an amount that depends on the sample it
    /// began on. Projecting relative to it folds that offset into every onset
    /// the block carries, which is enough to hand the same event two different
    /// samples under two callback partitions. Holding the anchor still across
    /// a segment makes the projection depend only on the tick. A block whose
    /// own sample/tick pair no longer sits on the anchored correspondence is a
    /// reposition, and re-anchors: a seek is free to put the tick timeline
    /// anywhere against the running sample clock.
    void update_anchor(const StepPlayerBlock& block) noexcept {
        const auto axis = tick_axis(block, block.tick_start.value);
        const auto offset = static_cast<long double>(block.sample_start.value) - axis;
        if (anchored_ &&
            std::fabs(offset - (static_cast<long double>(anchor_sample_) - anchor_axis_)) <= 0.5L)
            return;
        anchor_sample_ = block.sample_start.value;
        anchor_axis_ = axis;
        anchored_ = true;
    }

    static std::int64_t saturating_sample_of(long double absolute) noexcept {
        constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
        constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
        if (absolute <= minimum)
            return std::numeric_limits<std::int64_t>::min();
        if (absolute >= maximum)
            return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(std::llround(absolute));
    }

    timebase::TickPosition tick_at_block_offset(std::int32_t offset) const noexcept {
        const auto& block = *current_block_;
        if (block.tempo_map != nullptr) {
            const auto anchor = block.tempo_map->fractional_ticks_to_samples(
                static_cast<long double>(block.tick_start.value));
            const auto tick = block.tempo_map->fractional_samples_to_ticks(
                anchor + static_cast<long double>(offset));
            return {step_player_detail::saturating_round(tick)};
        }
        const auto ticks = static_cast<long double>(offset) *
                           static_cast<long double>(block.tempo_bpm) *
                           static_cast<long double>(timebase::kTicksPerQuarter) /
                           (60.0L * block.sample_rate.as_long_double());
        return {utility_detail::saturating_sample_add(
            block.tick_start.value, step_player_detail::saturating_round(ticks))};
    }

    std::int32_t block_offset(std::int64_t sample) const noexcept {
        if (current_block_ == nullptr)
            return 0;
        const auto start = current_block_->sample_start.value;
        if (sample <= start)
            return 0;
        const auto maximum_offset = std::max<std::int32_t>(0, current_block_->sample_count - 1);
        const auto difference =
            static_cast<std::uint64_t>(sample) - static_cast<std::uint64_t>(start);
        if (difference >= static_cast<std::uint64_t>(maximum_offset))
            return maximum_offset;
        return static_cast<std::int32_t>(difference);
    }

    bool render_until(std::int32_t offset, bool include_step_at_offset, bool playing,
                      MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        const auto absolute_limit =
            utility_detail::saturating_sample_add(current_block_->sample_start.value, offset);
        const bool limit_is_inside_block = offset < current_block_->sample_count;
        if (!playing)
            return release_due(absolute_limit, limit_is_inside_block, output, report);

        // A lane whose remaining burst hits land past the render limit is
        // parked for this call only; other lanes with due steps still fire.
        std::uint32_t parked = 0;
        for (;;) {
            // Steps fire in sample order across all lanes; ties resolve to the
            // lowest lane so the stream is deterministic.
            std::size_t best_lane = spec_.lane_count;
            std::int64_t best_sample = std::numeric_limits<std::int64_t>::max();
            for (std::size_t lane = 0; lane < spec_.lane_count; ++lane) {
                if (!clocks_[lane].initialized || !spec_.lanes[lane].enabled ||
                    (parked & (std::uint32_t{1} << lane)) != 0)
                    continue;
                const auto sample = next_onset_sample(lane);
                if (best_lane == spec_.lane_count || sample < best_sample) {
                    best_lane = lane;
                    best_sample = sample;
                }
            }
            if (best_lane == spec_.lane_count)
                break;
            const bool due =
                include_step_at_offset ? best_sample <= absolute_limit : best_sample < absolute_limit;
            if (!due)
                break;
            // A step boundary behind the block start is stale, unless the step
            // is mid-burst: then its onset legitimately predates this block
            // and only its remaining hits matter.
            if (best_sample < current_block_->sample_start.value &&
                clocks_[best_lane].hits_done == 0) {
                advance_clock(best_lane);
                report.complete = false;
                ++report.dropped;
                continue;
            }
            if (steps_this_block_ == MaximumStepsPerBlock) {
                report.complete = false;
                ++report.dropped;
                realign_clocks_past(offset);
                break;
            }
            const auto hits_before = clocks_[best_lane].hits_done;
            if (!fire_step(best_lane, best_sample, absolute_limit, output, report))
                return false;
            if (clocks_[best_lane].hits_done == 0) {
                advance_clock(best_lane);
                ++steps_this_block_;
            } else if (clocks_[best_lane].hits_done == hits_before) {
                // The burst's remaining hits land past the render limit; the
                // step stays armed and resumes at the next render call.
                parked |= std::uint32_t{1} << best_lane;
            }
        }
        return release_due(absolute_limit, limit_is_inside_block, output, report);
    }

    void realign_clocks_past(std::int32_t offset) noexcept {
        const auto tick = tick_at_block_offset(offset);
        for (std::size_t lane = 0; lane < spec_.lane_count; ++lane) {
            auto& clock = clocks_[lane];
            if (!clock.initialized)
                continue;
            const auto duration = spec_.lanes[lane].step_duration.value;
            auto ordinal = step_player_detail::floor_div(tick.value, duration);
            auto candidate = step_player_detail::saturating_multiply(ordinal, duration);
            if (candidate <= tick.value && ordinal != std::numeric_limits<std::int64_t>::max())
                candidate = step_player_detail::saturating_multiply(ordinal + 1, duration);
            clock.next_tick = {candidate};
            clock.hits_done = 0;
        }
    }

    std::size_t walk_index(const StepPlayerLaneSpec& lane, std::int64_t ordinal,
                           timebase::TickPosition tick, std::size_t lane_index,
                           std::uint64_t cycle) const noexcept {
        const auto length = static_cast<std::size_t>(lane.step_count);
        switch (lane.direction) {
        case StepPlayerDirection::Reverse:
            return length - 1 - step_player_detail::positive_mod(ordinal, length);
        case StepPlayerDirection::PingPong: {
            if (length == 1)
                return 0;
            const auto period = length * 2 - 2;
            const auto position = step_player_detail::positive_mod(ordinal, period);
            return position < length ? position : period - position;
        }
        case StepPlayerDirection::Random:
            // The walk is a pure function of the grid coordinate, so any block
            // partition of the same transport span selects the same step.
            return static_cast<std::size_t>(
                timebase::coordinate_random(
                    spec_.random_seed,
                    {tick, static_cast<std::uint64_t>(lane_index), cycle, 1}) %
                length);
        case StepPlayerDirection::Forward:
            return step_player_detail::positive_mod(ordinal, length);
        }
        return 0;
    }

    /// The grid coordinate a lane selects at `tick`.
    ///
    /// Sequencing, firing and the public queries all read the walk from here,
    /// so the three cannot disagree about which cell a tick names.
    StepPlayerCoordinate coordinate_for(const StepPlayerLaneSpec& lane, std::size_t lane_index,
                                        timebase::TickPosition tick) const noexcept {
        StepPlayerCoordinate coordinate{};
        coordinate.step_tick = tick;
        coordinate.following_tick = timebase::TickPosition{
            utility_detail::saturating_sample_add(tick.value, lane.step_duration.value)};
        coordinate.ordinal = step_player_detail::floor_div(tick.value, lane.step_duration.value);
        coordinate.cycle = static_cast<std::uint64_t>(step_player_detail::floor_div(
            coordinate.ordinal, static_cast<std::int64_t>(lane.step_count)));
        coordinate.index = walk_index(lane, coordinate.ordinal, tick, lane_index, coordinate.cycle);
        return coordinate;
    }

    /// How far a step asks to be displaced off its grid position, in ticks.
    ///
    /// The authored offset is held inside the step's own interval. A
    /// displacement that reached the next grid position would reorder the
    /// sequence against itself, and one that reached past this block's end
    /// would make the step's survival depend on where the host put its
    /// callback boundary. Bounding it here means sequencing and firing ask the
    /// same question and get the same answer from one place.
    std::int64_t bounded_timing_offset(const StepPlayerStep& step,
                                       std::int64_t interval_ticks) const noexcept {
        if (!step.on || step.timing_offset_ticks <= 0)
            return 0;
        return std::min<std::int64_t>(static_cast<std::int64_t>(step.timing_offset_ticks),
                                      std::max<std::int64_t>(0, interval_ticks - 1));
    }

    /// The sample a lane's next step actually speaks at, displacement included.
    ///
    /// Sequencing has to ask the displaced question rather than the grid one.
    /// A step found by its grid tick but sounding after the render limit would
    /// otherwise be advanced past without ever speaking, and whether that
    /// happened would depend on the callback partition.
    std::int64_t next_onset_sample(std::size_t lane_index) const noexcept {
        const auto& lane = spec_.lanes[lane_index];
        const auto tick = clocks_[lane_index].next_tick;
        if (lane.step_count == 0 || lane.step_duration.value <= 0)
            return sample_for_tick(tick);
        const auto coordinate = coordinate_for(lane, lane_index, tick);
        const auto offset = bounded_timing_offset(
            steps_[lane_index * MaxSteps + coordinate.index], lane.step_duration.value);
        return sample_for_tick({utility_detail::saturating_sample_add(tick.value, offset)});
    }

    bool fire_step(std::size_t lane_index, std::int64_t step_sample, std::int64_t absolute_limit,
                   MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        const auto& lane = spec_.lanes[lane_index];
        auto& clock = clocks_[lane_index];
        const auto coordinate = coordinate_for(lane, lane_index, clock.next_tick);
        const auto step_tick = coordinate.step_tick;
        const auto following_tick = coordinate.following_tick;
        const auto cycle = coordinate.cycle;
        const auto index = coordinate.index;
        const auto& step = steps_[lane_index * MaxSteps + index];
        if (!step.on)
            return true;

        if (step.probability_percent < 100) {
            if (step.probability_percent == 0)
                return true;
            const auto chance = timebase::coordinate_chance(
                spec_.random_seed,
                {step_tick, static_cast<std::uint64_t>(lane_index), cycle, 0},
                step.probability_percent, 100);
            if (!chance.has_value() || !chance.value())
                return true;
        }

        const auto pitch = static_cast<std::uint8_t>(std::clamp(
            static_cast<int>(lane.base_note) + static_cast<int>(step.pitch_offset), 0, 127));
        const auto interval_ticks = following_tick.value - step_tick.value;
        // The step's grid coordinate stays where it was authored — the walk
        // index, the cycle and the probability draw above all read it — and
        // only the moment it speaks moves. A displacement therefore cannot
        // accumulate into drift.
        const auto onset_tick = timebase::TickPosition{utility_detail::saturating_sample_add(
            step_tick.value, bounded_timing_offset(step, interval_ticks))};
        // The gate is measured from the interval the step was authored on, not
        // from what a displacement leaves of it, so a nudged step keeps the
        // length it was written with.
        const auto gate_ticks = std::max<std::int64_t>(
            1, step_player_detail::saturating_multiply(interval_ticks, step.gate_percent) / 100);
        const auto step_release_tick =
            timebase::TickPosition{utility_detail::saturating_sample_add(onset_tick.value,
                                                                         gate_ticks)};

        // Tie, slide, and choke are step-onset policies; they apply exactly
        // once, when the burst's first hit fires.
        if (clock.hits_done == 0) {
            // Tie extends a still-sounding note of the same pitch; it must win
            // over the due-release sweep below or the note would be released at
            // the boundary before we could extend it.
            if (step.tie) {
                if (auto* match = find_sounding(lane_index, lane.channel, pitch)) {
                    match->release_sample =
                        std::max(match->release_sample, sample_for_tick(step_release_tick));
                    return release_due(step_sample, true, output, report);
                }
            }

            // Slide keeps a sounding note of a different pitch ringing one tick
            // past the new attack so a downstream voice can glide between them.
            if (step.slide) {
                const auto overlap_sample =
                    std::max(step_sample + 1,
                             sample_for_tick({utility_detail::saturating_sample_add(
                                 onset_tick.value, 1)}));
                for (auto& sounding : sounding_)
                    if (sounding.active && sounding.lane == lane_index && sounding.note != pitch)
                        sounding.release_sample =
                            std::max(sounding.release_sample, overlap_sample);
            }

            if (lane.choke_group != 0 &&
                !emit_choke(lane_index, lane.choke_group, step_sample, output, report))
                return false;
        }

        std::array<timebase::TickPosition, MaxRatchetHits> hits{};
        std::size_t hit_count = 1;
        hits[0] = onset_tick;
        if (step.ratchet_count > 1) {
            // A burst subdivides what the displacement leaves of the interval,
            // so every hit stays inside the step that owns it however far the
            // onset was nudged.
            const auto projection = timebase::project_ratchet_interval<MaxRatchetHits>(
                onset_tick, following_tick, step.ratchet_count, onset_tick, following_tick,
                std::span<timebase::TickPosition>(hits.data(), hits.size()));
            if (projection && projection.event_count > 0)
                hit_count = projection.event_count;
            // A step interval too short to subdivide degrades to a single hit
            // rather than an error: the step still speaks.
        }
        const auto hit_gate_ticks =
            std::max<std::int64_t>(1, gate_ticks / static_cast<std::int64_t>(hit_count));

        for (std::size_t hit = clock.hits_done; hit < hit_count; ++hit) {
            const auto hit_tick = hits[hit];
            auto release_tick = timebase::TickPosition{utility_detail::saturating_sample_add(
                hit_tick.value, hit_gate_ticks)};
            if (hit + 1 < hit_count) {
                const auto next_tick = hits[hit + 1].value;
                if (next_tick <= release_tick.value)
                    release_tick = {std::max(next_tick - 1, hit_tick.value)};
            }
            const auto hit_sample = sample_for_tick(hit_tick);
            // A hit past the render limit belongs to a later render call or
            // block; it is re-projected from its tick, never clamped into this
            // block.
            if (hit_sample >= absolute_limit)
                break;
            if (!release_due(hit_sample, true, output, report))
                return false;
            auto* slot = free_sounding_slot();
            if (slot == nullptr) {
                // Drop the newest attack, never its release: audible thinning
                // under an exhausted ledger beats a stuck note.
                ++report.dropped;
                report.complete = false;
                continue;
            }
            auto attack = MidiEvent::note_on(lane.channel, pitch, step.velocity);
            attack.sample_offset = block_offset(hit_sample);
            if (!utility_detail::emit(output, attack, report))
                return false;
            *slot = {lane.channel,
                     pitch,
                     static_cast<std::uint8_t>(lane_index),
                     sample_for_tick(release_tick),
                     true};
            clock.hits_done = hit + 1;
        }
        if (clock.hits_done == hit_count)
            clock.hits_done = 0;
        return true;
    }

    SoundingNote* find_sounding(std::size_t lane, std::uint8_t channel, std::uint8_t note) noexcept {
        for (auto& sounding : sounding_)
            if (sounding.active && sounding.lane == lane && sounding.channel == channel &&
                sounding.note == note)
                return &sounding;
        return nullptr;
    }

    SoundingNote* free_sounding_slot() noexcept {
        for (auto& note : sounding_)
            if (!note.active)
                return &note;
        return nullptr;
    }

    bool emit_choke(std::size_t firing_lane, std::uint8_t group, std::int64_t sample,
                    MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        for (auto& sounding : sounding_) {
            if (!sounding.active || sounding.lane == firing_lane)
                continue;
            if (spec_.lanes[sounding.lane].choke_group != group)
                continue;
            if (!emit_release(sounding, sample, output, report))
                return false;
        }
        return true;
    }

    bool release_due(std::int64_t sample, bool include_at_sample, MidiBuffer& output,
                     MidiUtilityProcessReport& report) noexcept {
        for (auto& sounding : sounding_) {
            const bool due = sounding.release_sample < sample ||
                             (include_at_sample && sounding.release_sample == sample);
            if (sounding.active && due &&
                !emit_release(sounding, sounding.release_sample, output, report))
                return false;
        }
        return true;
    }

    bool release_all(MidiBuffer& output, MidiUtilityProcessReport& report,
                     std::int32_t offset) noexcept {
        const auto sample =
            current_block_ != nullptr
                ? utility_detail::saturating_sample_add(current_block_->sample_start.value, offset)
                : 0;
        for (auto& sounding : sounding_)
            if (sounding.active && !emit_release(sounding, sample, output, report))
                return false;
        return true;
    }

    bool emit_release(SoundingNote& sounding, std::int64_t sample, MidiBuffer& output,
                      MidiUtilityProcessReport& report) noexcept {
        auto release = MidiEvent::note_off(sounding.channel, sounding.note);
        release.sample_offset = block_offset(sample);
        if (!utility_detail::emit(output, release, report)) {
            sounding.release_sample = sample;
            ++report.deferred;
            return false;
        }
        sounding = {};
        return true;
    }

    Spec spec_{};
    bool valid_ = true;
    std::array<StepPlayerStep, kMaximumCells> steps_{};
    std::array<LaneClock, MaxLanes> clocks_{};
    std::array<SoundingNote, kMaximumSoundingNotes> sounding_{};
    const StepPlayerBlock* current_block_ = nullptr;
    bool anchored_ = false;
    std::int64_t anchor_sample_ = 0;
    long double anchor_axis_ = 0.0L;
    std::int64_t last_sample_end_ = 0;
    std::int64_t projected_tick_end_ = 0;
    bool last_playing_ = false;
    bool last_block_valid_ = false;
    std::size_t steps_this_block_ = 0;
};

} // namespace pulp::midi
