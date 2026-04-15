// Verifies MidiBuffer::attach_ump / ump() sidecar plumbing.
// Workstream 02 slice 2.6 — format adapters now have a place to deliver
// native UMP packets (type-4 channel voice, SysEx, utility, etc.) that
// don't fit in choc::midi::ShortMessage.

#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/midi/ump.hpp>

using namespace pulp::midi;

TEST_CASE("MidiBuffer defaults to no attached UMP sidecar",
          "[midi][buffer][ump]") {
    MidiBuffer buf;
    REQUIRE(buf.ump() == nullptr);
}

TEST_CASE("attach_ump installs + clears the sidecar pointer",
          "[midi][buffer][ump]") {
    MidiBuffer buf;
    UmpBuffer ump;
    buf.attach_ump(&ump);
    REQUIRE(buf.ump() == &ump);
    buf.attach_ump(nullptr);
    REQUIRE(buf.ump() == nullptr);
}

TEST_CASE("plugin-side consumer can iterate UMP packets via the sidecar",
          "[midi][buffer][ump]") {
    MidiBuffer buf;
    buf.add(MidiEvent::note_on(0, 60, 100));  // main short-message path

    UmpBuffer ump;
    ump.add(UmpPacket::note_on_2(0, 0, 72, 0xFFFF));
    ump.add(UmpPacket::note_off_2(0, 0, 72, 0));
    buf.attach_ump(&ump);

    REQUIRE(buf.size() == 1);            // short-message count unchanged
    REQUIRE(buf.ump() != nullptr);
    REQUIRE(buf.ump()->size() == 2);     // UMP sidecar visible
    REQUIRE((*buf.ump())[0].packet.message_type() == UmpMessageType::Midi2ChannelVoice);
}
