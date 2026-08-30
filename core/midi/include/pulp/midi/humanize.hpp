#pragma once

#include <pulp/midi/utility_contract.hpp>
#include <pulp/timebase/coordinate_random.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pulp::midi {

/// Seeded timing and velocity jitter over note attacks.
///
/// Timing jitter is a forward-only delay in `[0, timing_samples]`. A kernel on
/// the audio thread cannot emit an event earlier than it arrived, so a
/// symmetric jitter would only be expressible by delaying the whole stream;
/// a forward delay keeps the kernel causal and keeps latency equal to the
/// declared bound rather than hiding it.
///
/// Both jitter draws come from `timebase::coordinate_random` over the event's
/// absolute sample position, so the same authored note receives the same jitter
/// no matter how the caller partitions its blocks.
struct HumanizeSpec {
    std::int64_t timing_samples = 0;
    std::uint8_t velocity_amount = 0;
    std::uint64_t seed = 0;
    constexpr auto operator<=>(const HumanizeSpec&) const = default;
};

class Humanize {
  public:
    static constexpr std::size_t kKeySpace = 16 * 128;
    /// Random streams are separated so changing one amount cannot shift the
    /// other's draw for the same note.
    static constexpr std::uint64_t kTimingStream = 1;
    static constexpr std::uint64_t kVelocityStream = 2;

    static constexpr MidiUtilityContract contract() noexcept {
        return {1, kKeySpace, MidiUtilityOverflowPolicy::FailOpenBalanced,
                MidiUtilitySameSampleOrder::InputStable,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(HumanizeSpec spec) noexcept {
        return spec.timing_samples >= 0 && spec.velocity_amount <= 127;
    }

    /// True when the spec cannot change any event, which is the kernel's
    /// declared bypass identity.
    static constexpr bool is_identity(HumanizeSpec spec) noexcept {
        return valid_spec(spec) && spec.timing_samples == 0 && spec.velocity_amount == 0;
    }

    explicit constexpr Humanize(HumanizeSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }

    constexpr HumanizeSpec spec() const noexcept {
        return spec_;
    }

    /// The absolute sample position at which `absolute` is emitted, derived
    /// only from the spec and the event's own coordinate. Exposed so a test or
    /// a UI can predict the schedule without pushing events through the
    /// kernel.
    static std::int64_t jittered_position(HumanizeSpec spec, std::uint8_t channel,
                                          std::uint8_t note, std::int64_t absolute) noexcept {
        if (spec.timing_samples <= 0)
            return absolute;
        const auto draw = draw_value(spec.seed, channel, note, absolute, kTimingStream,
                                     static_cast<std::uint64_t>(spec.timing_samples) + 1);
        return utility_detail::saturating_sample_add(absolute, static_cast<std::int64_t>(draw));
    }

    /// The velocity `velocity` becomes, derived only from the spec and the
    /// event's own coordinate. Never returns 0, because 0 would silently turn
    /// an attack into a release.
    static std::uint8_t jittered_velocity(HumanizeSpec spec, std::uint8_t channel,
                                          std::uint8_t note, std::int64_t absolute,
                                          std::uint8_t velocity) noexcept {
        if (spec.velocity_amount == 0)
            return velocity;
        const auto span = static_cast<std::uint64_t>(spec.velocity_amount) * 2 + 1;
        const auto draw = draw_value(spec.seed, channel, note, absolute, kVelocityStream, span);
        const auto offset =
            static_cast<std::int32_t>(draw) - static_cast<std::int32_t>(spec.velocity_amount);
        return static_cast<std::uint8_t>(std::clamp(static_cast<std::int32_t>(velocity) + offset, 1, 127));
    }

    MidiUtilityProcessReport process(const MidiBuffer& input, MidiBuffer& output,
                                     timebase::SamplePosition block_start,
                                     std::int32_t block_samples) noexcept {
        if (utility_detail::blocks_alias(input, output))
            return {0, input.size(), 0, false};
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output) || !valid_ || block_samples < 0) {
            report.dropped = input.size();
            report.complete = false;
            return report;
        }
        const std::int64_t block_end =
            utility_detail::saturating_sample_add(block_start.value, block_samples);
        for (const auto& event : input) {
            const auto absolute =
                utility_detail::saturating_sample_add(block_start.value, event.sample_offset);
            emit_due_before(absolute, block_start, output, report);
            const bool is_attack = event.is_note_on() && event.velocity() != 0;
            const bool is_release =
                event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
            if (is_attack) {
                schedule_attack(event, absolute, block_start, output, report);
            } else if (is_release) {
                // An authored release can arrive before its own jittered attack
                // has fired. Pulling the attack back to just before the release
                // keeps the note audible and keeps the pair balanced instead of
                // emitting an orphaned note-off.
                const int key = utility_detail::key_index(event.channel(), event.note());
                clamp_pending_against_release(key, absolute, block_start, output, report);
                utility_detail::emit(output, event, report);
            } else {
                utility_detail::emit(output, event, report);
            }
        }
        emit_due_before(block_end, block_start, output, report);
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    /// Emit every attack still waiting in the jitter window. The caller passes
    /// the block the flush belongs to so the released attacks keep sample
    /// offsets inside it.
    MidiUtilityProcessReport flush(MidiBuffer& output, timebase::SamplePosition block_start) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            report.complete = false;
            return report;
        }
        for (std::size_t key = 0; key < kKeySpace; ++key) {
            if (!pending_[key].active)
                continue;
            if (!emit_pending(key, pending_[key].scheduled, block_start, output, report)) {
                ++report.deferred;
                report.complete = false;
                return report;
            }
        }
        report.complete = empty();
        if (report.complete && pending_spec_) {
            spec_ = *pending_spec_;
            valid_ = true;
            pending_spec_.reset();
        }
        return report;
    }

    MidiUtilityProcessReport reset(MidiBuffer& output,
                                   timebase::SamplePosition block_start) noexcept {
        return flush(output, block_start);
    }

    MidiUtilityProcessReport replace_spec(HumanizeSpec spec, MidiBuffer& output,
                                          timebase::SamplePosition block_start) noexcept {
        if (!valid_spec(spec)) {
            utility_detail::clear_output(output);
            return {0, 0, 0, false};
        }
        auto report = flush(output, block_start);
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
        for (const auto& slot : pending_)
            if (slot.active)
                return false;
        return true;
    }

  private:
    struct Pending {
        MidiEvent event{};
        std::int64_t scheduled = 0;
        bool active = false;
    };

    static std::uint64_t draw_value(std::uint64_t seed, std::uint8_t channel, std::uint8_t note,
                                    std::int64_t absolute, std::uint64_t stream,
                                    std::uint64_t span) noexcept {
        const timebase::RandomCoordinate coordinate{
            timebase::TickPosition{absolute},
            static_cast<std::uint64_t>(utility_detail::key_index(channel, note)), 0, stream};
        const auto value = timebase::coordinate_random(seed, coordinate);
        return span == 0 ? 0 : timebase::detail::multiply_high(value, span);
    }

    void schedule_attack(const MidiEvent& event, std::int64_t absolute,
                         timebase::SamplePosition block_start, MidiBuffer& output,
                         MidiUtilityProcessReport& report) noexcept {
        const auto key = static_cast<std::size_t>(
            utility_detail::key_index(event.channel(), event.note()));
        // One key holds at most one attack in flight; a re-attack forces the
        // earlier one out first so the pair order is never inverted.
        if (pending_[key].active)
            emit_pending(key, absolute, block_start, output, report);
        auto shaped = event;
        const auto velocity = jittered_velocity(spec_, event.channel(), event.note(), absolute,
                                                event.velocity());
        shaped = MidiEvent::note_on(event.channel(), event.note(), velocity);
        shaped.timestamp = event.timestamp;
        const auto scheduled =
            jittered_position(spec_, event.channel(), event.note(), absolute);
        pending_[key] = {shaped, scheduled, true};
    }

    void clamp_pending_against_release(int key, std::int64_t release_absolute,
                                       timebase::SamplePosition block_start, MidiBuffer& output,
                                       MidiUtilityProcessReport& report) noexcept {
        auto& slot = pending_[static_cast<std::size_t>(key)];
        if (!slot.active || slot.scheduled < release_absolute)
            return;
        const auto latest = release_absolute > 0 ? release_absolute - 1 : release_absolute;
        emit_pending(static_cast<std::size_t>(key), std::min(slot.scheduled, latest), block_start,
                     output, report);
    }

    void emit_due_before(std::int64_t boundary, timebase::SamplePosition block_start,
                         MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        for (std::size_t key = 0; key < kKeySpace; ++key) {
            auto& slot = pending_[key];
            if (slot.active && slot.scheduled < boundary)
                emit_pending(key, slot.scheduled, block_start, output, report);
        }
    }

    bool emit_pending(std::size_t key, std::int64_t at_absolute,
                      timebase::SamplePosition block_start, MidiBuffer& output,
                      MidiUtilityProcessReport& report) noexcept {
        auto& slot = pending_[key];
        if (!slot.active)
            return true;
        const auto offset = at_absolute - block_start.value;
        const auto clamped = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(offset, 0, std::numeric_limits<std::int32_t>::max()));
        if (!utility_detail::emit(output, utility_detail::at(slot.event, clamped), report))
            return false;
        slot = {};
        return true;
    }

    HumanizeSpec spec_{};
    bool valid_ = true;
    std::optional<HumanizeSpec> pending_spec_{};
    std::array<Pending, kKeySpace> pending_{};
};

} // namespace pulp::midi
