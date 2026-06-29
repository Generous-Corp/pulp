// SDK-gated runtime test for the AAX MIDI bridge (decode_midi_node /
// encode_midi_node in aax_midi_node.cpp). test_aax_midi.cpp already covers the
// reassembly/fragmentation state machines SDK-free; this test links the real
// Avid AAX SDK and drives the thin glue through genuine AAX_IMIDINode /
// AAX_CMidiStream / AAX_CMidiPacket types, so it catches any mismatch between
// the SDK packet layout and the SDK-free MidiPacketBytes copy (lengths,
// timestamps, the 4-byte clamp). Only built when PULP_HAS_AAX (see
// test/CMakeLists.txt). #239 AAX parity.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_midi_node.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <AAX.h>
#include <AAX_IMIDINode.h>

#include <cstdint>
#include <vector>

using namespace pulp;
using pulp::format::aax::decode_midi_node;
using pulp::format::aax::encode_midi_node;

namespace {

AAX_CMidiPacket make_packet(uint32_t timestamp, std::vector<uint8_t> bytes) {
    AAX_CMidiPacket p{};
    p.mTimestamp = timestamp;
    p.mLength = static_cast<uint32_t>(bytes.size());
    for (std::size_t i = 0; i < bytes.size() && i < 4; ++i) p.mData[i] = bytes[i];
    p.mIsImmediate = (timestamp == 0);
    return p;
}

// Input node: serves a fixed packet buffer from GetNodeBuffer(). Non-copyable
// because stream_.mBuffer aliases packets_.data().
class FakeInputNode final : public AAX_IMIDINode {
public:
    explicit FakeInputNode(std::vector<AAX_CMidiPacket> packets)
        : packets_(std::move(packets)) {
        stream_.mBufferSize = static_cast<uint32_t>(packets_.size());
        stream_.mBuffer = packets_.empty() ? nullptr : packets_.data();
    }
    FakeInputNode(const FakeInputNode&) = delete;
    FakeInputNode& operator=(const FakeInputNode&) = delete;

    AAX_CMidiStream* GetNodeBuffer() override { return &stream_; }
    AAX_Result PostMIDIPacket(AAX_CMidiPacket*) override { return AAX_SUCCESS; }
    AAX_ITransport* GetTransport() override { return nullptr; }

private:
    std::vector<AAX_CMidiPacket> packets_;
    AAX_CMidiStream stream_{};
};

// Output node: captures every packet posted to it.
class FakeOutputNode final : public AAX_IMIDINode {
public:
    AAX_CMidiStream* GetNodeBuffer() override { return nullptr; }
    AAX_Result PostMIDIPacket(AAX_CMidiPacket* packet) override {
        if (packet) posted.push_back(*packet);
        return AAX_SUCCESS;
    }
    AAX_ITransport* GetTransport() override { return nullptr; }

    std::vector<AAX_CMidiPacket> posted;
};

}  // namespace

TEST_CASE("decode_midi_node passes short messages through, sorted", "[aax][midi]") {
    FakeInputNode node({
        make_packet(5, {0x90, 60, 100}),  // note-on,  later
        make_packet(1, {0xB0, 7, 127}),   // CC,       earlier
    });
    midi::MidiBuffer in;
    decode_midi_node(&node, &in);

    REQUIRE(in.size() == 2);
    // Sorted by sample offset: the CC (offset 1) comes before the note-on (5).
    REQUIRE(in[0].sample_offset == 1);
    REQUIRE(in[0].data()[0] == 0xB0);
    REQUIRE(in[1].sample_offset == 5);
    REQUIRE(in[1].data()[0] == 0x90);
    REQUIRE(in[1].data()[1] == 60);
    REQUIRE(in.sysex().empty());
}

TEST_CASE("decode_midi_node skips zero-length packets", "[aax][midi]") {
    FakeInputNode node({
        make_packet(0, {}),               // empty — must be ignored
        make_packet(2, {0x90, 64, 90}),
        make_packet(0, {}),
    });
    midi::MidiBuffer in;
    decode_midi_node(&node, &in);
    REQUIRE(in.size() == 1);
    REQUIRE(in[0].data()[1] == 64);
}

TEST_CASE("decode_midi_node reassembles SysEx split across packets", "[aax][midi]") {
    // A 10-byte SysEx delivered as three <=4-byte packets. The first carries
    // F0; the middle/last are status-less CONTINUATION packets whose leading
    // byte (here 0x05, 0x09) is < 0xF0 and must be treated as payload, not as a
    // new short message — the exact AAX splitting behavior that trips naive
    // packet walkers.
    FakeInputNode node({
        make_packet(8, {0xF0, 0x7D, 0x01, 0x02}),
        make_packet(8, {0x05, 0x06, 0x07, 0x08}),  // continuation, no status byte
        make_packet(8, {0x09, 0xF7}),              // terminator
    });
    midi::MidiBuffer in;
    decode_midi_node(&node, &in);

    REQUIRE(in.size() == 0);  // nothing leaked into the short-message stream
    REQUIRE(in.sysex().size() == 1);
    const auto& sx = in.sysex()[0];
    REQUIRE(sx.sample_offset == 8);  // timestamp of the F0 packet
    const std::vector<uint8_t> expected{
        0xF0, 0x7D, 0x01, 0x02, 0x05, 0x06, 0x07, 0x08, 0x09, 0xF7};
    REQUIRE(sx.data == expected);
}

TEST_CASE("decode_midi_node drops a dangling SysEx (no F7)", "[aax][midi]") {
    FakeInputNode node({
        make_packet(0, {0xF0, 0x7D, 0x01, 0x02}),
        make_packet(0, {0x03, 0x04, 0x05, 0x06}),  // buffer ends with no F7
    });
    midi::MidiBuffer in;
    decode_midi_node(&node, &in);
    REQUIRE(in.sysex().empty());  // corrupt SysEx is dropped, not delivered
    REQUIRE(in.size() == 0);
}

TEST_CASE("decode_midi_node resumes short messages after a SysEx", "[aax][midi]") {
    FakeInputNode node({
        make_packet(0, {0xF0, 0x7D, 0xF7}),  // complete 3-byte SysEx in one packet
        make_packet(3, {0x90, 67, 80}),      // note-on after it
    });
    midi::MidiBuffer in;
    decode_midi_node(&node, &in);
    REQUIRE(in.sysex().size() == 1);
    REQUIRE(in.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0xF7});
    REQUIRE(in.size() == 1);
    REQUIRE(in[0].data()[0] == 0x90);
}

TEST_CASE("encode_midi_node posts short messages verbatim", "[aax][midi]") {
    midi::MidiBuffer out;
    out.add(midi::MidiEvent{
        .message = choc::midi::ShortMessage(0x90, 60, 100),
        .sample_offset = 12,
        .timestamp = 0.0,
    });
    FakeOutputNode node;
    encode_midi_node(&node, out);

    REQUIRE(node.posted.size() == 1);
    REQUIRE(node.posted[0].mTimestamp == 12);
    REQUIRE(node.posted[0].mLength == 3);
    REQUIRE(node.posted[0].mData[0] == 0x90);
    REQUIRE(node.posted[0].mData[1] == 60);
    REQUIRE(node.posted[0].mData[2] == 100);
}

TEST_CASE("encode_midi_node chunks SysEx into 4-byte packets sharing a timestamp", "[aax][midi]") {
    midi::MidiBuffer out;
    const std::vector<uint8_t> payload{
        0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xF7};  // 10 bytes
    out.add_sysex(payload, /*sample_offset=*/16, /*ts=*/0.0);

    FakeOutputNode node;
    encode_midi_node(&node, out);

    // ceil(10 / 4) == 3 packets, all sharing the source timestamp.
    REQUIRE(node.posted.size() == 3);
    std::vector<uint8_t> reassembled;
    for (const auto& p : node.posted) {
        REQUIRE(p.mTimestamp == 16);
        REQUIRE(p.mLength <= 4);
        for (uint32_t i = 0; i < p.mLength; ++i) reassembled.push_back(p.mData[i]);
    }
    REQUIRE(node.posted[0].mLength == 4);
    REQUIRE(node.posted[1].mLength == 4);
    REQUIRE(node.posted[2].mLength == 2);
    REQUIRE(reassembled == payload);
}

TEST_CASE("AAX MIDI bridge round-trips short + SysEx through the SDK types", "[aax][midi]") {
    midi::MidiBuffer out;
    out.add(midi::MidiEvent{
        .message = choc::midi::ShortMessage(0xB0, 1, 64),
        .sample_offset = 0,
        .timestamp = 0.0,
    });
    out.add_sysex({0xF0, 0x11, 0x22, 0x33, 0x44, 0xF7}, /*sample_offset=*/0, 0.0);

    FakeOutputNode out_node;
    encode_midi_node(&out_node, out);

    // Feed the posted packets straight back into the decoder.
    FakeInputNode in_node(out_node.posted);
    midi::MidiBuffer back;
    decode_midi_node(&in_node, &back);

    REQUIRE(back.size() == 1);
    REQUIRE(back[0].data()[0] == 0xB0);
    REQUIRE(back.sysex().size() == 1);
    REQUIRE(back.sysex()[0].data ==
            std::vector<uint8_t>{0xF0, 0x11, 0x22, 0x33, 0x44, 0xF7});
}
