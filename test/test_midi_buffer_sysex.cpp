// Verifies MidiBuffer::add_sysex sidecar — the variable-length parallel
// stream for F0 .. F7 payloads that don't fit in choc::midi::ShortMessage.
// Workstream 01 — full MIDI vocabulary (sysex).

#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp::midi;

TEST_CASE("MidiBuffer starts with no sysex events", "[midi][buffer][sysex]") {
    MidiBuffer buf;
    REQUIRE(buf.sysex_size() == 0);
    REQUIRE(buf.sysex().empty());
}

TEST_CASE("add_sysex appends variable-length payloads", "[midi][buffer][sysex]") {
    MidiBuffer buf;
    // Universal Non-Real Time / Identity Request: F0 7E <device> 06 01 F7
    buf.add_sysex({0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}, 0, 0.0);
    // Arbitrary manufacturer-specific dump at sample 128
    buf.add_sysex({0xF0, 0x41, 0x00, 0x42, 0x12, 0x40, 0x00, 0x7F, 0xF7}, 128, 0.001);

    REQUIRE(buf.sysex_size() == 2);
    REQUIRE(buf.sysex()[0].data.size() == 6);
    REQUIRE(buf.sysex()[0].data.front() == 0xF0);
    REQUIRE(buf.sysex()[0].data.back() == 0xF7);
    REQUIRE(buf.sysex()[0].sample_offset == 0);
    REQUIRE(buf.sysex()[1].sample_offset == 128);
    REQUIRE(buf.sysex()[1].data.size() == 9);
}

TEST_CASE("clear_sysex removes sidecar events but leaves short messages alone",
          "[midi][buffer][sysex]") {
    MidiBuffer buf;
    buf.add(MidiEvent::note_on(0, 60, 100));
    buf.add_sysex({0xF0, 0x7D, 0x01, 0xF7});
    REQUIRE(buf.size() == 1);
    REQUIRE(buf.sysex_size() == 1);

    buf.clear_sysex();
    REQUIRE(buf.sysex_size() == 0);
    REQUIRE(buf.size() == 1);   // short messages untouched
}

TEST_CASE("MidiBuffer::clear does not affect sysex sidecar",
          "[midi][buffer][sysex]") {
    // clear() historically only nuked short messages; sysex lives in a
    // parallel stream so tests pin that invariant.
    MidiBuffer buf;
    buf.add(MidiEvent::note_on(0, 60, 100));
    buf.add_sysex({0xF0, 0x7D, 0x01, 0xF7});
    buf.clear();
    REQUIRE(buf.empty());
    REQUIRE(buf.sysex_size() == 1);
}
