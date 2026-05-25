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

        // Drain the queue without losing future events. Events that
        // fit the current block are delivered immediately; future events
        // are stashed in the pending ring. When the ring is full, the
        // popped future event is parked in a single consumer-side
        // lookahead slot (`overflow_`) and the drain stops — so the
        // *remaining* queue items stay in the SPSC FIFO untouched and
        // get retried on the next drain when the ring has room.
        // Zero data loss; no consumer-side write to the SPSC queue
        // (Codex P1 on #2843, #2845, #2853).
        //
        // The next-event source is either the consumer-side overflow
        // slot (carried from the previous drain) or a fresh pop.
        while (true) {
            std::optional<TimestampedShortMessage> next;
            if (overflow_.has_value()) {
                next = std::move(overflow_);
                overflow_.reset();
            } else {
                next = queue_.try_pop();
            }
            if (!next.has_value()) break;
            const auto& entry = *next;
            if (entry.timestamp_seconds < block_end_seconds) {
                deliver(entry);
                ++drained;
                continue;
            }
            // Future event: try to stash in the pending ring.
            if (stash_pending(entry)) {
                continue; // ring had room — keep draining
            }
            // Ring full: park in the consumer-side overflow slot and
            // stop draining so the remaining queue items survive to
            // the next drain.
            overflow_ = entry;
            break;
        }
        return drained;
    }

    /// Approximate number of events currently queued (does NOT count
    /// the deferred-future entries in the pending ring or the
    /// consumer-side overflow slot).
    std::size_t size_approx() const { return queue_.size_approx(); }

    /// Compile-time capacity of the producer queue.
    static constexpr std::size_t capacity() { return Capacity; }

    /// Compile-time size of the consumer-owned pending ring. Large
    /// enough to absorb out-of-order producer bursts in practical
    /// usage (UI + scripting timing). If a workload routinely fills
    /// the ring, picking a larger value reduces per-drain overhead
    /// (extra ring slots get scanned first instead of going through
    /// the overflow lookahead path).
    static constexpr std::size_t pending_capacity() { return kPendingSlots; }

private:
    static constexpr std::size_t kPendingSlots = 8;

    pulp::runtime::SpscQueue<TimestampedShortMessage, Capacity> queue_;
    /// Consumer-owned pending ring for popped-but-future events.
    /// Linear scan; N=8 keeps the per-drain overhead negligible.
    std::array<std::optional<TimestampedShortMessage>, kPendingSlots> pending_{};
    /// One-slot lookahead for a future event the ring couldn't store —
    /// carried into the next drain so no event is ever silently dropped
    /// on the consumer side.
    std::optional<TimestampedShortMessage> overflow_;

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
