#pragma once

#include <cstdint>
#include <cstddef>
#include <choc/audio/choc_MIDI.h>

namespace pulp::midi {

// MIDI event combining choc::midi::ShortMessage with plugin timing info
// choc::midi provides battle-tested message creation and querying
struct MidiEvent {
    choc::midi::ShortMessage message;
    int32_t sample_offset = 0; // Sample position within buffer (for plugin use)
    double timestamp = 0.0;    // Absolute time in seconds (for device I/O)

    // Raw data access (for format adapters that need direct byte access)
    const uint8_t* data() const { return message.data(); }
    uint32_t size() const { return message.length(); }

    // Factory methods
    static MidiEvent note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0x90 | (channel & 0x0F)), note, velocity), 0, 0.0};
    }

    static MidiEvent note_off(uint8_t channel, uint8_t note, uint8_t velocity = 0) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0x80 | (channel & 0x0F)), note, velocity), 0, 0.0};
    }

    static MidiEvent cc(uint8_t channel, uint8_t controller, uint8_t value) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0xB0 | (channel & 0x0F)), controller, value), 0, 0.0};
    }

    static MidiEvent pitch_bend(uint8_t channel, uint16_t value) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0xE0 | (channel & 0x0F)),
            static_cast<uint8_t>(value & 0x7F),
            static_cast<uint8_t>((value >> 7) & 0x7F)), 0, 0.0};
    }

    static MidiEvent program_change(uint8_t channel, uint8_t program) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0xC0 | (channel & 0x0F)), program, 0), 0, 0.0};
    }

    // Queries — delegate to choc::midi
    bool is_note_on() const  { return message.isNoteOn(); }
    bool is_note_off() const { return message.isNoteOff(); }
    bool is_cc() const       { return message.isController(); }
    bool is_pitch_bend() const { return message.isPitchWheel(); }
    bool is_program_change() const { return message.isProgramChange(); }

    uint8_t channel() const  { return message.getChannel0to15(); }
    uint8_t note() const     { return message.getNoteNumber().note; }
    uint8_t velocity() const { return message.getVelocity(); }
    uint8_t cc_number() const { return message.getControllerNumber(); }
    uint8_t cc_value() const  { return message.getControllerValue(); }
};

} // namespace pulp::midi
