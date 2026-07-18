#pragma once

// MIDI 2.0 Universal MIDI Packet (UMP) support
// Implements the MIDI 2.0 UMP format for high-resolution note events,
// per-note controllers, and backwards compatibility with MIDI 1.0.
//
// Key concepts:
// - UMP: Universal MIDI Packet — 32/64/96/128-bit packets replacing raw bytes
// - Message Type: determines packet size and interpretation
// - Group: 0-15, logical grouping of channels (replaces cables)
// - Channel Voice: note on/off, CC, pitch bend, etc.
// - Per-Note: controllers, pitch bend, management per individual note

#include <cstdint>
#include <array>
#include <string>

namespace pulp::midi {

// ── UMP Message Types ───────────────────────────────────────────────────

enum class UmpMessageType : uint8_t {
    Utility           = 0x0,  // 32-bit: JR Clock, JR Timestamp, delta clockstamp
    System            = 0x1,  // 32-bit: System Real Time, System Common
    Midi1ChannelVoice = 0x2,  // 32-bit: MIDI 1.0 channel voice (backwards compat)
    DataSysEx         = 0x3,  // 64-bit: SysEx, data messages
    Midi2ChannelVoice = 0x4,  // 64-bit: MIDI 2.0 channel voice (high-res)
    Data128           = 0x5,  // 128-bit: 8-byte SysEx, mixed data set
};

// ── UMP word length by message type ─────────────────────────────────────

/// Spec-complete UMP packet length, in 32-bit words, for a raw message-type
/// nibble (bits 28-31 of word 0, values 0x0-0xF). Covers every message type
/// the MIDI 2.0 UMP spec defines — including the reserved ranges and the
/// 128-bit UMP-Stream type (0xF) — so a cursor walking a packet stream
/// advances past an unrecognized message by its true length instead of
/// re-reading its trailing words as a fresh header:
///
///   0x0,0x1,0x2,0x6,0x7  -> 1 word  (32-bit)
///   0x3,0x4,0x8,0x9,0xA  -> 2 words (64-bit)
///   0xB,0xC              -> 3 words (96-bit)
///   0x5,0xD,0xE,0xF      -> 4 words (128-bit)
inline int ump_words_for_message_type(uint8_t message_type) {
    switch (message_type & 0x0F) {
        case 0x0: case 0x1: case 0x2:
        case 0x6: case 0x7:
            return 1;
        case 0x3: case 0x4:
        case 0x8: case 0x9: case 0xA:
            return 2;
        case 0xB: case 0xC:
            return 3;
        case 0x5: case 0xD: case 0xE: case 0xF:
            return 4;
        default:
            return 1;  // unreachable: the nibble is masked to 0x0-0xF
    }
}

// ── UMP Status codes for MIDI 2.0 Channel Voice (type 0x4) ─────────────

enum class Midi2Status : uint8_t {
    RegisteredPerNoteCC  = 0x00,
    AssignablePerNoteCC  = 0x10,
    RegisteredCC         = 0x20,  // RPN
    AssignableCC         = 0x30,  // NRPN
    RelativeRegisteredCC = 0x40,
    RelativeAssignableCC = 0x50,
    PerNotePitchBend     = 0x60,
    NoteOff              = 0x80,
    NoteOn               = 0x90,
    PolyPressure         = 0xA0,
    ControlChange        = 0xB0,
    ProgramChange        = 0xC0,
    ChannelPressure      = 0xD0,
    PitchBend            = 0xE0,
    PerNoteManagement    = 0xF0,
};

// ── UMP Packet ──────────────────────────────────────────────────────────

// A Universal MIDI Packet: 1-4 32-bit words
struct UmpPacket {
    std::array<uint32_t, 4> words = {};
    int word_count = 0;  // 1, 2, 3, or 4

    // Extract fields from word 0
    UmpMessageType message_type() const {
        return static_cast<UmpMessageType>((words[0] >> 28) & 0x0F);
    }

    uint8_t group() const {
        return static_cast<uint8_t>((words[0] >> 24) & 0x0F);
    }

    uint8_t status() const {
        return static_cast<uint8_t>((words[0] >> 16) & 0xFF);
    }

    uint8_t channel() const {
        return static_cast<uint8_t>((words[0] >> 16) & 0x0F);
    }

    // For MIDI 2.0 Channel Voice (64-bit):
    uint8_t note_number() const {
        return static_cast<uint8_t>((words[0] >> 8) & 0x7F);
    }

    // MIDI 2.0 uses 16-bit velocity in word 1
    uint16_t velocity_16() const {
        return static_cast<uint16_t>((words[1] >> 16) & 0xFFFF);
    }

    // Convert 16-bit velocity to 7-bit for MIDI 1.0 compat
    uint8_t velocity_7() const {
        return static_cast<uint8_t>(velocity_16() >> 9);
    }

    // 32-bit data value (for CC, pitch bend, pressure)
    uint32_t data_32() const { return words[1]; }

    // Per-note attribute type (bits 8-15 of word 0)
    uint8_t attribute_type() const {
        return static_cast<uint8_t>(words[0] & 0xFF);
    }

    // Per-note attribute data (bits 0-15 of word 1)
    uint16_t attribute_data() const {
        return static_cast<uint16_t>(words[1] & 0xFFFF);
    }

    // Packet size in words based on message type. Delegates to the
    // spec-complete free function so this and any raw-nibble cursor walk share
    // one word-length table.
    static int size_for_type(UmpMessageType type) {
        return ump_words_for_message_type(static_cast<uint8_t>(type));
    }

    // ── Factory methods ─────────────────────────────────────────────────

    // MIDI 2.0 Note On (64-bit, 16-bit velocity)
    static UmpPacket note_on_2(uint8_t group, uint8_t channel,
                                uint8_t note, uint16_t velocity,
                                uint8_t attr_type = 0, uint16_t attr_data = 0) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0x90 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8)
                    | uint32_t(attr_type);
        p.words[1] = (uint32_t(velocity) << 16) | uint32_t(attr_data);
        return p;
    }

    // MIDI 2.0 Note Off (64-bit, 16-bit velocity)
    static UmpPacket note_off_2(uint8_t group, uint8_t channel,
                                 uint8_t note, uint16_t velocity = 0) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0x80 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8);
        p.words[1] = (uint32_t(velocity) << 16);
        return p;
    }

    // MIDI 2.0 Control Change (64-bit, 32-bit value)
    static UmpPacket cc_2(uint8_t group, uint8_t channel,
                           uint8_t controller, uint32_t value) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0xB0 | (channel & 0x0F)) << 16)
                    | (uint32_t(controller & 0x7F) << 8);
        p.words[1] = value;
        return p;
    }

    // MIDI 2.0 Pitch Bend (64-bit, 32-bit value)
    static UmpPacket pitch_bend_2(uint8_t group, uint8_t channel, uint32_t value) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0xE0 | (channel & 0x0F)) << 16);
        p.words[1] = value;
        return p;
    }

    // MIDI 2.0 Per-Note Pitch Bend
    static UmpPacket per_note_pitch_bend(uint8_t group, uint8_t channel,
                                          uint8_t note, uint32_t value) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0x60 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8);
        p.words[1] = value;
        return p;
    }

    // MIDI 2.0 Registered Per-Note Controller (status 0x00). Byte 2 is
    // the note number, byte 3 is the controller index per the UMP spec.
    static UmpPacket registered_per_note_cc(uint8_t group, uint8_t channel,
                                             uint8_t note, uint8_t cc_index,
                                             uint32_t value) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0x00 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8)
                    | uint32_t(cc_index & 0x7F);
        p.words[1] = value;
        return p;
    }

    // MIDI 1.0 Channel Voice (32-bit backwards compat)
    static UmpPacket midi1_note_on(uint8_t group, uint8_t channel,
                                    uint8_t note, uint8_t velocity) {
        UmpPacket p;
        p.word_count = 1;
        p.words[0] = (0x2u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0x90 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8)
                    | uint32_t(velocity & 0x7F);
        return p;
    }

    // MIDI 2.0 Assignable Per-Note Controller (status 0x10). Byte 2 is
    // the note number, byte 3 is the user-assigned controller index.
    // Distinct from `registered_per_note_cc` (status 0x00) which uses
    // standard registered controller IDs.
    static UmpPacket assignable_per_note_cc(uint8_t group, uint8_t channel,
                                             uint8_t note, uint8_t cc_index,
                                             uint32_t value) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0x10 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8)
                    | uint32_t(cc_index & 0x7F);
        p.words[1] = value;
        return p;
    }

    // MIDI 2.0 Per-Note Management (status 0xF0). Byte 2 is the note
    // number; byte 3 carries flags (bit 0 = reset per-note controllers
    // to default, bit 1 = detach previously-held controllers from the
    // note so they stop tracking it). Word 1 is reserved by the spec.
    static UmpPacket per_note_management(uint8_t group, uint8_t channel,
                                          uint8_t note, uint8_t flags) {
        UmpPacket p;
        p.word_count = 2;
        p.words[0] = (0x4u << 28) | (uint32_t(group & 0x0F) << 24)
                    | (uint32_t(0xF0 | (channel & 0x0F)) << 16)
                    | (uint32_t(note & 0x7F) << 8)
                    | uint32_t(flags);
        p.words[1] = 0;
        return p;
    }

    // Per-Note Management flags (UMP spec):
    static constexpr uint8_t kPerNoteResetControllers = 0x01;
    static constexpr uint8_t kPerNoteDetachControllers = 0x02;
};

// ── UMP packet-stream cursor ─────────────────────────────────────────────

/// Walk one UMP packet's contiguous word array, visiting each complete message
/// in order. `visit(mt, msg_words, msg_word_count)` fires once per message,
/// where `mt` is the message-type nibble, `msg_words` points at the message's
/// first word, and `msg_word_count` is its spec length. The cursor advances by
/// that spec length (see `ump_words_for_message_type`), so a multi-word
/// message's trailing words are never re-read as a fresh header. A message
/// whose declared length runs past `word_count` (a truncated packet) stops the
/// walk without visiting that message, so the visitor may safely read all
/// `msg_word_count` words. Pure and allocation-free — safe on the audio thread.
template <typename Visitor>
inline void walk_ump_packet(const uint32_t* words, uint32_t word_count,
                            Visitor&& visit) {
    if (!words) return;
    uint32_t w = 0;
    while (w < word_count) {
        const uint8_t mt = static_cast<uint8_t>((words[w] >> 28) & 0x0F);
        const uint32_t n = static_cast<uint32_t>(ump_words_for_message_type(mt));
        if (w + n > word_count) break;  // truncated: declared length overruns
        visit(mt, words + w, n);
        w += n;
    }
}

// ── Utility messages (UMP type 0x0) ──────────────────────────────────────

/// Utility message status codes (bits 20-23 of word 0 for type 0x0).
///
/// JR Clock / JR Timestamp carry a 16-bit value in bits 0-15 of word 0
/// measured in 1/31250-second units (~32 µs resolution). They let hosts
/// schedule UMP events with sub-block timing accuracy. JR Clock is the
/// "current time" reference; JR Timestamp is "this packet's event
/// happens at this offset". See the MIDI 2.0 UMP spec § Utility.
enum class UtilityStatus : uint8_t {
    Noop          = 0x0,
    JrClock       = 0x1,
    JrTimestamp   = 0x2,
    DeltaClockstampTicksPerQuarter = 0x3,
    DeltaClockstamp = 0x4,
};

/// Extract the utility status nibble from a type-0 UMP packet (bits 20-23 of word 0).
inline UtilityStatus utility_status(const UmpPacket& p) {
    return static_cast<UtilityStatus>((p.words[0] >> 20) & 0x0F);
}

/// Extract the 16-bit JR Clock / JR Timestamp value from a type-0 UMP
/// packet (bits 0-15 of word 0). Caller must check `utility_status()`
/// first to know which kind of value this is. Returns 0 for non-JR
/// utility messages.
inline uint16_t jr_value_16(const UmpPacket& p) {
    return static_cast<uint16_t>(p.words[0] & 0xFFFF);
}

/// One JR Clock tick is 1/31250 of a second (~32 microseconds).
/// Convenience accessors convert a 16-bit JR value to a time delta.
constexpr double kJrTicksPerSecond = 31250.0;

inline double jr_value_seconds(uint16_t value) {
    return static_cast<double>(value) / kJrTicksPerSecond;
}

inline double jr_value_microseconds(uint16_t value) {
    return static_cast<double>(value) * (1'000'000.0 / kJrTicksPerSecond);
}

/// Build a JR Clock utility packet (status 0x1).
inline UmpPacket make_jr_clock(uint8_t group, uint16_t value) {
    UmpPacket p;
    p.word_count = 1;
    p.words[0] = (0x0u << 28) | (uint32_t(group & 0x0F) << 24)
                | (uint32_t(0x1) << 20)
                | uint32_t(value);
    return p;
}

/// Build a JR Timestamp utility packet (status 0x2).
inline UmpPacket make_jr_timestamp(uint8_t group, uint16_t value) {
    UmpPacket p;
    p.word_count = 1;
    p.words[0] = (0x0u << 28) | (uint32_t(group & 0x0F) << 24)
                | (uint32_t(0x2) << 20)
                | uint32_t(value);
    return p;
}

// ── MPE (MIDI Polyphonic Expression) ────────────────────────────────────

// MPE zone configuration
struct MpeZone {
    uint8_t manager_channel = 0;  // Zone manager (0 for lower, 15 for upper)
    uint8_t member_channels = 0;  // Number of member channels (1-15)

    bool is_lower() const { return manager_channel == 0; }
    bool is_upper() const { return manager_channel == 15; }

    // Check if a channel belongs to this zone's member channels
    bool contains_channel(uint8_t ch) const {
        if (member_channels == 0) return false;
        if (is_lower()) return ch >= 1 && ch <= member_channels;
        if (is_upper()) return ch >= (15 - member_channels) && ch <= 14;
        return false;
    }
};

// MPE configuration for a MIDI port
struct MpeConfig {
    MpeZone lower_zone;  // Manager on channel 0
    MpeZone upper_zone;  // Manager on channel 15

    // Standard single lower zone (most common MPE setup)
    static MpeConfig standard_lower(uint8_t members = 15) {
        MpeConfig cfg;
        cfg.lower_zone = {0, members};
        return cfg;
    }

    // Dual zone (lower + upper)
    static MpeConfig dual(uint8_t lower_members, uint8_t upper_members) {
        MpeConfig cfg;
        cfg.lower_zone = {0, lower_members};
        cfg.upper_zone = {15, upper_members};
        return cfg;
    }

    // Check if a channel is a zone manager
    bool is_manager_channel(uint8_t ch) const {
        return ch == lower_zone.manager_channel || ch == upper_zone.manager_channel;
    }

    // Get the zone a member channel belongs to (nullptr if none)
    const MpeZone* zone_for_channel(uint8_t ch) const {
        if (lower_zone.contains_channel(ch)) return &lower_zone;
        if (upper_zone.contains_channel(ch)) return &upper_zone;
        return nullptr;
    }
};

} // namespace pulp::midi
