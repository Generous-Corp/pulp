#pragma once

#include <cstdint>
#include <cstddef>

namespace pulp::midi {

// Lightweight MIDI event — 3 bytes + timestamp
// Designed to be small and copyable for lock-free queues
struct MidiEvent {
    uint8_t data[3] = {0, 0, 0};
    uint8_t size = 0;         // 1, 2, or 3 bytes
    int32_t sample_offset = 0; // Sample position within buffer (for plugin use)
    double timestamp = 0.0;    // Absolute time in seconds (for device I/O)

    // Factory methods
    static MidiEvent note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
        return {{static_cast<uint8_t>(0x90 | (channel & 0x0F)), note, velocity}, 3, 0, 0.0};
    }

    static MidiEvent note_off(uint8_t channel, uint8_t note, uint8_t velocity = 0) {
        return {{static_cast<uint8_t>(0x80 | (channel & 0x0F)), note, velocity}, 3, 0, 0.0};
    }

    static MidiEvent cc(uint8_t channel, uint8_t controller, uint8_t value) {
        return {{static_cast<uint8_t>(0xB0 | (channel & 0x0F)), controller, value}, 3, 0, 0.0};
    }

    static MidiEvent pitch_bend(uint8_t channel, uint16_t value) {
        return {{static_cast<uint8_t>(0xE0 | (channel & 0x0F)),
                 static_cast<uint8_t>(value & 0x7F),
                 static_cast<uint8_t>((value >> 7) & 0x7F)}, 3, 0, 0.0};
    }

    static MidiEvent program_change(uint8_t channel, uint8_t program) {
        return {{static_cast<uint8_t>(0xC0 | (channel & 0x0F)), program, 0}, 2, 0, 0.0};
    }

    // Queries
    bool is_note_on() const  { return (data[0] & 0xF0) == 0x90 && data[2] > 0; }
    bool is_note_off() const { return (data[0] & 0xF0) == 0x80 || ((data[0] & 0xF0) == 0x90 && data[2] == 0); }
    bool is_cc() const       { return (data[0] & 0xF0) == 0xB0; }
    bool is_pitch_bend() const { return (data[0] & 0xF0) == 0xE0; }
    bool is_program_change() const { return (data[0] & 0xF0) == 0xC0; }

    uint8_t channel() const  { return data[0] & 0x0F; }
    uint8_t note() const     { return data[1]; }
    uint8_t velocity() const { return data[2]; }
    uint8_t cc_number() const { return data[1]; }
    uint8_t cc_value() const  { return data[2]; }
};

} // namespace pulp::midi
