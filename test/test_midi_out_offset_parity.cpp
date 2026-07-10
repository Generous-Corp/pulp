// Cross-format outbound-MIDI sample-offset parity.
//
// PAM (and any Pulp MIDI-effect / instrument) depends on a single invariant:
//     an event a Processor emits at in-block sample offset N is delivered to the
//     host at offset N, for every valid 0 <= N < frame_count,
// identically across AU v2, VST3, and CLAP. The three adapters map the offset
// onto a different host type (AU MIDIPacket.timeStamp, VST3 Event.sampleOffset,
// CLAP clap_event_header.time), so the mapping lives in one shared, tested place
// (detail/midi_out_offset.hpp). All three adapters route through it; this test
// pins the contract:
//   * the REAL AU v2 MidiOutputPacketBuilder preserves each event's offset;
//   * the shared VST3/CLAP offset helpers (which the adapters call) preserve
//     valid offsets and clamp the out-of-range ones per each host's type rules.
//
// Apple-only: MidiOutputPacketBuilder pulls in AudioUnitSDK via au_v2_adapter.hpp.

#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/detail/midi_out_offset.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using pulp::format::detail::au_output_offset;
using pulp::format::detail::clap_output_offset;
using pulp::format::detail::vst3_output_offset;

namespace {
constexpr std::uint32_t kFrames = 256;
constexpr std::array<std::int32_t, 4> kOffsets{0, 17, 64, 255};
}  // namespace

TEST_CASE("MIDI out parity: AU v2 builder delivers each event at its emit offset",
          "[midi-out][parity][au]") {
    pulp::midi::MidiBuffer midi_out;
    for (auto off : kOffsets) {
        auto n = pulp::midi::MidiEvent::note_on(0, static_cast<uint8_t>(60), 100);
        n.sample_offset = off;
        midi_out.add(n);
    }

    pulp::format::au::MidiOutputPacketBuilder builder;
    const MIDIPacketList* list = builder.build(midi_out, kFrames);
    REQUIRE(list != nullptr);
    REQUIRE(list->numPackets == kOffsets.size());
    REQUIRE(builder.dropped == 0);

    const MIDIPacket* pkt = &list->packet[0];
    for (std::size_t i = 0; i < kOffsets.size(); ++i) {
        INFO("event " << i);
        REQUIRE(pkt->timeStamp == static_cast<MIDITimeStamp>(kOffsets[i]));
        pkt = MIDIPacketNext(pkt);
    }
}

TEST_CASE("MIDI out parity: all three formats preserve valid in-block offsets",
          "[midi-out][parity]") {
    for (auto off : kOffsets) {
        INFO("offset " << off);
        REQUIRE(vst3_output_offset(off) == off);
        REQUIRE(clap_output_offset(off) == static_cast<std::uint32_t>(off));
        REQUIRE(au_output_offset(off, kFrames) == off);
    }
}

TEST_CASE("MIDI out parity: negative offset clamps to the block start",
          "[midi-out][parity]") {
    // A well-behaved Processor never emits a negative offset, but each host type
    // has a defined floor: CLAP/AU clamp up to 0; VST3's signed field passes it
    // through for the host to clamp.
    REQUIRE(clap_output_offset(-5) == 0u);
    REQUIRE(au_output_offset(-5, kFrames) == 0);
    REQUIRE(vst3_output_offset(-5) == -5);
}

TEST_CASE("MIDI out parity: AU clamps an out-of-block offset into the block",
          "[midi-out][parity][au]") {
    // AU MIDIPacket timestamps are in-block sample offsets; an offset past the
    // block is clamped to the last frame so the packet never lands past the
    // block. CLAP/VST3 forward the (still valid unsigned/signed) value.
    REQUIRE(au_output_offset(1000, kFrames) ==
            static_cast<std::int32_t>(kFrames) - 1);
    // A degenerate zero-length block has no valid frame to clamp into, so the
    // offset is left unchanged (matches the AU builder's original clamp: the
    // upper-bound clamp is guarded on frame_count > 0). A 0-frame render never
    // emits events in practice.
    REQUIRE(au_output_offset(64, 0) == 64);
    REQUIRE(clap_output_offset(1000) == 1000u);
    REQUIRE(vst3_output_offset(1000) == 1000);
}
