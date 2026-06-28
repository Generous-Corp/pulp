#pragma once

/// @file aax_midi_packets.hpp
/// SDK-free MIDI/SysEx packet translation for the AAX runtime.
///
/// AAX delivers MIDI as a stream of fixed 4-byte `AAX_CMidiPacket` entries:
/// short channel-voice/system messages are 1..3 bytes in a single packet, and
/// a system-exclusive run is split across consecutive packets with `F0` in the
/// first and `F7` in the last (continuation packets carry no status byte).
/// Getting that reassembly — and the inverse fragmentation on output — right is
/// the most fragile, regression-prone part of the AAX adapter, but it lived
/// inline in `aax_runtime.cpp` against `AAX_*` types and could only be compiled
/// (let alone tested) on a machine with the developer-supplied AAX SDK.
///
/// This header lifts that logic into pure functions over a minimal,
/// SDK-independent packet struct so the state machine can be unit-tested
/// without the SDK. `aax_runtime.cpp` translates `AAX_CMidiPacket` <-> these
/// structs and delegates here, so the tested code is the shipping code.

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::format::aax {

/// SDK-free mirror of the fields of `AAX_CMidiPacket` this layer needs.
/// `data`'s first `length` bytes are valid; `length` is 1..4.
struct MidiPacketBytes {
    std::array<uint8_t, 4> data{};
    uint8_t length = 0;
    uint32_t timestamp = 0;
};

/// Decode a stream of AAX-style packets into @p out (short messages plus
/// reassembled SysEx). Mirrors the AAX runtime's input path exactly:
///   - zero-length packets are skipped;
///   - a packet whose first byte is `0xF0` (when not already mid-SysEx) starts
///     a SysEx run; every byte up to and including `0xF7` is accumulated, and
///     the completed `F0..F7` payload is appended at the start packet's
///     timestamp;
///   - continuation packets (no status byte) are treated as SysEx payload while
///     a run is in progress;
///   - a dangling SysEx (no `F7` before the stream ends) is dropped rather than
///     delivered as a corrupt message;
///   - any non-SysEx packet longer than 3 bytes is malformed and skipped;
///   - short messages become `choc::midi::ShortMessage` at the packet
///     timestamp.
/// @p out is sorted by sample offset before returning.
inline void decode_midi_packets(std::span<const MidiPacketBytes> packets,
                                midi::MidiBuffer& out) {
    std::vector<uint8_t> sysex_buffer;
    bool sysex_in_progress = false;
    int32_t sysex_start_offset = 0;

    for (const auto& packet : packets) {
        if (packet.length == 0) {
            continue;
        }

        const bool starts_sysex =
            (!sysex_in_progress && packet.data[0] == 0xF0);
        if (starts_sysex || sysex_in_progress) {
            if (starts_sysex) {
                sysex_buffer.clear();
                sysex_start_offset = static_cast<int32_t>(packet.timestamp);
                sysex_in_progress = true;
            }
            for (uint8_t b = 0; b < packet.length && b < 4; ++b) {
                const uint8_t byte = packet.data[b];
                sysex_buffer.push_back(byte);
                if (byte == 0xF7) {
                    out.add_sysex(std::move(sysex_buffer), sysex_start_offset,
                                  0.0);
                    sysex_buffer.clear();
                    sysex_in_progress = false;
                    break;
                }
            }
            continue;
        }

        // Short channel-voice or system-common (1..3 bytes).
        if (packet.length > 3) {
            continue;
        }
        const unsigned char data1 = packet.length > 1 ? packet.data[1] : 0;
        const unsigned char data2 = packet.length > 2 ? packet.data[2] : 0;
        out.add(midi::MidiEvent{
            .message = choc::midi::ShortMessage(packet.data[0], data1, data2),
            .sample_offset = static_cast<int32_t>(packet.timestamp),
            .timestamp = 0.0,
        });
    }

    out.sort();
}

/// Fragment one full `F0..F7` SysEx @p payload into consecutive 4-byte packets
/// that all share @p timestamp — the inverse of decode's reassembly. An empty
/// payload yields no packets. Callers set any SDK-specific flags (e.g.
/// `mIsImmediate`) on the returned packets.
inline std::vector<MidiPacketBytes> fragment_sysex(
    std::span<const uint8_t> payload, uint32_t timestamp) {
    std::vector<MidiPacketBytes> packets;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const std::size_t chunk = std::min<std::size_t>(4, payload.size() - offset);
        MidiPacketBytes packet{};
        packet.timestamp = timestamp;
        packet.length = static_cast<uint8_t>(chunk);
        for (std::size_t b = 0; b < chunk; ++b) {
            packet.data[b] = payload[offset + b];
        }
        packets.push_back(packet);
        offset += chunk;
    }
    return packets;
}

} // namespace pulp::format::aax
