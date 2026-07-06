#include <catch2/catch_test_macros.hpp>

#include <pulp/format/aax_midi_packets.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace pulp;
using pulp::format::aax::MidiPacketBytes;
using pulp::format::aax::decode_midi_packets;
using pulp::format::aax::fragment_sysex;

namespace {

MidiPacketBytes packet(std::vector<uint8_t> bytes, uint32_t ts = 0) {
    MidiPacketBytes p{};
    p.length = static_cast<uint8_t>(bytes.size());
    for (std::size_t i = 0; i < bytes.size() && i < 4; ++i) {
        p.data[i] = bytes[i];
    }
    p.timestamp = ts;
    return p;
}

std::vector<uint8_t> sysex_bytes(const midi::MidiBuffer& buf, std::size_t index) {
    return buf.sysex()[index].data.to_vector();
}

} // namespace

TEST_CASE("AAX: decode reassembles multi-packet sysex across the 4-byte boundary",
          "[aax][midi]") {
    std::array<MidiPacketBytes, 2> packets{
        packet({0xF0, 0x12, 0x34, 0x56}, 0),
        packet({0x78, 0x9A, 0xF7}, 0),
    };
    midi::MidiBuffer out;
    decode_midi_packets(packets, out);

    REQUIRE(out.sysex_size() == 1);
    REQUIRE(sysex_bytes(out, 0) ==
            std::vector<uint8_t>{0xF0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xF7});
    REQUIRE(out.sysex()[0].sample_offset == 0);
    REQUIRE(out.size() == 0); // no short messages
}

TEST_CASE("AAX: decode stops a sysex at F7 mid-packet and ignores trailing bytes",
          "[aax][midi]") {
    std::array<MidiPacketBytes, 1> packets{
        packet({0xF0, 0x7E, 0xF7, 0x00}, 0),
    };
    midi::MidiBuffer out;
    decode_midi_packets(packets, out);
    REQUIRE(out.sysex_size() == 1);
    REQUIRE(sysex_bytes(out, 0) == std::vector<uint8_t>{0xF0, 0x7E, 0xF7});
}

TEST_CASE("AAX: decode drops a dangling sysex with no terminator",
          "[aax][midi]") {
    std::array<MidiPacketBytes, 1> packets{
        packet({0xF0, 0x01, 0x02, 0x03}, 0), // F0 with no F7 before stream ends
    };
    midi::MidiBuffer out;
    decode_midi_packets(packets, out);
    REQUIRE(out.sysex_size() == 0);
    REQUIRE(out.size() == 0);
}

TEST_CASE("AAX: decode maps short messages with timestamp as sample offset",
          "[aax][midi]") {
    std::array<MidiPacketBytes, 2> packets{
        packet({0x90, 60, 100}, 5),   // note on, ch0
        packet({0xB0, 7, 64}, 1),     // CC, earlier offset
    };
    midi::MidiBuffer out;
    decode_midi_packets(packets, out);

    REQUIRE(out.size() == 2);
    // Sorted by sample offset: CC (offset 1) before note-on (offset 5).
    REQUIRE(out[0].sample_offset == 1);
    REQUIRE(out[0].is_cc());
    REQUIRE(out[1].sample_offset == 5);
    REQUIRE(out[1].is_note_on());
    REQUIRE(out[1].note() == 60);
    REQUIRE(out[1].velocity() == 100);
}

TEST_CASE("AAX: decode skips a malformed >3-byte non-sysex packet",
          "[aax][midi]") {
    std::array<MidiPacketBytes, 1> packets{
        packet({0x90, 1, 2, 3}, 0), // 4 bytes, not a sysex start: malformed
    };
    midi::MidiBuffer out;
    decode_midi_packets(packets, out);
    REQUIRE(out.size() == 0);
    REQUIRE(out.sysex_size() == 0);
}

TEST_CASE("AAX: fragment_sysex splits a payload into 4-byte packets",
          "[aax][midi]") {
    const std::vector<uint8_t> payload{0xF0, 0x11, 0x22, 0x33,
                                       0x44, 0x55, 0x66, 0xF7};
    auto packets = fragment_sysex(payload, 10);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[0].length == 4);
    REQUIRE(packets[0].timestamp == 10);
    REQUIRE(std::vector<uint8_t>(packets[0].data.begin(),
                                 packets[0].data.begin() + 4) ==
            std::vector<uint8_t>{0xF0, 0x11, 0x22, 0x33});
    REQUIRE(packets[1].length == 4);
    REQUIRE(packets[1].timestamp == 10);
    REQUIRE(std::vector<uint8_t>(packets[1].data.begin(),
                                 packets[1].data.begin() + 4) ==
            std::vector<uint8_t>{0x44, 0x55, 0x66, 0xF7});
}

TEST_CASE("AAX: fragment_sysex handles a non-multiple-of-4 tail",
          "[aax][midi]") {
    const std::vector<uint8_t> payload{0xF0, 0x11, 0x22, 0x33, 0xF7};
    auto packets = fragment_sysex(payload, 0);
    REQUIRE(packets.size() == 2);
    REQUIRE(packets[0].length == 4);
    REQUIRE(packets[1].length == 1);
    REQUIRE(packets[1].data[0] == 0xF7);

    REQUIRE(fragment_sysex({}, 0).empty());
}

TEST_CASE("AAX: fragment -> decode round-trips an arbitrary sysex payload",
          "[aax][midi]") {
    const std::vector<uint8_t> payload{0xF0, 0x7D, 0x01, 0x02, 0x03,
                                       0x04, 0x05, 0x06, 0x07, 0xF7};
    auto fragments = fragment_sysex(payload, 3);
    std::vector<MidiPacketBytes> packets(fragments.begin(), fragments.end());

    midi::MidiBuffer out;
    decode_midi_packets(packets, out);
    REQUIRE(out.sysex_size() == 1);
    REQUIRE(sysex_bytes(out, 0) == payload);
    REQUIRE(out.sysex()[0].sample_offset == 3);
}
