#pragma once

/// @file activity_channel.hpp
/// Lock-free occurrence signals for realtime-producer to UI-consumer feedback.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pulp::runtime {

/// A fixed set of lock-free occurrence counters.
///
/// This channel is for transient UI feedback whose payload is simply "lane N
/// was active": MIDI pad flashes, clipping indicators, voice-activity lights,
/// and similar signals. A producer calls signal() from the audio thread; a UI
/// consumer keeps one Sequence cursor per lane and polls consume(). Multiple
/// signals between UI ticks intentionally coalesce into one observation.
///
/// The channel carries no associated payload, so relaxed atomic ordering is
/// sufficient. It allocates nothing and takes no locks in signal(). Share the
/// channel with a view through SharedActivityChannel so a host that retains an
/// editor after destroying its Processor cannot leave the view polling freed
/// processor memory. Sequence wrap is harmless for ordinary polling; only an
/// exact multiple of 2^32 unconsumed signals can alias the cursor.
template <std::size_t LaneCount>
class ActivityChannel {
    static_assert(LaneCount > 0, "ActivityChannel must have at least one lane");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "ActivityChannel requires lock-free 32-bit atomics");

public:
    using Sequence = std::uint32_t;

    static constexpr std::size_t lane_count() noexcept { return LaneCount; }

    /// Record activity on @p lane. Invalid lanes are ignored so a realtime
    /// producer can fail closed without exceptions, logging, or assertions.
    void signal(std::size_t lane) noexcept {
        if (lane >= LaneCount) return;
        sequences_[lane].fetch_add(1, std::memory_order_relaxed);
    }

    /// Read the current sequence for @p lane. Invalid lanes read as zero.
    Sequence sequence(std::size_t lane) const noexcept {
        if (lane >= LaneCount) return 0;
        return sequences_[lane].load(std::memory_order_relaxed);
    }

    /// Return true once when activity has occurred since @p cursor was last
    /// consumed, updating the cursor to the newest sequence. A burst may
    /// advance by more than one, but is deliberately reported as one visual
    /// wake-up rather than a queue of stale flashes.
    bool consume(std::size_t lane, Sequence& cursor) const noexcept {
        if (lane >= LaneCount) return false;
        const Sequence current = sequence(lane);
        if (current == cursor) return false;
        cursor = current;
        return true;
    }

private:
    std::array<std::atomic<Sequence>, LaneCount> sequences_{};
};

template <std::size_t LaneCount>
using SharedActivityChannel = std::shared_ptr<ActivityChannel<LaneCount>>;

template <std::size_t LaneCount>
SharedActivityChannel<LaneCount> make_activity_channel() {
    return std::make_shared<ActivityChannel<LaneCount>>();
}

} // namespace pulp::runtime
