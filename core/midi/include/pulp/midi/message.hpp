#pragma once

#include <cstdint>
#include <cstddef>
#include <choc/audio/choc_MIDI.h>

namespace pulp::midi {

/// Timestamped MIDI event combining a choc::midi::ShortMessage with timing info.
///
/// Two timing fields serve different contexts:
/// - @c sample_offset for sample-accurate positioning within a processing block.
/// - @c timestamp for absolute time in device I/O scenarios.
///
/// Factory methods create common message types. Query methods delegate to
/// choc::midi for parsing.
struct MidiEvent {
    choc::midi::ShortMessage message;
    int32_t sample_offset = 0; ///< Sample position within the current buffer block.
    double timestamp = 0.0;    ///< Absolute time in seconds (for device I/O).

    /// Raw MIDI byte data (for format adapters that need direct byte access).
    const uint8_t* data() const { return message.data(); }
    /// Number of bytes in the MIDI message (1-3).
    uint32_t size() const { return message.length(); }

    /// Create a Note On event.
    /// @param channel   MIDI channel (0-15).
    /// @param note      Note number (0-127).
    /// @param velocity  Velocity (0-127).
    static MidiEvent note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0x90 | (channel & 0x0F)), note, velocity), 0, 0.0};
    }

    /// Create a Note Off event.
    static MidiEvent note_off(uint8_t channel, uint8_t note, uint8_t velocity = 0) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0x80 | (channel & 0x0F)), note, velocity), 0, 0.0};
    }

    /// Create a Control Change event.
    /// @param controller  CC number (0-127).
    /// @param value       CC value (0-127).
    static MidiEvent cc(uint8_t channel, uint8_t controller, uint8_t value) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0xB0 | (channel & 0x0F)), controller, value), 0, 0.0};
    }

    /// Create a Pitch Bend event.
    /// @param value  14-bit pitch bend value (0-16383, center = 8192).
    static MidiEvent pitch_bend(uint8_t channel, uint16_t value) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0xE0 | (channel & 0x0F)),
            static_cast<uint8_t>(value & 0x7F),
            static_cast<uint8_t>((value >> 7) & 0x7F)), 0, 0.0};
    }

    /// Create a Program Change event.
    static MidiEvent program_change(uint8_t channel, uint8_t program) {
        return {choc::midi::ShortMessage(
            static_cast<uint8_t>(0xC0 | (channel & 0x0F)), program, 0), 0, 0.0};
    }

    bool is_note_on() const  { return message.isNoteOn(); }
    bool is_note_off() const { return message.isNoteOff(); }
    bool is_cc() const       { return message.isController(); }
    bool is_pitch_bend() const { return message.isPitchWheel(); }
    bool is_program_change() const { return message.isProgramChange(); }

    /// MIDI channel (0-15).
    uint8_t channel() const  { return message.getChannel0to15(); }
    uint8_t note() const     { return message.getNoteNumber().note; }
    uint8_t velocity() const { return message.getVelocity(); }
    uint8_t cc_number() const { return message.getControllerNumber(); }
    uint8_t cc_value() const  { return message.getControllerValue(); }
};

} // namespace pulp::midi
