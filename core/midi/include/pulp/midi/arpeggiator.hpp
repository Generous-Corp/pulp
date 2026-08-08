#pragma once

#include <pulp/midi/detail/arpeggiator_math.hpp>
#include <pulp/midi/utility_contract.hpp>
#include <pulp/music/pitch.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/quantize.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace pulp::midi {

enum class ArpeggiatorOrder : std::uint8_t {
    Up,
    Down,
    UpDown,
    AsPlayed,
    Random,
    Chord,
};

enum class ArpeggiatorRepeatedNotePolicy : std::uint8_t {
    Retrigger,
    Tie,
};

enum class ArpeggiatorBoundaryOrder : std::uint8_t {
    InputBeforeStep,
    StepBeforeInput,
};

enum class ArpeggiatorTransportEvent : std::uint8_t {
    Continuous,
    Started,
    Stopped,
    Seeked,
    LoopWrapped,
};

struct ArpeggiatorGate {
    std::uint32_t numerator = 1;
    std::uint32_t denominator = 2;
    constexpr auto operator<=>(const ArpeggiatorGate&) const = default;
};

struct ArpeggiatorSpec {
    timebase::TickDuration rate{timebase::kTicksPerQuarter / 4};
    ArpeggiatorOrder order = ArpeggiatorOrder::Up;
    std::uint8_t octave_count = 1;
    ArpeggiatorGate gate{};
    timebase::SwingRatio swing = timebase::kStraightSwing;
    ArpeggiatorRepeatedNotePolicy repeated_note = ArpeggiatorRepeatedNotePolicy::Retrigger;
    ArpeggiatorBoundaryOrder boundary_order = ArpeggiatorBoundaryOrder::InputBeforeStep;
    bool latch = false;
    std::uint64_t random_seed = 0;
    constexpr auto operator<=>(const ArpeggiatorSpec&) const = default;
};

/// Block timing for one sample-accurate arpeggiator call.
///
/// `tick_start` and `sample_start` describe the same block boundary. A compiled
/// tempo map gives the exact tick-to-sample projection used by Pulp's sequencer.
/// Host-facing plugins can leave it null and provide the block's constant tempo
/// and sample rate instead. `transport_event` is explicit because a loop wrap
/// can keep the host sample clock monotonic while its musical clock jumps.
struct ArpeggiatorBlock {
    timebase::SamplePosition sample_start{};
    timebase::TickPosition tick_start{};
    std::int32_t sample_count = 0;
    timebase::RationalRate sample_rate{};
    double tempo_bpm = 120.0;
    const timebase::CompiledTempoMap* tempo_map = nullptr;
    bool playing = true;
    ArpeggiatorTransportEvent transport_event = ArpeggiatorTransportEvent::Continuous;
};

struct ArpeggiatorCapacityContract {
    std::size_t maximum_held_notes = 0;
    std::size_t maximum_pattern_notes = 0;
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

namespace arpeggiator_detail {

constexpr bool valid_order(ArpeggiatorOrder order) noexcept {
    return order >= ArpeggiatorOrder::Up && order <= ArpeggiatorOrder::Chord;
}

constexpr bool valid_repeated_note_policy(ArpeggiatorRepeatedNotePolicy policy) noexcept {
    return policy >= ArpeggiatorRepeatedNotePolicy::Retrigger &&
           policy <= ArpeggiatorRepeatedNotePolicy::Tie;
}

constexpr bool valid_boundary_order(ArpeggiatorBoundaryOrder order) noexcept {
    return order >= ArpeggiatorBoundaryOrder::InputBeforeStep &&
           order <= ArpeggiatorBoundaryOrder::StepBeforeInput;
}

} // namespace arpeggiator_detail

/// Fixed-capacity, sample-accurate MIDI arpeggiator.
///
/// Short-message note input is consumed; non-note short messages, SysEx, and
/// attached UMP packets pass through unchanged. Every accepted attack owns
/// exactly one later release, including repeated attacks of the same key. An
/// attack that cannot enter the fixed ledger is suppressed, and its matching
/// release is consumed before an older accepted ownership can be retired.
/// Generated note releases are retained as debt when the caller's reserved
/// output capacity is exhausted.
template <std::size_t MaximumHeldNotes = 32, std::size_t MaximumOctaves = 4,
          std::size_t MaximumStepsPerBlock = 64, std::size_t MaximumOwnershipRuns = 256>
class Arpeggiator {
  public:
    static_assert(MaximumHeldNotes > 0);
    static_assert(MaximumOctaves > 0);
    static_assert(MaximumStepsPerBlock > 0);
    static_assert(MaximumOwnershipRuns >= MaximumHeldNotes);
    static_assert(MaximumHeldNotes <= std::numeric_limits<std::size_t>::max() / MaximumOctaves);

    static constexpr std::size_t kMaximumPatternNotes = MaximumHeldNotes * MaximumOctaves;
    static_assert(MaximumStepsPerBlock <=
                  (std::numeric_limits<std::size_t>::max() / kMaximumPatternNotes - 1) / 2);

    static constexpr ArpeggiatorCapacityContract contract() noexcept {
        constexpr auto generated = (MaximumStepsPerBlock * 2 + 1) * kMaximumPatternNotes;
        return {MaximumHeldNotes,
                kMaximumPatternNotes,
                MaximumStepsPerBlock,
                generated,
                MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity,
                true,
                true};
    }

    static constexpr bool valid_spec(const ArpeggiatorSpec& spec) noexcept {
        constexpr auto kMaximumRate = timebase::kTicksPerQuarter * 16;
        return spec.rate.value > 0 && spec.rate.value <= kMaximumRate && spec.octave_count > 0 &&
               spec.octave_count <= MaximumOctaves && spec.gate.numerator > 0 &&
               spec.gate.denominator > 0 && spec.gate.numerator <= spec.gate.denominator &&
               spec.gate.denominator <= 1'000'000 && timebase::valid_swing_grid(spec.rate) &&
               timebase::valid_swing_ratio(spec.swing) &&
               arpeggiator_detail::valid_order(spec.order) &&
               arpeggiator_detail::valid_repeated_note_policy(spec.repeated_note) &&
               arpeggiator_detail::valid_boundary_order(spec.boundary_order);
    }

    explicit constexpr Arpeggiator(ArpeggiatorSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }
    constexpr const ArpeggiatorSpec& spec() const noexcept {
        return spec_;
    }

    std::size_t held_note_count() const noexcept {
        std::size_t count = 0;
        for (const auto& note : held_)
            if (note.active)
                ++count;
        return count;
    }

    std::size_t physical_note_count() const noexcept {
        return physical_count_;
    }
    std::size_t sounding_note_count() const noexcept {
        std::size_t count = 0;
        for (const auto& note : sounding_)
            if (note.active)
                ++count;
        return count;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output,
                                     const ArpeggiatorBlock& block) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!valid_ || !valid_block(block) || !ordered(input) || !utility_detail::ready(output)) {
            retain_unprocessed_input(input);
            return {0, input.size(), 0, false};
        }

        current_block_ = &block;
        steps_this_block_ = 0;
        const bool discontinuity = detects_discontinuity(block);
        if (discontinuity) {
            clock_initialized_ = false;
            if (!release_all(output, report, 0)) {
                retain_unprocessed_input(input);
                finish_block(block);
                return report;
            }
        }
        if (!drain_pending_spec(output, report)) {
            retain_unprocessed_input(input);
            finish_block(block);
            return report;
        }

        const bool should_play =
            block.playing && block.transport_event != ArpeggiatorTransportEvent::Stopped;
        if (!should_play) {
            if (!release_all(output, report, 0)) {
                retain_unprocessed_input(input);
                finish_block(block);
                return report;
            }
            clock_initialized_ = false;
        } else if (!clock_initialized_) {
            align_clock(block.tick_start);
        }

        std::size_t input_index = 0;
        while (input_index < input.size()) {
            const auto offset = input[input_index].sample_offset;
            if (offset < 0 || offset >= block.sample_count) {
                ++report.dropped;
                report.complete = false;
                retain_input_event(input[input_index]);
                ++input_index;
                continue;
            }
            if (!render_until(offset,
                              spec_.boundary_order == ArpeggiatorBoundaryOrder::StepBeforeInput,
                              should_play, output, report)) {
                retain_unprocessed_input(input, input_index);
                finish_block(block);
                return report;
            }
            const auto group_end = equal_offset_end(input, input_index, offset);
            for (; input_index < group_end; ++input_index) {
                if (!consume_input_event(input[input_index], output, report)) {
                    retain_unprocessed_input(input, input_index + 1);
                    finish_block(block);
                    return report;
                }
            }
            if (should_play && spec_.boundary_order == ArpeggiatorBoundaryOrder::InputBeforeStep &&
                !render_until(offset, true, true, output, report)) {
                retain_unprocessed_input(input, input_index);
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

    /// Release generated notes and reset their clock while preserving the held
    /// input ledger. Use this for a transport stop, seek, or loop boundary when
    /// the caller handles transitions outside process().
    MidiUtilityProcessReport flush_transport(MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output))
            return {0, 0, sounding_note_count(), false};
        release_all(output, report, 0);
        clock_initialized_ = false;
        output.sort();
        return report;
    }

    /// Release generated notes and discard every held/latch ownership.
    MidiUtilityProcessReport reset(MidiBuffer& output) noexcept {
        auto report = flush_transport(output);
        clear_held_state();
        last_block_valid_ = false;
        pending_spec_.reset();
        return report;
    }

    /// Flush old generated notes before adopting a new policy. Held physical
    /// notes survive the swap so the next grid boundary can use the new spec;
    /// latch-only notes are discarded when latch is disabled.
    MidiUtilityProcessReport replace_spec(const ArpeggiatorSpec& spec,
                                          MidiBuffer& output) noexcept {
        utility_detail::clear_output(output);
        if (!valid_spec(spec))
            return {0, 0, 0, false};
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            pending_spec_ = spec;
            return {0, 0, sounding_note_count(), false};
        }
        if (!release_all(output, report, 0)) {
            pending_spec_ = spec;
            return report;
        }
        apply_spec(spec);
        output.sort();
        return report;
    }

  private:
    struct HeldNote {
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint64_t serial = 0;
        bool physical = false;
        bool active = false;
    };

    struct PatternNote {
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint64_t serial = 0;
    };

    struct SoundingNote {
        std::uint8_t channel = 0;
        std::uint8_t note = 0;
        std::int64_t release_sample = 0;
        bool active = false;
    };

    struct OwnershipRun {
        std::uint16_t key = 0;
        std::uint32_t count = 0;
        bool accepted = false;
    };

    static constexpr bool is_attack(const MidiEvent& event) noexcept {
        return event.is_note_on() && event.velocity() != 0;
    }

    static constexpr bool is_release(const MidiEvent& event) noexcept {
        return event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
    }

    static bool valid_block(const ArpeggiatorBlock& block) noexcept {
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

    static std::size_t equal_offset_end(const MidiBuffer& input, std::size_t begin,
                                        std::int32_t offset) noexcept {
        auto end = begin;
        while (end < input.size() && input[end].sample_offset == offset)
            ++end;
        return end;
    }

    bool detects_discontinuity(const ArpeggiatorBlock& block) const noexcept {
        if (block.transport_event != ArpeggiatorTransportEvent::Continuous)
            return true;
        if (!last_block_valid_)
            return false;
        if (block.sample_start.value != last_sample_end_ || block.playing != last_playing_)
            return true;
        return arpeggiator_detail::distance_exceeds(block.tick_start.value, projected_tick_end_, 2);
    }

    void finish_block(const ArpeggiatorBlock& block) noexcept {
        last_sample_end_ =
            utility_detail::saturating_sample_add(block.sample_start.value, block.sample_count);
        if (block.tempo_map != nullptr) {
            const auto anchor = block.tempo_map->fractional_ticks_to_samples(
                static_cast<long double>(block.tick_start.value));
            projected_tick_end_ =
                arpeggiator_detail::saturating_round(block.tempo_map->fractional_samples_to_ticks(
                    anchor + static_cast<long double>(block.sample_count)));
        } else {
            const auto ticks = static_cast<long double>(block.sample_count) *
                               static_cast<long double>(block.tempo_bpm) *
                               static_cast<long double>(timebase::kTicksPerQuarter) /
                               (60.0L * block.sample_rate.as_long_double());
            projected_tick_end_ = utility_detail::saturating_sample_add(
                block.tick_start.value, arpeggiator_detail::saturating_round(ticks));
        }
        last_playing_ = block.playing;
        last_block_valid_ = true;
        current_block_ = nullptr;
    }

    void align_clock(timebase::TickPosition tick) noexcept {
        const auto step = spec_.rate.value;
        const auto quotient = arpeggiator_detail::floor_div(tick.value, step);
        auto index = quotient == std::numeric_limits<std::int64_t>::min() ? quotient : quotient - 1;
        for (int attempts = 0; attempts < 4; ++attempts, ++index) {
            const auto candidate = swung_boundary(index);
            if (candidate.value >= tick.value) {
                next_step_index_ = index;
                next_step_tick_ = candidate;
                clock_initialized_ = true;
                return;
            }
        }
        next_step_index_ = index;
        next_step_tick_ = swung_boundary(index);
        clock_initialized_ = true;
    }

    timebase::TickPosition swung_boundary(std::int64_t index) const noexcept {
        const auto straight = timebase::TickPosition{
            arpeggiator_detail::saturating_multiply(index, spec_.rate.value)};
        const auto displacement = timebase::swing_displacement(straight, spec_.rate, spec_.swing);
        return {utility_detail::saturating_sample_add(straight.value, displacement.value)};
    }

    void advance_clock() noexcept {
        if (next_step_index_ != std::numeric_limits<std::int64_t>::max())
            ++next_step_index_;
        next_step_tick_ = swung_boundary(next_step_index_);
    }

    std::int64_t sample_for_tick(timebase::TickPosition tick) const noexcept {
        const auto& block = *current_block_;
        long double delta = 0.0L;
        if (block.tempo_map != nullptr) {
            const auto event_sample =
                block.tempo_map->fractional_ticks_to_samples(static_cast<long double>(tick.value));
            const auto anchor_sample = block.tempo_map->fractional_ticks_to_samples(
                static_cast<long double>(block.tick_start.value));
            delta = event_sample - anchor_sample;
        } else {
            const auto tick_delta = static_cast<long double>(tick.value) -
                                    static_cast<long double>(block.tick_start.value);
            delta = tick_delta * block.sample_rate.as_long_double() * 60.0L /
                    (static_cast<long double>(block.tempo_bpm) *
                     static_cast<long double>(timebase::kTicksPerQuarter));
        }
        constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
        constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
        const auto absolute = static_cast<long double>(block.sample_start.value) + delta;
        if (absolute <= minimum)
            return std::numeric_limits<std::int64_t>::min();
        if (absolute >= maximum)
            return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(std::llround(absolute));
    }

    bool render_until(std::int32_t offset, bool include_step_at_offset, bool playing,
                      MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        const auto absolute_limit =
            utility_detail::saturating_sample_add(current_block_->sample_start.value, offset);
        const bool limit_is_inside_block = offset < current_block_->sample_count;
        const bool full_gate_tie = spec_.repeated_note == ArpeggiatorRepeatedNotePolicy::Tie &&
                                   spec_.gate.numerator == spec_.gate.denominator;
        if (!playing)
            return release_due(absolute_limit, limit_is_inside_block, output, report);

        while (clock_initialized_) {
            const auto step_sample = sample_for_tick(next_step_tick_);
            const bool due = include_step_at_offset ? step_sample <= absolute_limit
                                                    : step_sample < absolute_limit;
            if (!due)
                break;
            if (!release_due(step_sample, !full_gate_tie, output, report))
                return false;
            if (step_sample < current_block_->sample_start.value) {
                advance_clock();
                report.complete = false;
                ++report.dropped;
                continue;
            }
            if (steps_this_block_ == MaximumStepsPerBlock) {
                report.complete = false;
                ++report.dropped;
                const auto tick = tick_at_block_offset(offset);
                align_clock({utility_detail::saturating_sample_add(tick.value, 1)});
                break;
            }
            const auto following_tick =
                swung_boundary(next_step_index_ == std::numeric_limits<std::int64_t>::max()
                                   ? next_step_index_
                                   : next_step_index_ + 1);
            if (!fire_step(step_sample, next_step_tick_, following_tick, output, report))
                return false;
            advance_clock();
            ++steps_this_block_;
        }
        const bool step_at_limit =
            clock_initialized_ && sample_for_tick(next_step_tick_) == absolute_limit;
        const bool include_releases_at_limit =
            limit_is_inside_block && !(full_gate_tie && step_at_limit);
        return release_due(absolute_limit, include_releases_at_limit, output, report);
    }

    timebase::TickPosition tick_at_block_offset(std::int32_t offset) const noexcept {
        const auto& block = *current_block_;
        if (block.tempo_map != nullptr) {
            const auto anchor = block.tempo_map->fractional_ticks_to_samples(
                static_cast<long double>(block.tick_start.value));
            const auto tick = block.tempo_map->fractional_samples_to_ticks(
                anchor + static_cast<long double>(offset));
            return {arpeggiator_detail::saturating_round(tick)};
        }
        const auto ticks = static_cast<long double>(offset) *
                           static_cast<long double>(block.tempo_bpm) *
                           static_cast<long double>(timebase::kTicksPerQuarter) /
                           (60.0L * block.sample_rate.as_long_double());
        return {utility_detail::saturating_sample_add(block.tick_start.value,
                                                      arpeggiator_detail::saturating_round(ticks))};
    }

    bool consume_input_event(const MidiEvent& event, MidiBuffer& output,
                             MidiUtilityProcessReport& report) noexcept {
        if (is_attack(event)) {
            const bool starts_latched_phrase =
                spec_.latch && physical_count_ == 0 && held_note_count() != 0;
            if (starts_latched_phrase) {
                const bool releases_complete = release_all(output, report, event.sample_offset);
                clear_latched_notes();
                retain_attack(event, report);
                return releases_complete;
            }
            retain_attack(event, report);
            return true;
        }
        if (is_release(event)) {
            consume_release(event.channel(), event.note());
            if (!spec_.latch && held_note_count() == 0)
                return release_all(output, report, event.sample_offset);
            return true;
        }
        return utility_detail::emit(output, event, report);
    }

    void retain_input_event(const MidiEvent& event) noexcept {
        if (is_attack(event)) {
            const auto key = static_cast<std::uint16_t>(
                utility_detail::key_index(event.channel(), event.note()));
            record_ownership(key, false);
        } else if (is_release(event)) {
            consume_release(event.channel(), event.note());
        }
    }

    void retain_attack(const MidiEvent& event, MidiUtilityProcessReport& report) noexcept {
        const auto key =
            static_cast<std::uint16_t>(utility_detail::key_index(event.channel(), event.note()));
        const auto slot = free_held_slot();
        const bool accepted = slot != nullptr && can_record_ownership(key, true);
        if (!record_ownership(key, accepted) || !accepted) {
            ++report.dropped;
            report.complete = false;
            return;
        }
        if (next_serial_ == std::numeric_limits<std::uint64_t>::max())
            rebase_serials();
        *slot = {event.channel(), event.note(), event.velocity(), next_serial_++, true, true};
        ++physical_count_;
    }

    void retain_unprocessed_input(const MidiBuffer& input, std::size_t begin = 0) noexcept {
        for (std::size_t index = begin; index < input.size(); ++index)
            retain_input_event(input[index]);
    }

    HeldNote* free_held_slot() noexcept {
        for (auto& note : held_)
            if (!note.active)
                return &note;
        return nullptr;
    }

    bool can_record_ownership(std::uint16_t key, bool accepted) const noexcept {
        if (overflow_suppressed_[key] != 0)
            return !accepted;
        if (ownership_size_ < ownership_.size())
            return true;
        return !accepted;
    }

    bool record_ownership(std::uint16_t key, bool accepted) noexcept {
        if (overflow_suppressed_[key] != 0 || ownership_size_ == ownership_.size()) {
            if (accepted || overflow_suppressed_[key] == std::numeric_limits<std::uint32_t>::max())
                return false;
            ++overflow_suppressed_[key];
            return true;
        }
        for (std::size_t index = ownership_size_; index != 0; --index) {
            auto& previous = ownership_[index - 1];
            if (previous.key != key)
                continue;
            if (previous.accepted == accepted &&
                previous.count != std::numeric_limits<std::uint32_t>::max()) {
                ++previous.count;
                return true;
            }
            break;
        }
        ownership_[ownership_size_++] = {key, 1, accepted};
        return true;
    }

    void consume_release(std::uint8_t channel, std::uint8_t note) noexcept {
        const auto key = static_cast<std::uint16_t>(utility_detail::key_index(channel, note));
        for (std::size_t index = 0; index < ownership_size_; ++index) {
            auto& run = ownership_[index];
            if (run.key != key)
                continue;
            const bool accepted = run.accepted;
            if (--run.count == 0)
                erase_ownership(index);
            if (accepted)
                release_oldest_physical(key);
            return;
        }
        if (overflow_suppressed_[key] != 0)
            --overflow_suppressed_[key];
    }

    void release_oldest_physical(std::uint16_t key) noexcept {
        HeldNote* oldest = nullptr;
        for (auto& note : held_) {
            if (!note.active || !note.physical ||
                utility_detail::key_index(note.channel, note.note) != key)
                continue;
            if (oldest == nullptr || note.serial < oldest->serial)
                oldest = &note;
        }
        if (oldest == nullptr)
            return;
        oldest->physical = false;
        --physical_count_;
        if (!spec_.latch)
            *oldest = {};
    }

    void erase_ownership(std::size_t index) noexcept {
        for (std::size_t next = index + 1; next < ownership_size_; ++next)
            ownership_[next - 1] = ownership_[next];
        --ownership_size_;
    }

    void clear_latched_notes() noexcept {
        for (auto& note : held_)
            if (note.active && !note.physical)
                note = {};
    }

    void clear_held_state() noexcept {
        held_.fill({});
        ownership_.fill({});
        overflow_suppressed_.fill(0);
        ownership_size_ = 0;
        physical_count_ = 0;
    }

    void rebase_serials() noexcept {
        std::array<std::size_t, MaximumHeldNotes> indices{};
        std::size_t size = 0;
        for (std::size_t index = 0; index < held_.size(); ++index)
            if (held_[index].active)
                indices[size++] = index;
        std::sort(indices.begin(), indices.begin() + size, [&](std::size_t lhs, std::size_t rhs) {
            return held_[lhs].serial < held_[rhs].serial;
        });
        for (std::size_t serial = 0; serial < size; ++serial)
            held_[indices[serial]].serial = serial;
        next_serial_ = size;
    }

    std::size_t build_pattern() noexcept {
        std::array<std::size_t, MaximumHeldNotes> held_indices{};
        std::size_t held_size = 0;
        for (std::size_t index = 0; index < held_.size(); ++index)
            if (held_[index].active)
                held_indices[held_size++] = index;
        std::sort(held_indices.begin(), held_indices.begin() + held_size,
                  [&](std::size_t lhs, std::size_t rhs) {
                      return held_[lhs].serial < held_[rhs].serial;
                  });
        std::size_t size = 0;
        for (std::size_t octave = 0; octave < spec_.octave_count; ++octave) {
            for (std::size_t held_index = 0; held_index < held_size; ++held_index) {
                const auto& held = held_[held_indices[held_index]];
                const auto pitch = static_cast<int>(held.note) +
                                   static_cast<int>(octave) * music::kPitchClassesPerOctave;
                if (pitch > 127)
                    continue;
                const auto order_serial = static_cast<std::uint64_t>(size);
                pattern_[size++] = {held.channel, static_cast<std::uint8_t>(pitch), held.velocity,
                                    order_serial};
            }
        }
        if (spec_.order != ArpeggiatorOrder::AsPlayed) {
            std::sort(pattern_.begin(), pattern_.begin() + size,
                      [](const PatternNote& lhs, const PatternNote& rhs) {
                          if (lhs.note != rhs.note)
                              return lhs.note < rhs.note;
                          if (lhs.channel != rhs.channel)
                              return lhs.channel < rhs.channel;
                          return lhs.serial < rhs.serial;
                      });
        }
        return size;
    }

    std::size_t selected_pattern_index(std::size_t size) const noexcept {
        const auto positive_mod = [size](std::int64_t value) {
            auto result = value % static_cast<std::int64_t>(size);
            if (result < 0)
                result += static_cast<std::int64_t>(size);
            return static_cast<std::size_t>(result);
        };
        switch (spec_.order) {
        case ArpeggiatorOrder::Down:
            return size - 1 - positive_mod(next_step_index_);
        case ArpeggiatorOrder::UpDown: {
            if (size == 1)
                return 0;
            const auto period = size * 2 - 2;
            auto position = next_step_index_ % static_cast<std::int64_t>(period);
            if (position < 0)
                position += static_cast<std::int64_t>(period);
            const auto index = static_cast<std::size_t>(position);
            return index < size ? index : period - index;
        }
        case ArpeggiatorOrder::Random:
            return static_cast<std::size_t>(
                arpeggiator_detail::mix(spec_.random_seed ^
                                        static_cast<std::uint64_t>(next_step_index_)) %
                size);
        case ArpeggiatorOrder::Up:
        case ArpeggiatorOrder::AsPlayed:
        case ArpeggiatorOrder::Chord:
            return positive_mod(next_step_index_);
        }
        return 0;
    }

    bool fire_step(std::int64_t step_sample, timebase::TickPosition step_tick,
                   timebase::TickPosition following_tick, MidiBuffer& output,
                   MidiUtilityProcessReport& report) noexcept {
        const auto pattern_size = build_pattern();
        if (pattern_size == 0)
            return release_all_at_sample(output, report, step_sample);

        const auto unsigned_interval = following_tick.value <= step_tick.value
                                           ? std::uint64_t{0}
                                           : static_cast<std::uint64_t>(following_tick.value) -
                                                 static_cast<std::uint64_t>(step_tick.value);
        const auto interval = std::max<std::int64_t>(
            1, unsigned_interval >=
                       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                   ? std::numeric_limits<std::int64_t>::max()
                   : static_cast<std::int64_t>(unsigned_interval));
        const auto gate_ticks = std::max<std::int64_t>(
            1, arpeggiator_detail::saturating_round(
                   (static_cast<long double>(interval) * spec_.gate.numerator) /
                   spec_.gate.denominator));
        const auto release_tick = timebase::TickPosition{
            utility_detail::saturating_sample_add(step_tick.value, gate_ticks)};
        const auto release_sample = sample_for_tick(release_tick);

        selected_count_ = 0;
        if (spec_.order == ArpeggiatorOrder::Chord) {
            for (std::size_t index = 0; index < pattern_size; ++index)
                selected_[selected_count_++] = pattern_[index];
        } else {
            selected_[selected_count_++] = pattern_[selected_pattern_index(pattern_size)];
        }

        const bool may_tie = spec_.repeated_note == ArpeggiatorRepeatedNotePolicy::Tie &&
                             spec_.gate.numerator == spec_.gate.denominator;
        if (!release_for_step(selected_, selected_count_, may_tie, step_sample, output, report))
            return false;

        const auto offset = block_offset(step_sample);
        for (std::size_t index = 0; index < selected_count_; ++index) {
            const auto& note = selected_[index];
            if (may_tie && sounding_contains(note.channel, note.note)) {
                extend_sounding(note.channel, note.note, release_sample);
                continue;
            }
            auto* slot = free_sounding_slot();
            if (slot == nullptr) {
                ++report.dropped;
                report.complete = false;
                return false;
            }
            auto attack = MidiEvent::note_on(note.channel, note.note, note.velocity);
            attack.sample_offset = offset;
            if (!utility_detail::emit(output, attack, report))
                return false;
            *slot = {note.channel, note.note, release_sample, true};
        }
        return true;
    }

    bool release_for_step(const std::array<PatternNote, kMaximumPatternNotes>& next,
                          std::size_t next_count, bool may_tie, std::int64_t sample,
                          MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        for (auto& sounding : sounding_) {
            if (!sounding.active)
                continue;
            bool keep = false;
            if (may_tie) {
                for (std::size_t index = 0; index < next_count; ++index) {
                    if (next[index].channel == sounding.channel &&
                        next[index].note == sounding.note) {
                        keep = true;
                        break;
                    }
                }
            }
            if (keep)
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
        return release_all_at_sample(output, report, sample);
    }

    bool release_all_at_sample(MidiBuffer& output, MidiUtilityProcessReport& report,
                               std::int64_t sample) noexcept {
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

    SoundingNote* free_sounding_slot() noexcept {
        for (auto& note : sounding_)
            if (!note.active)
                return &note;
        return nullptr;
    }

    bool sounding_contains(std::uint8_t channel, std::uint8_t note) const noexcept {
        for (const auto& sounding : sounding_)
            if (sounding.active && sounding.channel == channel && sounding.note == note)
                return true;
        return false;
    }

    void extend_sounding(std::uint8_t channel, std::uint8_t note,
                         std::int64_t release_sample) noexcept {
        for (auto& sounding : sounding_)
            if (sounding.active && sounding.channel == channel && sounding.note == note)
                sounding.release_sample = std::max(sounding.release_sample, release_sample);
    }

    bool drain_pending_spec(MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        if (!pending_spec_)
            return true;
        if (!release_all(output, report, 0))
            return false;
        const auto replacement = *pending_spec_;
        pending_spec_.reset();
        apply_spec(replacement);
        return true;
    }

    void apply_spec(const ArpeggiatorSpec& spec) noexcept {
        const bool disabling_latch = spec_.latch && !spec.latch;
        spec_ = spec;
        valid_ = true;
        clock_initialized_ = false;
        if (disabling_latch)
            clear_latched_notes();
    }

    ArpeggiatorSpec spec_{};
    bool valid_ = true;
    std::optional<ArpeggiatorSpec> pending_spec_;
    std::array<HeldNote, MaximumHeldNotes> held_{};
    std::array<PatternNote, kMaximumPatternNotes> pattern_{};
    std::array<PatternNote, kMaximumPatternNotes> selected_{};
    std::size_t selected_count_ = 0;
    std::array<SoundingNote, kMaximumPatternNotes> sounding_{};
    std::array<OwnershipRun, MaximumOwnershipRuns> ownership_{};
    std::array<std::uint32_t, 16 * 128> overflow_suppressed_{};
    std::size_t ownership_size_ = 0;
    std::size_t physical_count_ = 0;
    std::uint64_t next_serial_ = 0;
    std::int64_t next_step_index_ = 0;
    timebase::TickPosition next_step_tick_{};
    bool clock_initialized_ = false;
    const ArpeggiatorBlock* current_block_ = nullptr;
    std::int64_t last_sample_end_ = 0;
    std::int64_t projected_tick_end_ = 0;
    bool last_playing_ = false;
    bool last_block_valid_ = false;
    std::size_t steps_this_block_ = 0;
};

} // namespace pulp::midi
