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
#include <cmath>
#include <cstddef>

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
        while (true) {
            auto peeked = queue_.try_pop();
            if (!peeked) break;
            const auto& entry = *peeked;
            if (entry.timestamp_seconds >= block_end_seconds) {
                // This event belongs to a future block. Push it back so
                // the next drain sees it. SpscQueue is single-reader so
                // a re-push is safe from the drain thread.
                queue_.try_push(entry);
                break;
            }
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
            ++drained;
        }
        return drained;
    }

    /// Approximate number of events currently queued.
    std::size_t size_approx() const { return queue_.size_approx(); }

    /// Compile-time capacity.
    static constexpr std::size_t capacity() { return Capacity; }

private:
    pulp::runtime::SpscQueue<TimestampedShortMessage, Capacity> queue_;
};

} // namespace pulp::midi
