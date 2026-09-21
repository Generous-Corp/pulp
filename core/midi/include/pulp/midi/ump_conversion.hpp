#pragma once

/// @file ump_conversion.hpp
/// Bidirectional conversion between MIDI 1.0 (MidiBuffer) and MIDI 2.0 UMP
/// (UmpBuffer).
///
/// Scaling follows the MIDI 2.0 spec (M2-115-U §2.4, "Data Value Scaling"):
///   - 7-bit → 16-bit velocity: `v16 = (v7 << 9) | (v7 >> ((v7>0)?5:32))`
///     with v7==0 staying 0 for note-off semantics.
///   - 14-bit → 32-bit: center-biased expansion.
///
/// These helpers are deliberately header-only and allocation-free apart from
/// destination-buffer append behavior. For realtime use, reserve the
/// destination MidiBuffer/UmpBuffer and enable its realtime capacity limit so
/// overflow is counted instead of growing storage.

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/midi/ump.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <cstdint>

namespace pulp::midi {

// ── 7/14/16/32-bit scaling helpers ──────────────────────────────────────

/// Scale a 7-bit value (0..127) to 16-bit (0..65535) per MIDI 2.0 spec.
/// Zero stays zero; 127 maps to 0xFFFF (no dead zone at the top).
inline uint16_t scale_7_to_16(uint8_t v7) {
    // Rounded division so 100 → 51604 round-trips back to 100 (not 99).
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(v7) * 0xFFFFu + 63u) / 127u);
}

/// Scale 16-bit (0..65535) down to 7-bit (0..127).
inline uint8_t scale_16_to_7(uint16_t v16) {
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(v16) * 127u + 0x7FFFu) / 0xFFFFu);
}

/// Scale a 14-bit value (0..16383, center 8192) to 32-bit (center 0x80000000)
/// using MIDI 2.0 Min/Center/Max scaling so the midpoint is preserved exactly.
inline uint32_t scale_14_to_32(uint16_t v14) {
    constexpr uint32_t src_max = 0x3FFFu;
    constexpr uint32_t src_center = 0x2000u;
    constexpr uint64_t dst_center = 0x80000000ull;
    if (v14 <= src_center) {
        return static_cast<uint32_t>((static_cast<uint64_t>(v14) * dst_center) / src_center);
    }
    const uint64_t above = static_cast<uint64_t>(v14 - src_center);
    const uint64_t dst_above_max = 0x7FFFFFFFull;
    return static_cast<uint32_t>(dst_center + (above * dst_above_max) / (src_max - src_center));
}

/// Scale a 32-bit value back to 14-bit (0..16383), inverse of `scale_14_to_32`.
inline uint16_t scale_32_to_14(uint32_t v32) {
    constexpr uint32_t dst_max = 0x3FFFu;
    constexpr uint32_t dst_center = 0x2000u;
    constexpr uint64_t src_center = 0x80000000ull;
    if (v32 <= src_center) {
        return static_cast<uint16_t>((static_cast<uint64_t>(v32) * dst_center) / src_center);
    }
    const uint64_t above = static_cast<uint64_t>(v32 - src_center);
    const uint64_t src_above_max = 0x7FFFFFFFull;
    return static_cast<uint16_t>(dst_center + (above * (dst_max - dst_center)) / src_above_max);
}

// ── MIDI 1.0 → UMP ──────────────────────────────────────────────────────

/// Convert a single MIDI 1.0 event to a MIDI 2.0 Channel Voice UMP packet.
/// Uses message type 0x4 so the host sees full MIDI 2.0 resolution.
/// Group defaults to 0.
inline UmpPacket midi1_event_to_ump2(const MidiEvent& ev, uint8_t group = 0) {
    const uint8_t status = ev.data()[0] & 0xF0;
    const uint8_t channel = ev.channel();

    if (status == 0x90 && ev.velocity() == 0) {
        // Velocity-0 note-on is a note-off.
        return UmpPacket::note_off_2(group, channel, ev.note(), 0);
    }
    switch (status) {
        case 0x80:
            return UmpPacket::note_off_2(group, channel, ev.note(),
                                          scale_7_to_16(ev.velocity()));
        case 0x90:
            return UmpPacket::note_on_2(group, channel, ev.note(),
                                         scale_7_to_16(ev.velocity()));
        case 0xB0:
            return UmpPacket::cc_2(group, channel, ev.cc_number(),
                                    static_cast<uint32_t>(ev.cc_value()) << 25);
        case 0xE0: {
            const uint16_t raw = static_cast<uint16_t>(
                ev.data()[1] | (uint16_t(ev.data()[2]) << 7));
            return UmpPacket::pitch_bend_2(group, channel, scale_14_to_32(raw));
        }
        default:
            break;
    }
    // Fall back to a "raw bytes" UMP carrying the original status + data.
    // System messages (status >= 0xF0: real-time clock/start/stop/active-
    // sensing and system-common like MTC / song-position) are UMP Type 0x1
    // (System Real Time and System Common). Everything else that reaches
    // here (program change, channel pressure, …) is a MIDI 1.0 Channel
    // Voice message and stays Type 0x2. Encoding system messages as Type
    // 0x2 produced malformed UMP that transport/clock-sync consumers reject.
    const uint8_t status_byte = ev.data()[0];
    const uint32_t mt = (status_byte >= 0xF0) ? 0x1u : 0x2u;
    UmpPacket p;
    p.word_count = 1;
    p.words[0] = (mt << 28)
               | (uint32_t(group & 0x0F) << 24)
               | (uint32_t(status_byte) << 16)
               | (uint32_t(ev.size() > 1 ? ev.data()[1] : 0) << 8)
               | uint32_t(ev.size() > 2 ? ev.data()[2] : 0);
    return p;
}

/// Append a MIDI 1.0 buffer to a UmpBuffer as MIDI 2.0 UMP packets.
inline void midi1_to_ump(const MidiBuffer& src, UmpBuffer& dst, uint8_t group = 0) {
    for (const auto& ev : src) {
        dst.add({midi1_event_to_ump2(ev, group), ev.sample_offset});
    }
}

// ── UMP → MIDI 1.0 ──────────────────────────────────────────────────────

/// Decoded fields of a MIDI 2.0 Channel Voice Program Change (status 0xC).
///
/// UMP layout (MIDI 2.0 UMP spec, MIDI 2.0 Program Change Message):
///   word 0: [mt=0x4][group][status=0xC][channel][reserved][option flags]
///   word 1: [program][reserved][bank MSB][bank LSB]
/// Option-flag bit 0 is "Bank Valid"; when it is clear the bank bytes are
/// reserved and carry no meaning. Each of program / bank MSB / bank LSB
/// occupies the low 7 bits of its byte.
struct Midi2ProgramChange {
    uint8_t program = 0;     ///< 0..127
    bool bank_valid = false; ///< option-flag bit 0
    uint8_t bank_msb = 0;    ///< 0..127, meaningful only when bank_valid
    uint8_t bank_lsb = 0;    ///< 0..127, meaningful only when bank_valid
};

/// Decode a MIDI 2.0 Program Change packet's fields without converting it.
/// Pure: depends only on @p p. Callers that need the complete MIDI 1.0
/// rendering of a bank-valid program change (CC 0, CC 32, then the program
/// change) can build it from this, which `ump_to_midi1_event` alone cannot
/// express through its single-event out-parameter.
inline Midi2ProgramChange ump_program_change_fields(const UmpPacket& p) {
    Midi2ProgramChange f;
    f.program = static_cast<uint8_t>((p.words[1] >> 24) & 0x7F);
    f.bank_valid = (p.words[0] & 0x01u) != 0;
    f.bank_msb = static_cast<uint8_t>((p.words[1] >> 8) & 0x7F);
    f.bank_lsb = static_cast<uint8_t>(p.words[1] & 0x7F);
    return f;
}

/// MIDI 1.0 controller numbers carrying Bank Select for a program change.
inline constexpr uint8_t kBankSelectMsbCc = 0;
inline constexpr uint8_t kBankSelectLsbCc = 32;

/// Convert a UMP Channel Voice packet into a MIDI 1.0 event when possible.
/// Returns true on success and writes to @p out. Returns false for packets
/// that have no MIDI 1.0 equivalent (e.g. per-note pitch bend, per-note CC),
/// which callers typically route via the MPE sidecar instead.
///
/// Emits at most one event. The one message whose faithful MIDI 1.0 rendering
/// needs more than that is a bank-valid MIDI 2.0 program change (CC 0, CC 32,
/// program change); here it converts to the program change alone. Use
/// `ump_program_change_fields` to read the bank, or `ump_to_midi1`, which
/// emits the complete sequence.
inline bool ump_to_midi1_event(const UmpPacket& p, MidiEvent& out) {
    const auto mt = p.message_type();
    const uint8_t ch = p.channel();

    if (mt == UmpMessageType::Midi1ChannelVoice) {
        // Byte 1 is status|channel (already in words[0] bits 16-23),
        // Byte 2 in bits 8-15, byte 3 in bits 0-7.
        const uint8_t s = static_cast<uint8_t>((p.words[0] >> 16) & 0xFF);
        const uint8_t d1 = static_cast<uint8_t>((p.words[0] >> 8) & 0xFF);
        const uint8_t d2 = static_cast<uint8_t>(p.words[0] & 0xFF);
        out = {choc::midi::ShortMessage(s, d1, d2), 0, 0.0};
        return true;
    }
    if (mt == UmpMessageType::System) {
        // System Real Time (clock 0xF8, start/continue/stop, active sensing)
        // and System Common (MTC 0xF1, song position 0xF2, …). The UMP carries
        // the status byte plus up to two data bytes in the same byte layout as
        // a MIDI 1.0 Channel Voice UMP; ShortMessage derives the correct
        // length from the status byte. Dropping these (as the prior
        // channel-voice-only path did) silently loses clock/transport input.
        const uint8_t s = static_cast<uint8_t>((p.words[0] >> 16) & 0xFF);
        const uint8_t d1 = static_cast<uint8_t>((p.words[0] >> 8) & 0xFF);
        const uint8_t d2 = static_cast<uint8_t>(p.words[0] & 0xFF);
        out = {choc::midi::ShortMessage(s, d1, d2), 0, 0.0};
        return true;
    }
    if (mt != UmpMessageType::Midi2ChannelVoice) return false;

    const uint8_t status_byte = static_cast<uint8_t>((p.words[0] >> 16) & 0xF0);
    switch (status_byte) {
        case 0x80:
            out = MidiEvent::note_off(ch, p.note_number(),
                                      scale_16_to_7(p.velocity_16()));
            return true;
        case 0x90: {
            // MIDI 2.0 velocity 0 is *not* a note-off (that's an explicit
            // 0x80 status). Clamp to 1 so the MIDI 1.0 representation
            // stays semantically a note-on.
            const uint16_t v16 = p.velocity_16();
            uint8_t v7 = scale_16_to_7(v16);
            if (v16 > 0 && v7 == 0) v7 = 1;
            out = MidiEvent::note_on(ch, p.note_number(), v7);
            return true;
        }
        case 0xB0:
            out = MidiEvent::cc(ch, static_cast<uint8_t>((p.words[0] >> 8) & 0x7F),
                                static_cast<uint8_t>(p.data_32() >> 25));
            return true;
        case 0xC0: {
            // Program change is NOT shaped like the CC or pressure cases: the
            // program lives in the TOP byte of word 1, not in a 32-bit data
            // value, and word 0's low byte is an option-flag field rather than
            // a controller index.
            //
            // Known limit: a bank-valid program change is three MIDI 1.0
            // messages (CC 0 bank MSB, CC 32 bank LSB, then the program
            // change). This function has a single MidiEvent out-parameter and
            // so can only emit the program change; the bank is not
            // representable here. It is not silently discarded — the bytes
            // stay readable via `ump_program_change_fields`, and the
            // buffer-level `ump_to_midi1` emits the full three-message
            // sequence. A caller using this single-event entry point directly
            // must consult `ump_program_change_fields` if it needs the bank.
            const auto fields = ump_program_change_fields(p);
            out = MidiEvent::program_change(ch, fields.program);
            return true;
        }
        case 0xE0:
            out = MidiEvent::pitch_bend(ch, scale_32_to_14(p.data_32()));
            return true;
        default:
            return false;
    }
}

/// True when @p p is a MIDI 2.0 Program Change carrying a valid bank, i.e.
/// one whose faithful MIDI 1.0 rendering needs more than a single event.
inline bool ump_is_bank_valid_program_change(const UmpPacket& p) {
    if (p.message_type() != UmpMessageType::Midi2ChannelVoice)
        return false;
    if ((static_cast<uint8_t>((p.words[0] >> 16) & 0xF0)) != 0xC0)
        return false;
    return ump_program_change_fields(p).bank_valid;
}

/// Flatten a UmpBuffer into a MidiBuffer. Packets that have no MIDI 1.0
/// equivalent are skipped; if you care about them, use the MPE sidecar.
///
/// One packet is not always one event: a MIDI 2.0 program change whose
/// Bank Valid option flag is set expands to the three MIDI 1.0 messages that
/// carry the same meaning — CC 0 (bank MSB), CC 32 (bank LSB), then the
/// program change — all at the source packet's sample offset and in that
/// order, because a MIDI 1.0 receiver latches the bank bytes and applies them
/// on the following program change.
inline void ump_to_midi1(const UmpBuffer& src, MidiBuffer& dst) {
    for (const auto& ue : src) {
        if (ump_is_bank_valid_program_change(ue.packet)) {
            const auto fields = ump_program_change_fields(ue.packet);
            const uint8_t ch = ue.packet.channel();
            auto msb = MidiEvent::cc(ch, kBankSelectMsbCc, fields.bank_msb);
            msb.sample_offset = ue.sample_offset;
            dst.add(msb);
            auto lsb = MidiEvent::cc(ch, kBankSelectLsbCc, fields.bank_lsb);
            lsb.sample_offset = ue.sample_offset;
            dst.add(lsb);
        }
        MidiEvent ev{};
        if (ump_to_midi1_event(ue.packet, ev)) {
            ev.sample_offset = ue.sample_offset;
            dst.add(ev);
        }
    }
}

} // namespace pulp::midi
