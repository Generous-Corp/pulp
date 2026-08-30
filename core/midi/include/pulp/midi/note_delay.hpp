#pragma once

#include <pulp/midi/detail/note_schedule.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pulp::midi {

enum class NoteDelaySync : std::uint8_t {
    Division,
    Milliseconds,
};

/// MIDI note delay / echo.
///
/// The authored note passes through untouched and each repeat is an additional
/// scheduled note, so the kernel is a send rather than a replacement. A repeat
/// carries a decayed velocity and an optional cumulative transposition.
///
/// `repeats == 0` is the declared bypass: the stream passes through unchanged
/// and the kernel owns nothing.
struct NoteDelaySpec {
    NoteDelaySync sync = NoteDelaySync::Division;
    timebase::TickDuration interval{timebase::kTicksPerQuarter / 4};
    std::int64_t milliseconds = 250;
    std::uint8_t repeats = 0;
    std::uint8_t velocity_decay_percent = 70;
    std::int8_t transpose_semitones = 0;
    constexpr auto operator<=>(const NoteDelaySpec&) const = default;
};

/// `MaximumHeldSources` bounds how many authored notes can have armed echoes at
/// once. Like the scheduled-note queue it is a polyphony bound, not the whole
/// 16x128 key space: the key space would make the kernel a 52 KiB object to buy
/// headroom no keyboard can reach.
template <std::size_t MaximumScheduledNotes = 128, std::size_t MaximumHeldSources = 64>
class NoteDelay {
  public:
    using Block = note_schedule::Block;

    static constexpr MidiUtilityContract contract() noexcept {
        return {2 * 256, MaximumScheduledNotes, MidiUtilityOverflowPolicy::DropUnstarted,
                MidiUtilitySameSampleOrder::ReleaseBeforeAttack,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(NoteDelaySpec spec) noexcept {
        if (spec.velocity_decay_percent > 100)
            return false;
        return spec.sync == NoteDelaySync::Division ? spec.interval.value > 0
                                                    : spec.milliseconds > 0;
    }

    /// True when the kernel cannot change the stream, which is its declared
    /// bypass identity.
    static constexpr bool is_identity(NoteDelaySpec spec) noexcept {
        return valid_spec(spec) && spec.repeats == 0;
    }

    /// Sample stride between consecutive repeats under whichever clock the
    /// spec selects. Exposed so a test or a UI can predict the echo series
    /// without pushing events through the kernel.
    static std::int64_t repeat_stride(NoteDelaySpec spec, const Block& block) noexcept {
        return spec.sync == NoteDelaySync::Division
                   ? note_schedule::samples_for_duration(block, spec.interval)
                   : note_schedule::samples_for_milliseconds(block, spec.milliseconds);
    }

    /// Absolute sample of repeat `index` (1-based; index 0 is the dry note).
    static std::int64_t repeat_sample(NoteDelaySpec spec, const Block& block,
                                      std::int64_t attack_absolute, std::size_t index) noexcept {
        return utility_detail::saturating_sample_add(
            attack_absolute, repeat_stride(spec, block) * static_cast<std::int64_t>(index));
    }

    /// Velocity of repeat `index` under the shipped decay law.
    static constexpr std::uint8_t repeat_velocity(NoteDelaySpec spec, std::uint8_t velocity,
                                                  std::size_t index) noexcept {
        return note_schedule::decayed_velocity(velocity, spec.velocity_decay_percent, index);
    }

    /// Pitch of repeat `index`, or nothing when the cumulative transposition
    /// has walked the repeat off the MIDI note range. An out-of-range repeat is
    /// skipped rather than folded back, because folding would invent a pitch
    /// the authored transposition never asked for.
    static constexpr std::optional<std::uint8_t>
    repeat_note(NoteDelaySpec spec, std::uint8_t note, std::size_t index) noexcept {
        const auto shifted = static_cast<std::int32_t>(note) +
                             static_cast<std::int32_t>(spec.transpose_semitones) *
                                 static_cast<std::int32_t>(index);
        if (shifted < 0 || shifted > 127)
            return std::nullopt;
        return static_cast<std::uint8_t>(shifted);
    }

    explicit constexpr NoteDelay(NoteDelaySpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }

    constexpr NoteDelaySpec spec() const noexcept {
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
            // The dry note always passes through: this kernel is a send.
            utility_detail::emit(output, event, report);
            if (is_identity(spec_) || (!is_attack && !is_release))
                continue;
            if (is_attack)
                schedule_echoes(event, absolute, block, report);
            else
                roll_back_armed(event, absolute, block, output, report);
        }
        emit_due_before(block_end, block, output, report);
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    /// Cancel unstarted echoes and release every echo the kernel still owns,
    /// including the ones whose end is still armed.
    MidiUtilityProcessReport flush(MidiBuffer& output, const Block& block) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            report.complete = false;
            return report;
        }
        report.deferred += queue_.cancel_unstarted();
        // An armed end is `kArmedEnd`; clamping it here is what lets the drain
        // below finish notes whose authored release never arrived.
        queue_.release_sounding_at(block.sample_start.value);
        sources_.fill({});
        const bool drained = queue_.drain(
            [&](const note_schedule::ScheduledNote& slot, bool attack, std::int64_t at) {
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

    MidiUtilityProcessReport replace_spec(NoteDelaySpec spec, MidiBuffer& output,
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
    /// The authored attack an armed echo group came from. Group ids are the key
    /// index plus one, so 0 stays available as "no group".
    struct Source {
        std::int64_t attack = 0;
        std::uint16_t group = 0;
        bool armed = false;
    };

    Source* find_source(std::uint16_t group) noexcept {
        for (auto& source : sources_)
            if (source.armed && source.group == group)
                return &source;
        return nullptr;
    }

    Source* claim_source(std::uint16_t group) noexcept {
        if (auto* existing = find_source(group); existing != nullptr)
            return existing;
        for (auto& source : sources_)
            if (!source.armed)
                return &source;
        return nullptr;
    }

    static constexpr std::uint16_t group_for(std::uint8_t channel, std::uint8_t note) noexcept {
        return static_cast<std::uint16_t>(utility_detail::key_index(channel, note) + 1);
    }

    void schedule_echoes(const MidiEvent& event, std::int64_t absolute, const Block& block,
                         MidiUtilityProcessReport& report) noexcept {
        const auto stride = repeat_stride(spec_, block);
        const auto group = group_for(event.channel(), event.note());
        // A re-attack before the previous release retires the old group's arm,
        // so the earlier echoes keep the length they already have instead of
        // being rolled back by the wrong release.
        retire_group(group);
        auto* source = claim_source(group);
        if (source == nullptr) {
            // With nothing to record the arm against, the echoes could never be
            // rolled back to a real length, so none is scheduled.
            ++report.dropped;
            report.complete = false;
            return;
        }
        *source = {absolute, group, true};
        for (std::size_t index = 1; index <= spec_.repeats; ++index) {
            const auto pitch = repeat_note(spec_, event.note(), index);
            if (!pitch)
                continue;
            auto* slot = queue_.acquire();
            if (slot == nullptr) {
                // DropUnstarted: an echo that cannot be scheduled never sounds,
                // so it can never leave an unbalanced note behind.
                ++report.dropped;
                report.complete = false;
                return;
            }
            slot->start = utility_detail::saturating_sample_add(
                absolute, stride * static_cast<std::int64_t>(index));
            slot->end = note_schedule::kArmedEnd;
            slot->channel = event.channel();
            slot->note = *pitch;
            slot->velocity = repeat_velocity(spec_, event.velocity(), index);
            slot->group = group;
            slot->step = static_cast<std::uint16_t>(index);
        }
    }

    /// The authored release reveals how long the source note was held, which is
    /// the length every armed echo of that note should have. Echoes already
    /// past that length are released at the current position instead, which is
    /// the rollback the armed end exists for.
    void roll_back_armed(const MidiEvent& event, std::int64_t absolute, const Block& block,
                         MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        const auto group = group_for(event.channel(), event.note());
        auto* source = find_source(group);
        if (source == nullptr)
            return;
        const auto held = std::max<std::int64_t>(1, absolute - source->attack);
        queue_.visit([&](note_schedule::ScheduledNote& slot) {
            if (slot.group != group || slot.end != note_schedule::kArmedEnd)
                return;
            const auto end = utility_detail::saturating_sample_add(slot.start, held);
            slot.end = slot.sounding ? std::max(end, absolute) : end;
        });
        *source = {};
        emit_due_before(absolute, block, output, report);
    }

    void retire_group(std::uint16_t group) noexcept {
        queue_.visit([&](note_schedule::ScheduledNote& slot) {
            if (slot.group == group)
                slot.group = 0;
        });
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

    NoteDelaySpec spec_{};
    bool valid_ = true;
    std::optional<NoteDelaySpec> pending_spec_{};
    note_schedule::ScheduleQueue<MaximumScheduledNotes> queue_{};
    std::array<Source, MaximumHeldSources> sources_{};
};

} // namespace pulp::midi
