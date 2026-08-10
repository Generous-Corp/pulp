// Verifies MidiBuffer::attach_ump / ump() sidecar plumbing.
// Format adapters now have a place to deliver
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

TEST_CASE("UmpBuffer sorts, iterates, and clears sample-accurate events",
          "[midi][buffer][ump]") {
    UmpBuffer ump;
    ump.add(UmpPacket::note_on_2(0, 2, 67, 0x4000), 96);
    ump.add(UmpEvent{UmpPacket::note_off_2(0, 1, 60, 0), 12});
    ump.add(UmpPacket::note_on_2(0, 1, 60, 0x8000), 48);

    REQUIRE_FALSE(ump.empty());
    REQUIRE(ump.size() == 3);

    ump.sort();
    REQUIRE(ump[0].sample_offset == 12);
    REQUIRE(ump[1].sample_offset == 48);
    REQUIRE(ump[2].sample_offset == 96);

    int total_offsets = 0;
    for (const auto& event : ump)
        total_offsets += event.sample_offset;
    REQUIRE(total_offsets == 156);

    ump.clear();
    REQUIRE(ump.empty());
    REQUIRE(ump.size() == 0);
}

TEST_CASE("attached UMP sidecar remains externally owned across MidiBuffer edits",
          "[midi][buffer][ump][issue-645]") {
    MidiBuffer buf;
    UmpBuffer first;
    UmpBuffer second;
    first.add(UmpPacket::note_on_2(0, 1, 60, 0x8000), 32);
    second.add(UmpPacket::note_on_2(0, 2, 67, 0x4000), 64);

    buf.attach_ump(&first);
    buf.add(MidiEvent::note_on(0, 60, 100));
    REQUIRE(buf.ump() == &first);
    REQUIRE(buf.ump()->size() == 1);

    buf.clear();
    REQUIRE(buf.empty());
    REQUIRE(buf.ump() == &first);
    REQUIRE(buf.ump()->size() == 1);

    buf.attach_ump(&second);
    REQUIRE(buf.ump() == &second);
    REQUIRE((*buf.ump())[0].packet.channel() == 2);
}

TEST_CASE("MidiBuffer copies isolate MIDI1 storage while sharing the external UMP sidecar",
          "[midi][buffer][ump][copy][ownership]") {
    UmpBuffer shared;
    const auto ump_on = UmpPacket::note_on_2(0, 2, 60, 0x9000);
    const auto ump_off = UmpPacket::note_off_2(0, 2, 60, 0x1000);
    REQUIRE(shared.add(ump_on, 24));

    MidiBuffer source;
    REQUIRE(source.add(MidiEvent::note_on(2, 60, 100)));
    source.attach_ump(&shared);

    MidiBuffer copy(source);
    REQUIRE(copy.ump() == &shared);
    REQUIRE(source.ump() == &shared);
    REQUIRE(copy.size() == 1);
    REQUIRE(copy[0].is_note_on());
    REQUIRE(copy[0].channel() == 2);
    REQUIRE(copy[0].note() == 60);
    REQUIRE(copy.ump()->size() == 1);
    REQUIRE((*copy.ump())[0].packet.words == ump_on.words);

    copy.clear();
    REQUIRE(copy.add(MidiEvent::cc(3, 74, 91)));
    REQUIRE(copy.size() == 1);
    REQUIRE(copy[0].is_cc());
    REQUIRE(source.size() == 1);
    REQUIRE(source[0].is_note_on());
    REQUIRE(source[0].channel() == 2);
    REQUIRE(source[0].note() == 60);

    REQUIRE(shared.add(ump_off, 48));
    REQUIRE(source.ump()->size() == 2);
    REQUIRE(copy.ump()->size() == 2);
    REQUIRE((*source.ump())[1].packet.words == ump_off.words);
    REQUIRE((*copy.ump())[1].packet.words == ump_off.words);
    int ump_note_depth = 0;
    for (const auto& event : *copy.ump()) {
        const auto status = static_cast<std::uint8_t>(event.packet.status() & 0xF0);
        if (status == static_cast<std::uint8_t>(Midi2Status::NoteOn))
            ++ump_note_depth;
        else if (status == static_cast<std::uint8_t>(Midi2Status::NoteOff))
            --ump_note_depth;
        REQUIRE(ump_note_depth >= 0);
    }
    REQUIRE(ump_note_depth == 0);

    UmpBuffer displaced;
    const auto sentinel = UmpPacket::cc_2(0, 7, 1, 0x12345678u);
    REQUIRE(displaced.add(sentinel, 7));
    MidiBuffer assigned;
    REQUIRE(assigned.add(MidiEvent::note_off(7, 72)));
    assigned.attach_ump(&displaced);

    assigned = source;
    REQUIRE(assigned.ump() == &shared);
    REQUIRE(assigned.size() == 1);
    REQUIRE(assigned[0].is_note_on());
    REQUIRE(assigned[0].channel() == 2);
    REQUIRE(assigned[0].note() == 60);
    REQUIRE(assigned.ump()->size() == 2);
    REQUIRE(displaced.size() == 1);
    REQUIRE(displaced[0].sample_offset == 7);
    REQUIRE(displaced[0].packet.words == sentinel.words);
}

TEST_CASE("MidiBuffer move assignment preserves attached UMP sidecar pointer",
          "[midi][buffer][ump]") {
    UmpBuffer sidecar;
    sidecar.add(UmpPacket::cc_2(0, 3, 74, 0x80000000u), 24);

    MidiBuffer source;
    source.add(MidiEvent::cc(3, 74, 64));
    source.attach_ump(&sidecar);

    MidiBuffer target;
    target = std::move(source);

    REQUIRE(target.size() == 1);
    REQUIRE(target.ump() == &sidecar);
    REQUIRE(target.ump()->size() == 1);
    REQUIRE((*target.ump())[0].sample_offset == 24);
    REQUIRE((*target.ump())[0].packet.channel() == 3);
}
