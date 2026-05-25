#pragma once

/// @file message_collector.hpp
/// SPSC MIDI accumulator for UI-thread → audio-thread MIDI delivery.
///
/// `MidiMessageCollector` lets the UI / virtual-keyboard / scripting
/// thread push timestamped MIDI events into a lock-free queue that the
/// audio callback drains into the current block's `MidiBuffer` at the
/// correct sample offsets.
///
/// Capacity is a compile-time parameter so the underlying queue
/// allocates no heap memory in the audio thread. The default of 256
/// covers typical UI input rates (mouse + scripting) at any
/// block size; larger consumers (sequencers playing back patterns)
/// should pick a higher capacity at construction.
///
/// Usage:
/// @code
///   pulp::midi::MidiMessageCollector<> collector;
///
///   // UI thread:
///   collector.push_now(MidiEvent::note_on(0, 60, 100), now_seconds);
///
///   // Audio thread (each block):
///   collector.drain_into(output_buffer, block_start_seconds,
///                        block_end_seconds, sample_rate);
/// @endcode

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <optional>

namespace pulp::midi {

/// Timestamped MIDI event in the collector queue. The collector
/// holds events as `choc::midi::ShortMessage` + absolute timestamp
/// in seconds; the audio thread converts to sample-offset on drain.
struct TimestampedShortMessage {
    choc::midi::ShortMessage message{};
    double timestamp_seconds = 0.0;
};

template<std::size_t Capacity = 256>
class MidiMessageCollector {
public:
    MidiMessageCollector() = default;

    /// UI / producer thread — push a MidiEvent timestamped at `now_seconds`.
    /// Returns false when the queue is full (caller decides whether to
    /// drop, log, or back off — `push_now` never blocks).
    bool push_now(const MidiEvent& event, double now_seconds) {
        TimestampedShortMessage entry{event.message, now_seconds};
        return queue_.try_push(entry);
    }

    /// Convenience overload using `MidiEvent::timestamp` directly.
    bool push_now(const MidiEvent& event) {
        return push_now(event, event.timestamp);
    }

    /// Audio / consumer thread — drain queued events into `out` at the
    /// correct sample offsets within the current processing block.
    ///
    /// Events whose timestamps are <= @p block_start_seconds land at
    /// sample 0 (catch-up). Events between block_start and block_end
    /// land at the sample offset that matches their timestamp. Events
    /// strictly after `block_end_seconds` stay queued for the next
    /// block.
    ///
    /// `block_size_samples` and `sample_rate` together define the block
    /// length: block_end_seconds = block_start_seconds + block_size_samples / sample_rate.
    ///
    /// Returns the number of events drained into @p out this call.
    std::size_t drain_into(MidiBuffer& out,
                           double block_start_seconds,
                           int block_size_samples,
                           double sample_rate) {
        if (block_size_samples <= 0 || sample_rate <= 0.0) return 0;
        const double block_duration = block_size_samples / sample_rate;
        const double block_end_seconds = block_start_seconds + block_duration;

        std::size_t drained = 0;

        auto deliver = [&](const TimestampedShortMessage& entry) {
            int sample_offset = 0;
            if (entry.timestamp_seconds > block_start_seconds) {
                const double offset_seconds = entry.timestamp_seconds - block_start_seconds;
                // Round to nearest sample — truncation would lose
                // ~1 sample on every tick whose product is not exactly
                // representable in binary float.
                sample_offset = static_cast<int>(std::lround(offset_seconds * sample_rate));
                sample_offset = std::clamp(sample_offset, 0, block_size_samples - 1);
            }
            MidiEvent event{entry.message, sample_offset, entry.timestamp_seconds};
            out.add(event);
        };

        // Walk the consumer-owned `pending_` ring first: any deferred-
        // future entry whose timestamp now fits the current block is
        // delivered. Entries that are still in the future stay put so
        // a later block can consume them. (Pulled out of the queue so
        // the consumer thread never writes back to the SPSC FIFO —
        // Codex P1 on #2843.)
        for (auto& slot : pending_) {
            if (slot.has_value() && slot->timestamp_seconds < block_end_seconds) {
                deliver(*slot);
                slot.reset();
                ++drained;
            }
        }

        // Drain the queue. Events that fit the current block are
        // delivered immediately; future events are stashed in the
        // consumer-owned pending ring. We KEEP draining even after a
        // future event lands so later in-block events still get
        // delivered this block (Codex P1 on #2856 — earlier
        // break-after-stash starved out-of-order in-block items).
        //
        // The pending ring is sized (`kPendingSlots`) to absorb the
        // realistic Pulp workloads in one drain: UI clicks, scripting
        // schedulers with look-ahead, MIDI file playback. When the
        // ring is genuinely saturated, additional FUTURE events have
        // nowhere to live and are dropped — observable via the atomic
        // `dropped_future_` counter so callers can detect pathological
        // back-pressure.
        while (auto next = queue_.try_pop()) {
            const auto& entry = *next;
            if (entry.timestamp_seconds < block_end_seconds) {
                deliver(entry);
                ++drained;
                continue;
            }
            // Future event: try to stash in the pending ring.
            if (stash_pending(entry)) {
                continue;
            }
            // Ring genuinely saturated. Drop with atomic counter.
            dropped_future_.fetch_add(1, std::memory_order_relaxed);
        }
        return drained;
    }

    /// Number of FUTURE events the consumer dropped because the pending
    /// ring was saturated at the moment they were popped. Monotonic,
    /// atomic, never reset. A non-zero value means producers buffered
    /// more events than `pending_capacity()` can carry across drains;
    /// either reduce producer-side look-ahead or pick a larger
    /// `Capacity` so the producer SPSC queue itself absorbs more
    /// in-flight events.
    std::size_t dropped_future() const {
        return dropped_future_.load(std::memory_order_relaxed);
    }

    /// Approximate number of events currently queued (does NOT count
    /// the deferred-future entries in the pending ring or the
    /// consumer-side overflow slot).
    std::size_t size_approx() const { return queue_.size_approx(); }

    /// Compile-time capacity of the producer queue.
    static constexpr std::size_t capacity() { return Capacity; }

    /// Compile-time size of the consumer-owned pending ring. Sized to
    /// absorb realistic Pulp producer bursts (UI input + scripting
    /// look-ahead + MIDI file playback) within a single drain. A
    /// workload that exceeds this in one drain will see drops counted
    /// via `dropped_future()`; raising `Capacity` lets the producer
    /// queue itself absorb more in-flight events before they reach the
    /// consumer ring.
    static constexpr std::size_t pending_capacity() { return kPendingSlots; }

private:
    static constexpr std::size_t kPendingSlots = 64;

    pulp::runtime::SpscQueue<TimestampedShortMessage, Capacity> queue_;
    /// Consumer-owned pending ring for popped-but-future events. Linear
    /// scan; N=64 keeps the per-drain overhead small while absorbing
    /// realistic scripting / playback bursts without consumer-side drops.
    std::array<std::optional<TimestampedShortMessage>, kPendingSlots> pending_{};
    std::atomic<std::size_t> dropped_future_{0};

    /// Place @p entry in an empty pending slot. Returns true if
    /// @p entry was stored, false if all slots are occupied. Eviction
    /// is deliberately not performed here — overflow events stay in
    /// the consumer-side `overflow_` slot or the producer queue so
    /// the contract is zero data loss.
    bool stash_pending(const TimestampedShortMessage& entry) {
        for (auto& slot : pending_) {
            if (!slot.has_value()) {
                slot = entry;
                return true;
            }
        }
        return false; // all slots full
    }
};

} // namespace pulp::midi
