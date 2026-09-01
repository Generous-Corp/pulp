#pragma once

#include <pulp/midi/detail/note_schedule.hpp>
#include <pulp/timebase/coordinate_random.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pulp::midi {

enum class StrumDirection : std::uint8_t {
    Up,
    Down,
    /// Flip between up and down on each successive cluster.
    Alternate,
    /// Keep the order the notes arrived in.
    AsPlayed,
    /// Seeded shuffle, stable for a given cluster coordinate.
    Random,
};

/// How the spacing is distributed across the spread.
enum class StrumShape : std::uint8_t {
    Linear,
    /// Notes bunch at the start and open out.
    Accelerate,
    /// Notes open wide and close up.
    Decelerate,
};

enum class StrumSpacingSync : std::uint8_t {
    Division,
    Milliseconds,
};

/// Spread a near-simultaneous cluster of notes across time.
///
/// Notes arriving within `window` of the cluster's first note are one chord.
/// The kernel cannot know a cluster is finished until the window closes, so it
/// costs exactly one window of latency — declared here rather than hidden.
///
/// A single-note cluster is emitted unchanged, which is the declared identity.
struct StrumSpec {
    StrumDirection direction = StrumDirection::Up;
    StrumShape shape = StrumShape::Linear;
    StrumSpacingSync sync = StrumSpacingSync::Milliseconds;
    /// Cluster window in samples: how close two notes must be to be one chord.
    std::int64_t window_samples = 0;
    timebase::TickDuration spacing_interval{timebase::kTicksPerQuarter / 16};
    std::int64_t spacing_milliseconds = 20;
    /// Forward-only timing jitter per note, in samples.
    std::int64_t timing_jitter_samples = 0;
    std::uint8_t velocity_jitter = 0;
    std::uint64_t seed = 0;
    constexpr auto operator<=>(const StrumSpec&) const = default;
};

template <std::size_t MaximumClusterNotes = 16> class Strum {
  public:
    using Block = note_schedule::Block;
    static constexpr std::uint64_t kTimingStream = 5;
    static constexpr std::uint64_t kVelocityStream = 6;
    static constexpr std::uint64_t kOrderStream = 7;

    static constexpr MidiUtilityContract contract() noexcept {
        return {1, MaximumClusterNotes, MidiUtilityOverflowPolicy::FailOpenBalanced,
                MidiUtilitySameSampleOrder::InputStable,
                MidiUtilityTransportRequirement::FlushOnDiscontinuity};
    }

    static constexpr bool valid_spec(StrumSpec spec) noexcept {
        if (spec.window_samples < 0 || spec.timing_jitter_samples < 0 || spec.velocity_jitter > 127)
            return false;
        return spec.sync == StrumSpacingSync::Division ? spec.spacing_interval.value > 0
                                                       : spec.spacing_milliseconds > 0;
    }

    /// Sample stride between adjacent notes of a linear spread.
    static std::int64_t spacing_samples(StrumSpec spec, const Block& block) noexcept {
        return spec.sync == StrumSpacingSync::Division
                   ? note_schedule::samples_for_duration(block, spec.spacing_interval)
                   : note_schedule::samples_for_milliseconds(block, spec.spacing_milliseconds);
    }

    /// Position of the note at ordered index `index` in a cluster of `count`,
    /// expressed in whole spacing steps. Integer arithmetic throughout, so the
    /// spread is bit-exact and a test can derive it independently.
    static constexpr std::int64_t shaped_step(StrumShape shape, std::size_t index,
                                              std::size_t count) noexcept {
        if (count <= 1 || index == 0)
            return 0;
        const auto last = static_cast<std::int64_t>(count - 1);
        const auto position = static_cast<std::int64_t>(index);
        switch (shape) {
        case StrumShape::Accelerate:
            return position * position / last;
        case StrumShape::Decelerate:
            return last - (last - position) * (last - position) / last;
        case StrumShape::Linear:
            break;
        }
        return position;
    }

    explicit constexpr Strum(StrumSpec spec = {}) noexcept
        : spec_(spec), valid_(valid_spec(spec)) {}

    constexpr bool valid() const noexcept {
        return valid_;
    }

    constexpr StrumSpec spec() const noexcept {
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
            advance_to(absolute, block, output, report);
            const bool is_attack = event.is_note_on() && event.velocity() != 0;
            const bool is_release =
                event.is_note_off() || (event.is_note_on() && event.velocity() == 0);
            if (!is_attack && !is_release) {
                utility_detail::emit(output, event, report);
                continue;
            }
            if (is_attack) {
                admit(event, absolute, block, output, report);
                continue;
            }
            // A release can outrun its own spread note. Emitting the attack
            // first keeps the pair ordered and the note balanced.
            force_out(event.channel(), event.note(), absolute, block, output, report);
            utility_detail::emit(output, event, report);
        }
        advance_to(block_end, block, output, report);
        utility_detail::copy_sidecars(input, output, report);
        output.sort();
        return report;
    }

    /// Emit every buffered and scheduled attack immediately.
    MidiUtilityProcessReport flush(MidiBuffer& output, const Block& block) noexcept {
        utility_detail::clear_output(output);
        MidiUtilityProcessReport report;
        if (!utility_detail::ready(output)) {
            report.complete = false;
            return report;
        }
        if (open_)
            close_cluster(block);
        for (auto& slot : cluster_) {
            if (!slot.active)
                continue;
            if (!emit_slot(slot, block.sample_start.value, block, output, report)) {
                ++report.deferred;
                report.complete = false;
                return report;
            }
        }
        count_ = 0;
        open_ = false;
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

    MidiUtilityProcessReport replace_spec(StrumSpec spec, MidiBuffer& output,
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
        for (const auto& slot : cluster_)
            if (slot.active)
                return false;
        return true;
    }

  private:
    struct Slot {
        MidiEvent event{};
        std::int64_t arrival = 0;
        std::int64_t scheduled = 0;
        std::uint8_t order = 0;
        bool scheduled_yet = false;
        bool active = false;
    };

    static std::uint64_t draw(std::uint64_t seed, std::int64_t coordinate, std::uint64_t lane,
                              std::uint64_t stream, std::uint64_t span) noexcept {
        if (span == 0)
            return 0;
        const timebase::RandomCoordinate point{timebase::TickPosition{coordinate}, lane, 0, stream};
        return timebase::detail::multiply_high(timebase::coordinate_random(seed, point), span);
    }

    void admit(const MidiEvent& event, std::int64_t absolute, const Block& block,
               MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        if (open_ && absolute - cluster_start_ > spec_.window_samples)
            close_cluster(block);
        if (!open_) {
            cluster_start_ = absolute;
            open_ = true;
        }
        Slot* slot = free_slot();
        if (slot == nullptr) {
            // FailOpenBalanced: a cluster that cannot grow forwards the note
            // untouched rather than swallowing it, so the player's note always
            // sounds and its release always matches something.
            close_cluster(block);
            flush_scheduled(std::numeric_limits<std::int64_t>::max(), block, output, report);
            utility_detail::emit(output, event, report);
            return;
        }
        *slot = {event, absolute, 0, 0, false, true};
        ++count_;
    }

    /// Fix the cluster's order and each note's position. Called when the window
    /// closes, which is the first moment the full chord is known.
    void close_cluster(const Block& block) noexcept {
        open_ = false;
        std::array<Slot*, MaximumClusterNotes> members{};
        std::size_t members_count = 0;
        for (auto& slot : cluster_)
            if (slot.active && !slot.scheduled_yet && members_count < MaximumClusterNotes)
                members[members_count++] = &slot;
        if (members_count == 0)
            return;
        order_members(members, members_count);
        const auto spacing = spacing_samples(spec_, block);
        const auto base =
            utility_detail::saturating_sample_add(cluster_start_, spec_.window_samples);
        for (std::size_t index = 0; index < members_count; ++index) {
            auto* slot = members[index];
            const auto step = shaped_step(spec_.shape, index, members_count);
            auto at = utility_detail::saturating_sample_add(base, spacing * step);
            if (spec_.timing_jitter_samples > 0) {
                const auto lane = static_cast<std::uint64_t>(
                    utility_detail::key_index(slot->event.channel(), slot->event.note()));
                at = utility_detail::saturating_sample_add(
                    at, static_cast<std::int64_t>(
                            draw(spec_.seed, slot->arrival, lane, kTimingStream,
                                 static_cast<std::uint64_t>(spec_.timing_jitter_samples) + 1)));
            }
            slot->scheduled = std::max(at, slot->arrival);
            slot->order = static_cast<std::uint8_t>(index);
            slot->scheduled_yet = true;
            if (spec_.velocity_jitter != 0)
                apply_velocity_jitter(*slot);
        }
        alternate_flip_ = !alternate_flip_;
    }

    void apply_velocity_jitter(Slot& slot) noexcept {
        const auto lane = static_cast<std::uint64_t>(
            utility_detail::key_index(slot.event.channel(), slot.event.note()));
        const auto span = static_cast<std::uint64_t>(spec_.velocity_jitter) * 2 + 1;
        const auto value = static_cast<std::int32_t>(
            draw(spec_.seed, slot.arrival, lane, kVelocityStream, span));
        const auto offset = value - static_cast<std::int32_t>(spec_.velocity_jitter);
        const auto velocity = static_cast<std::uint8_t>(
            std::clamp(static_cast<std::int32_t>(slot.event.velocity()) + offset, 1, 127));
        auto shaped = MidiEvent::note_on(slot.event.channel(), slot.event.note(), velocity);
        shaped.timestamp = slot.event.timestamp;
        slot.event = shaped;
    }

    void order_members(std::array<Slot*, MaximumClusterNotes>& members,
                       std::size_t count) noexcept {
        switch (effective_direction()) {
        case StrumDirection::AsPlayed:
            // Arrival order is already the buffer order; ties keep input order.
            std::stable_sort(members.begin(), members.begin() + static_cast<std::ptrdiff_t>(count),
                             [](const Slot* a, const Slot* b) { return a->arrival < b->arrival; });
            return;
        case StrumDirection::Up:
            std::stable_sort(members.begin(), members.begin() + static_cast<std::ptrdiff_t>(count),
                             [](const Slot* a, const Slot* b) {
                                 return a->event.note() < b->event.note();
                             });
            return;
        case StrumDirection::Down:
            std::stable_sort(members.begin(), members.begin() + static_cast<std::ptrdiff_t>(count),
                             [](const Slot* a, const Slot* b) {
                                 return a->event.note() > b->event.note();
                             });
            return;
        case StrumDirection::Random:
            break;
        case StrumDirection::Alternate:
            break;
        }
        // Random: a seeded key per member gives a stable permutation for this
        // cluster coordinate without any mutable generator state.
        std::stable_sort(members.begin(), members.begin() + static_cast<std::ptrdiff_t>(count),
                         [this](const Slot* a, const Slot* b) {
                             return random_key(*a) < random_key(*b);
                         });
    }

    StrumDirection effective_direction() const noexcept {
        if (spec_.direction != StrumDirection::Alternate)
            return spec_.direction;
        return alternate_flip_ ? StrumDirection::Down : StrumDirection::Up;
    }

    std::uint64_t random_key(const Slot& slot) const noexcept {
        const auto lane =
            static_cast<std::uint64_t>(utility_detail::key_index(slot.event.channel(), slot.event.note()));
        const timebase::RandomCoordinate point{timebase::TickPosition{cluster_start_}, lane, 0,
                                               kOrderStream};
        return timebase::coordinate_random(spec_.seed, point);
    }

    void advance_to(std::int64_t boundary, const Block& block, MidiBuffer& output,
                    MidiUtilityProcessReport& report) noexcept {
        if (open_ && boundary - cluster_start_ > spec_.window_samples)
            close_cluster(block);
        flush_scheduled(boundary, block, output, report);
    }

    void flush_scheduled(std::int64_t boundary, const Block& block, MidiBuffer& output,
                         MidiUtilityProcessReport& report) noexcept {
        while (true) {
            Slot* earliest = nullptr;
            for (auto& slot : cluster_) {
                if (!slot.active || !slot.scheduled_yet || slot.scheduled >= boundary)
                    continue;
                if (earliest == nullptr || slot.scheduled < earliest->scheduled ||
                    (slot.scheduled == earliest->scheduled && slot.order < earliest->order))
                    earliest = &slot;
            }
            if (earliest == nullptr)
                return;
            if (!emit_slot(*earliest, earliest->scheduled, block, output, report))
                return;
        }
    }

    void force_out(std::uint8_t channel, std::uint8_t note, std::int64_t before, const Block& block,
                   MidiBuffer& output, MidiUtilityProcessReport& report) noexcept {
        for (auto& slot : cluster_) {
            if (!slot.active || slot.event.channel() != channel || slot.event.note() != note)
                continue;
            const auto at = slot.scheduled_yet ? std::min(slot.scheduled, before) : before;
            emit_slot(slot, at, block, output, report);
        }
    }

    bool emit_slot(Slot& slot, std::int64_t at, const Block& block, MidiBuffer& output,
                   MidiUtilityProcessReport& report) noexcept {
        const auto offset = note_schedule::offset_in_block(block, at);
        if (!utility_detail::emit(output, utility_detail::at(slot.event, offset), report))
            return false;
        slot = {};
        if (count_ != 0)
            --count_;
        return true;
    }

    Slot* free_slot() noexcept {
        for (auto& slot : cluster_)
            if (!slot.active)
                return &slot;
        return nullptr;
    }

    StrumSpec spec_{};
    bool valid_ = true;
    std::optional<StrumSpec> pending_spec_{};
    std::array<Slot, MaximumClusterNotes> cluster_{};
    std::size_t count_ = 0;
    std::int64_t cluster_start_ = 0;
    bool open_ = false;
    bool alternate_flip_ = false;
};

} // namespace pulp::midi
