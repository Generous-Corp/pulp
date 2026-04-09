#pragma once

// Android MIDI input ring buffer.
//
// MIDI bytes arrive from the Kotlin side on the MidiReceiver callback
// thread. They get pushed into this lock-free SPSC queue and the audio
// thread drains them into a MidiBuffer at the start of each process
// block, converting the monotonic nanosecond timestamp into a sample
// offset within the current block.
//
// This is an INTERNAL Phase-1 API. External consumers should use
// `pulp::midi::android::drain_into(MidiBuffer&, ...)` from the audio
// thread.

#if defined(__ANDROID__)

#include <pulp/midi/buffer.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <array>
#include <cstdint>

namespace pulp::midi::android {

// A single MIDI event delivered from the Kotlin bridge.
// `data` is the raw MIDI byte stream (bytestream MIDI 1.0); `size`
// holds the number of valid bytes in `data`. `timestamp_ns` is the
// Android monotonic timestamp the Kotlin receiver gave us.
struct AndroidMidiEvent {
    std::uint64_t timestamp_ns = 0;
    std::uint8_t  size = 0;
    // Long enough for any MIDI 1.0 message (SysEx is chunked externally).
    std::array<std::uint8_t, 16> data{};
};

// Push raw MIDI bytes into the FIFO. Returns false if the queue is
// full or if `count` > sizeof(AndroidMidiEvent::data) and the caller
// should split the message. Safe to call from any thread (designed for
// the Kotlin MidiReceiver thread).
bool push_bytes(const std::uint8_t* bytes, int count, std::int64_t timestamp_ns);

// Drain pending events into `buffer`, converting timestamps to sample
// offsets relative to the current block. `block_start_ns` is the
// monotonic timestamp of the first sample of the block (or 0 if
// unknown, in which case every event maps to sample 0). `sample_rate`
// is used to convert nanoseconds to samples. `block_size` clamps the
// maximum sample offset so out-of-block events appear at the end of
// the block (they will be applied next block if clamped to the last
// sample — this is the simplest latency-correct behavior).
//
// Call from the audio thread.
void drain_into(MidiBuffer& buffer,
                std::int64_t block_start_ns,
                double sample_rate,
                int block_size);

// Returns true if at least one event is currently queued. For
// diagnostics only — do not use for control flow, since the producer
// may push a new event between the check and a subsequent drain.
bool has_pending();

} // namespace pulp::midi::android

#endif // __ANDROID__
