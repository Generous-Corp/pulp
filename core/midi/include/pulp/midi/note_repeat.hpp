#pragma once

#include <pulp/midi/detail/note_schedule.hpp>
#include <pulp/timebase/coordinate_random.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pulp::midi {

/// Clock-divided note repeat / retrigger.
///
/// An authored attack is consumed and re-emitted as up to `count` gated hits
/// one `interval` apart, each hit carrying a decayed velocity and passing an
/// independent probability draw. Releasing the key cancels the hits that have
/// not started; a hit already sounding keeps its gate, so the kernel never
/// truncates a note it already owns.
///
/// `count <= 1` is the declared bypass: the input passes through unchanged and
/// the kernel owns nothing.
struct NoteRepeatSpec {
    timebase::TickDuration interval{timebase::kTicksPerQuarter / 4};
    std::uint8_t count = 1;
    std::uint8_t gate_percent = 50;
    std::uint8_t probability_percent = 100;
    std::uint8_t velocity_decay_percent = 100;
    std::uint64_t seed = 0;
    constexpr auto operator<=>(const NoteRepeatSpec&) const = default;
};

template <std::size_t MaximumScheduledNotes = 128> class NoteRepeat {
  public:
    using Block = note_schedule::Block;

    static constexpr MidiUtilityContract contract() noexcept {
        return {2 * 255, MaximumScheduledNotes, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(NoteRepeatSpec spec) noexcept {
        return spec.interval.value > 0 && spec.count >= 1 && spec.gate_percent >= 1 &&
               spec.gate_percent <= 100 && spec.probability_percent <= 100 &&
               spec.velocity_decay_percent <= 100;
    }

    /// True when the kernel cannot change the stream, which is its declared
    /// bypass identity.
    static constexpr bool is_identity(NoteRepeatSpec spec) noexcept {
        return valid_spec(spec) && spec.count <= 1;
    }

    /// Sample stride between consecutive hits, derived from division arithmetic
    /// alone. Exposed so a test or a UI can predict the series without pushing
    /// events through the kernel.
    static std::int64_t hit_stride(NoteRepeatSpec spec, const Block& block) noexcept {
        return note_schedule::samples_for_duration(block, spec.interval);
    }

    /// Absolute sample of hit `index` for an attack at `attack_absolute`.
    static std::int64_t hit_sample(NoteRepeatSpec spec, const Block& block,
                                   std::int64_t attack_absolute, std::size_t index) noexcept {
        return utility_detail::saturating_sample_add(
            attack_absolute, hit_stride(spec, block) * static_cast<std::int64_t>(index));
    }

    /// Velocity of hit `index` under the shipped decay law.
    static constexpr std::uint8_t hit_velocity(NoteRepeatSpec spec, std::uint8_t velocity,
                                               std::size_t index) noexcept {
        return note_schedule::decayed_velocity(velocity, spec.velocity_decay_percent, index);
    }

    /// Whether hit `index` of the attack at `attack_absolute` survives its
    /// probability draw. Depends only on the spec and the attack's own
    /// coordinate, so it is invariant under block partition.
    static bool hit_sounds(NoteRepeatSpec spec, std::uint8_t channel, std::uint8_t note,
                           std::int64_t attack_absolute, std::size_t index) noexcept {
        if (spec.probability_percent >= 100)
            return true;
        if (spec.probability_percent == 0)
            return false;
        const timebase::RandomCoordinate coordinate{
            timebase::TickPosition{attack_absolute},
            static_cast<std::uint64_t>(utility_detail::key_index(channel, note)),
            static_cast<std::uint64_t>(index), kProbabilityStream};
        const auto chance =
            timebase::coordinate_chance(spec.seed, coordinate, spec.probability_percent, 100);
        return chance.has_value() && chance.value();
    }

    explicit constexpr NoteRepeat(NoteRepeatSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }

    constexpr NoteRepeatSpec spec() const noexcept {
        return spec_;
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output,
                                     const Block& block) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output) || !valid_ || !note_schedule::valid_block(block)) {
            report.dropped = input.size();
            report.complete = false;
            return report;
        }
        const std::int64_t block_end =
            utility_detail::saturating_sample_add(block.sample_start.value, block.sample_count);
        for (const auto& event : input) {
            const auto absolute =
                utility_detail::saturating_sample_add(block.sample_start.value, event.sample_offset);
            emit_due_before(absolute, block, output, report);
            const bool is_attack = event.is_note_on() && event.velocity() != 0;
            const bool is_release =
                event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
            if (is_identity(spec_) || (!is_attack && !is_release)) {
                utility_detail::emit(output, event, report);
                continue;
            }
            if (is_attack) {
                schedule_series(event, absolute, block, report);
                continue;
            }
            // Letting go stops the hits that have not started. A hit already
            // sounding keeps its gate, so no note the kernel owns is truncated
            // and no note-off is orphaned.
            report.deferred += queue_.cancel_unstarted_key(event.channel(), event.note());
        }
        emit_due_before(block_end, block, output, report);
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    /// Cancel unstarted hits and release every note the kernel still owns.
    /// Offsets are clamped into `block`.
    MidiUtilityProcessReport flush(MidiBuffer& output, const Block& block) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            report.complete = false;
            return report;
        }
        report.deferred += queue_.cancel_unstarted();
        queue_.release_sounding_at(block.sample_start.value);
        const bool drained =
            queue_.drain([&](const note_schedule::ScheduledNote& slot, bool attack, std::int64_t at) {
                return emit_edge(slot, attack, at, block, output, report);
            });
        if (!drained) {
            ++report.deferred;
            report.complete = false;
            return report;
        }
        report.complete = empty();
        if (report.complete && pending_spec_) {
            spec_ = *pending_spec_;
            valid_ = true;
            pending_spec_.reset();
        }
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output, const Block& block) noexcept {
        return flush(output, block);
    }

    MidiUtilityProcessReport replace_spec(NoteRepeatSpec spec, MidiBuffer& output,
                                          const Block& block) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(output);
            return {0, 0, 0, false};
        }
        auto report = flush(output, block);
        if (report.complete) {
            spec_ = spec;
            valid_ = true;
            pending_spec_.reset();
        } else {
            pending_spec_ = spec;
        }
        return report;
    }

    bool empty() const noexcept {
        return queue_.empty();
    }

    std::size_t scheduled() const noexcept {
        return queue_.size();
    }

    std::size_t sounding() const noexcept {
        return queue_.sounding();
    }

  private:
    static constexpr std::uint64_t kProbabilityStream = 3;

    void schedule_series(const MidiEvent& event, std::int64_t absolute, const Block& block,
                         MidiUtilityProcessReport& report) noexcept {
        const auto stride = hit_stride(spec_, block);
        const auto gate =
            std::max<std::int64_t>(1, stride * static_cast<std::int64_t>(spec_.gate_percent) / 100);
        for (std::size_t index = 0; index < spec_.count; ++index) {
            if (!hit_sounds(spec_, event.channel(), event.note(), absolute, index))
                continue;
            auto* slot = queue_.acquire();
            if (slot == nullptr) {
                // DropUnstarted: a hit that cannot be scheduled never sounds, so
                // it can never leave an unbalanced note behind.
                ++report.dropped;
                report.complete = false;
                return;
            }
            const auto at = utility_detail::saturating_sample_add(
                absolute, stride * static_cast<std::int64_t>(index));
            slot->start = at;
            slot->end = utility_detail::saturating_sample_add(at, gate);
            slot->channel = event.channel();
            slot->note = event.note();
            slot->velocity = hit_velocity(spec_, event.velocity(), index);
        }
    }

    void emit_due_before(std::int64_t boundary, const Block& block, MidiBuffer& output,
                         MidiUtilityProcessReport& report) noexcept {
        queue_.emit_due_before(
            boundary, [&](const note_schedule::ScheduledNote& slot, bool attack, std::int64_t at) {
                return emit_edge(slot, attack, at, block, output, report);
            });
    }

    static bool emit_edge(const note_schedule::ScheduledNote& slot, bool attack, std::int64_t at,
                          const Block& block, MidiBuffer& output,
                          MidiUtilityProcessReport& report) noexcept {
        const auto offset = note_schedule::offset_in_block(block, at);
        const auto event = attack ? MidiEvent::note_on(slot.channel, slot.note, slot.velocity)
                                  : MidiEvent::note_off(slot.channel, slot.note);
        return utility_detail::emit(output, utility_detail::at(event, offset), report);
    }

    NoteRepeatSpec spec_{};
    bool valid_ = true;
    std::optional<NoteRepeatSpec> pending_spec_{};
    note_schedule::ScheduleQueue<MaximumScheduledNotes> queue_{};
};

} // namespace pulp::midi
