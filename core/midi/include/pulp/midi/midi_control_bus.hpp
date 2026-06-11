#pragma once

/// @file midi_control_bus.hpp
/// Lock-free publication of MIDI events from a UI control to the audio
/// thread.
///
/// A UI control that should emit MIDI (a pad that plays notes, a knob
/// mapped to a CC) cannot touch the plugin's MIDI-out buffer directly —
/// that buffer is filled on the audio thread inside `process()`. The UI
/// thread `send()`s events into this bus; the audio thread `drain()`s
/// them into `midi_out` at the top of the block. Single writer (UI),
/// single reader (audio); never blocks either side and allocates nothing
/// after construction.
///
/// Events arrive in the block they are drained in (sample offset 0). For
/// a UI gesture that is the natural quantization — the event lands at the
/// start of the next audio buffer.

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <cstddef>
#include <cstdint>

namespace pulp::midi {

class MidiControlBus {
public:
    /// UI thread: queue a MIDI event for the audio thread to emit.
    /// Returns false if the queue is momentarily full (event dropped).
    bool send(const MidiEvent& event) { return queue_.try_push(event); }

    /// Convenience senders (UI thread).
    bool send_cc(uint8_t channel, uint8_t cc, uint8_t value) {
        return send(MidiEvent::cc(channel, cc, value));
    }
    bool send_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
        return send(MidiEvent::note_on(channel, note, velocity));
    }
    bool send_note_off(uint8_t channel, uint8_t note) {
        return send(MidiEvent::note_off(channel, note));
    }

    /// Audio thread: drain every queued event into `out` (sample offset 0).
    /// Call once at the top of `process()`. RT-safe.
    void drain_into(MidiBuffer& out) {
        while (auto event = queue_.try_pop()) {
            event->sample_offset = 0;
            out.add(*event);
        }
    }

    /// Audio thread: drain into an arbitrary sink `void(const MidiEvent&)`.
    template <typename Fn>
    void drain(Fn&& sink) {
        while (auto event = queue_.try_pop())
            sink(*event);
    }

private:
    static constexpr std::size_t kCapacity = 256;
    pulp::runtime::SpscQueue<MidiEvent, kCapacity> queue_;
};

} // namespace pulp::midi
