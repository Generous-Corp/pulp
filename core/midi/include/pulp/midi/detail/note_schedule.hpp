#pragma once

#include <pulp/midi/utility_contract.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

/// Shared substrate for the MIDI kernels that re-emit an authored note as a
/// bounded series of scheduled notes. Note repeat and note delay project the
/// same authored `timebase` divisions onto absolute samples with the same
/// arithmetic, so the projection and the scheduled-note queue live here once
/// rather than in two public headers that could drift apart.
namespace pulp::midi::note_schedule {

/// Block timing for one sample-accurate call.
///
/// `tick_start` and `sample_start` describe the same block boundary. A compiled
/// tempo map gives the exact tick-to-sample projection; a caller without one
/// supplies the block's constant tempo and sample rate instead.
struct Block {
    timebase::SamplePosition sample_start{};
    timebase::TickPosition tick_start{};
    std::int32_t sample_count = 0;
    timebase::RationalRate sample_rate{};
    double tempo_bpm = 120.0;
    const timebase::CompiledTempoMap* tempo_map = nullptr;
};

inline bool valid_block(const Block& block) noexcept {
    if (block.sample_count < 0)
        return false;
    if (block.tempo_map != nullptr)
        return block.tempo_map->sample_rate().valid();
    return block.sample_rate.valid() && block.tempo_bpm > 0.0;
}

constexpr long double difference_as_long_double(std::int64_t lhs, std::int64_t rhs) noexcept {
    return static_cast<long double>(lhs) - static_cast<long double>(rhs);
}

inline std::int64_t saturate_to_samples(long double absolute) noexcept {
    constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if (absolute <= minimum)
        return std::numeric_limits<std::int64_t>::min();
    if (absolute >= maximum)
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(absolute);
}

/// Absolute sample position of `tick`, anchored on `block`'s own boundary so a
/// tempo map and a constant tempo agree at the anchor.
inline std::int64_t sample_for_tick(const Block& block, timebase::TickPosition tick) noexcept {
    long double delta = 0.0L;
    if (block.tempo_map != nullptr) {
        const auto event_sample =
            block.tempo_map->fractional_ticks_to_samples(static_cast<long double>(tick.value));
        const auto anchor_sample = block.tempo_map->fractional_ticks_to_samples(
            static_cast<long double>(block.tick_start.value));
        delta = event_sample - anchor_sample;
    } else {
        const auto tick_delta = difference_as_long_double(tick.value, block.tick_start.value);
        delta = tick_delta * block.sample_rate.as_long_double() * 60.0L /
                (static_cast<long double>(block.tempo_bpm) *
                 static_cast<long double>(timebase::kTicksPerQuarter));
    }
    return saturate_to_samples(static_cast<long double>(block.sample_start.value) + delta);
}

/// Sample count spanned by `duration` at `block`'s tempo, measured from the
/// block anchor so it uses the same projection as `sample_for_tick`.
inline std::int64_t samples_for_duration(const Block& block,
                                         timebase::TickDuration duration) noexcept {
    const auto end = utility_detail::saturating_sample_add(block.tick_start.value, duration.value);
    return sample_for_tick(block, timebase::TickPosition{end}) - block.sample_start.value;
}

/// Milliseconds expressed in this block's samples.
inline std::int64_t samples_for_milliseconds(const Block& block,
                                             std::int64_t milliseconds) noexcept {
    const auto rate = block.tempo_map != nullptr ? block.tempo_map->sample_rate().as_long_double()
                                                 : block.sample_rate.as_long_double();
    return saturate_to_samples(static_cast<long double>(milliseconds) * rate / 1000.0L);
}

/// One note the kernel has promised to play.
///
/// A slot owns the note's whole lifecycle, so an attack and its release can
/// never be separated: the release position is fixed when the note is
/// scheduled, and freeing an unstarted slot cancels both halves at once. That
/// is what makes "cancel the repeats that have not started" a safe operation
/// with no orphaned note-off.
/// A note whose end is not known yet carries `kArmedEnd`. An echo of a held
/// note is armed at schedule time and rolled back to a real end once the
/// authored release reveals how long the source note was.
inline constexpr std::int64_t kArmedEnd = std::numeric_limits<std::int64_t>::max();

struct ScheduledNote {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    /// Identifies the authored note this one was scheduled from, so a kernel
    /// can find every echo of one source without re-deriving it from the
    /// echo's own pitch, which transposition has already changed.
    std::uint16_t group = 0;
    /// Repeat index within the group, used to place the rolled-back end.
    std::uint16_t step = 0;
    bool sounding = false;
    bool active = false;
};

/// Fixed-capacity queue of scheduled notes.
///
/// The queue never allocates. Emission repeatedly scans for the earliest due
/// edge, so notes scheduled out of order still leave in sample order, and a
/// release at the same sample as an attack leaves first.
template <std::size_t Capacity> class ScheduleQueue {
  public:
    static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    /// Reserve a slot. Returns nullptr when the queue is full, which the caller
    /// reports rather than silently dropping.
    ScheduledNote* acquire() noexcept {
        for (auto& slot : slots_) {
            if (slot.active)
                continue;
            slot = {};
            slot.active = true;
            return &slot;
        }
        return nullptr;
    }

    bool empty() const noexcept {
        for (const auto& slot : slots_)
            if (slot.active)
                return false;
        return true;
    }

    std::size_t size() const noexcept {
        std::size_t count = 0;
        for (const auto& slot : slots_)
            if (slot.active)
                ++count;
        return count;
    }

    /// Number of scheduled notes that have not started sounding.
    std::size_t unstarted() const noexcept {
        std::size_t count = 0;
        for (const auto& slot : slots_)
            if (slot.active && !slot.sounding)
                ++count;
        return count;
    }

    /// Number of notes currently sounding, which is exactly the number of
    /// note-offs the kernel still owes.
    std::size_t sounding() const noexcept {
        std::size_t count = 0;
        for (const auto& slot : slots_)
            if (slot.active && slot.sounding)
                ++count;
        return count;
    }

    /// Emit every edge strictly before `boundary`, earliest first.
    ///
    /// `emit` receives the event to write and returns false when the output
    /// could not take it; the slot is then left untouched so the caller can
    /// retry it in a later block.
    template <typename Emit> bool emit_due_before(std::int64_t boundary, Emit&& emit) noexcept {
        while (true) {
            ScheduledNote* earliest = nullptr;
            std::int64_t earliest_at = 0;
            bool earliest_attack = false;
            for (auto& slot : slots_) {
                if (!slot.active)
                    continue;
                const bool attack = !slot.sounding;
                const auto at = attack ? slot.start : slot.end;
                if (at >= boundary)
                    continue;
                // Ties resolve release-before-attack so a re-attack of the same
                // pitch at one sample cannot leave the note stuck on.
                const bool better = earliest == nullptr || at < earliest_at ||
                                    (at == earliest_at && !attack && earliest_attack);
                if (better) {
                    earliest = &slot;
                    earliest_at = at;
                    earliest_attack = attack;
                }
            }
            if (earliest == nullptr)
                return true;
            if (!emit(*earliest, earliest_attack, earliest_at))
                return false;
            if (earliest_attack)
                earliest->sounding = true;
            else
                *earliest = {};
        }
    }

    /// Emit every remaining edge regardless of position.
    template <typename Emit> bool drain(Emit&& emit) noexcept {
        return emit_due_before(std::numeric_limits<std::int64_t>::max(), std::forward<Emit>(emit));
    }

    /// Discard every scheduled note that has not started sounding. Because a
    /// slot owns both halves, this cancels the attack and its release together
    /// and can never orphan a note-off.
    std::size_t cancel_unstarted() noexcept {
        std::size_t cancelled = 0;
        for (auto& slot : slots_) {
            if (slot.active && !slot.sounding) {
                slot = {};
                ++cancelled;
            }
        }
        return cancelled;
    }

    /// Discard the unstarted notes for one key only.
    std::size_t cancel_unstarted_key(std::uint8_t channel, std::uint8_t note) noexcept {
        std::size_t cancelled = 0;
        for (auto& slot : slots_) {
            if (slot.active && !slot.sounding && slot.channel == channel && slot.note == note) {
                slot = {};
                ++cancelled;
            }
        }
        return cancelled;
    }

    /// Pull every sounding note's release forward to `at`, used when a flush
    /// must end owned notes now rather than at their authored gate.
    void release_sounding_at(std::int64_t at) noexcept {
        for (auto& slot : slots_)
            if (slot.active && slot.sounding)
                slot.end = std::min(slot.end, at);
    }

    /// Visit every live slot. Used by kernels that must reach a scheduled note
    /// by something other than its pitch, such as the source note an echo was
    /// derived from.
    template <typename Fn> void visit(Fn&& fn) noexcept {
        for (auto& slot : slots_)
            if (slot.active)
                fn(slot);
    }

    template <typename Fn> void visit(Fn&& fn) const noexcept {
        for (const auto& slot : slots_)
            if (slot.active)
                fn(slot);
    }

    void clear() noexcept {
        slots_.fill({});
    }

  private:
    std::array<ScheduledNote, Capacity> slots_{};
};

/// Velocity after `step` applications of a percent decay, floored at 1 so a
/// decayed repeat never turns into a release.
constexpr std::uint8_t decayed_velocity(std::uint8_t velocity, std::uint8_t decay_percent,
                                        std::size_t step) noexcept {
    std::int32_t value = velocity;
    for (std::size_t index = 0; index < step; ++index)
        value = value * static_cast<std::int32_t>(decay_percent) / 100;
    return static_cast<std::uint8_t>(std::clamp(value, 1, 127));
}

/// Clamp an absolute sample position into `block` as a buffer offset.
inline std::int32_t offset_in_block(const Block& block, std::int64_t absolute) noexcept {
    const auto offset = absolute - block.sample_start.value;
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(offset, 0, std::numeric_limits<std::int32_t>::max()));
}

} // namespace pulp::midi::note_schedule
