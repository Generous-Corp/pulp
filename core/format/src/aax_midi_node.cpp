// AAX MIDI node <-> pulp::midi::MidiBuffer bridge. See aax_midi_node.hpp.
//
// Moved out of aax_runtime.cpp's anonymous namespace so the SDK glue is
// reachable by the SDK-gated unit test (test/test_aax_midi_node.cpp), while
// keeping the runtime behavior identical. The reassembly/fragmentation state
// machines stay SDK-free in aax_midi_packets.hpp (unit-tested without the SDK
// in test/test_aax_midi.cpp); this file only copies bytes/timestamps between
// AAX_CMidiPacket and the SDK-free MidiPacketBytes. #239 AAX parity.

#include <pulp/format/aax_midi_node.hpp>

#include <pulp/format/aax_midi_packets.hpp>
#include <pulp/midi/message.hpp>

#include <AAX.h>
#include <AAX_IMIDINode.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace pulp::format::aax {

void decode_midi_node(AAX_IMIDINode* node, midi::MidiBuffer* midi_in) {
    if (!node || !midi_in) {
        return;
    }

    auto* stream = node->GetNodeBuffer();
    if (!stream || !stream->mBuffer) {
        return;
    }

    // Translate the SDK packet stream into the SDK-free MidiPacketBytes the
    // unit-tested reassembler in aax_midi_packets.hpp understands, then let it
    // handle the F0/continuation/F7 state machine, malformed-packet skipping,
    // dangling-sysex drop, and sort. Keeping the algorithm SDK-free is what
    // makes it testable without the AAX SDK (see test/test_aax_midi.cpp).
    std::vector<MidiPacketBytes> packets;
    packets.reserve(stream->mBufferSize);
    for (uint32_t index = 0; index < stream->mBufferSize; ++index) {
        const auto& packet = stream->mBuffer[index];
        MidiPacketBytes mp{};
        mp.length = static_cast<uint8_t>(std::min<uint32_t>(packet.mLength, 4));
        for (uint8_t b = 0; b < mp.length; ++b) {
            mp.data[b] = static_cast<uint8_t>(packet.mData[b]);
        }
        mp.timestamp = packet.mTimestamp;
        packets.push_back(mp);
    }
    decode_midi_packets(packets, *midi_in);
}

void encode_midi_node(AAX_IMIDINode* node, const midi::MidiBuffer& midi_out) {
    if (!node) {
        return;
    }

    // Short channel-voice messages first (unchanged).
    for (const auto& event : midi_out) {
        if (event.size() == 0 || event.size() > 3) {
            continue;
        }

        AAX_CMidiPacket packet{};
        packet.mTimestamp = static_cast<uint32_t>(std::max(0, event.sample_offset));
        packet.mLength = event.size();
        std::memcpy(packet.mData, event.data(), packet.mLength);
        packet.mIsImmediate = (packet.mTimestamp == 0);
        node->PostMIDIPacket(&packet);
    }

    // Sysex outbound — chunk each SysexEvent into 4-byte AAX packets via the
    // SDK-free fragmenter (the inverse of decode's reassembly; unit-tested in
    // test/test_aax_midi.cpp). The full F0..F7 byte stream is delivered as
    // ceil(N / 4) consecutive packets sharing the same timestamp.
    for (const auto& sx : midi_out.sysex()) {
        if (sx.data.empty()) continue;
        const uint32_t ts = static_cast<uint32_t>(
            std::max(0, sx.sample_offset));
        auto fragments = fragment_sysex(
            std::span<const uint8_t>(sx.data.data(), sx.data.size()), ts);
        for (std::size_t k = 0; k < fragments.size(); ++k) {
            const auto& mp = fragments[k];
            AAX_CMidiPacket packet{};
            packet.mTimestamp = mp.timestamp;
            packet.mLength = mp.length;
            std::memcpy(packet.mData, mp.data.data(), mp.length);
            // mIsImmediate flags the first chunk only; continuation
            // packets ride the same timestamp and shouldn't re-fire
            // the immediate-dispatch path.
            packet.mIsImmediate = (k == 0 && ts == 0);
            node->PostMIDIPacket(&packet);
        }
    }
}

} // namespace pulp::format::aax
