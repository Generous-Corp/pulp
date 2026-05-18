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

TEST_CASE("add_sysex preserves payload bytes and timing metadata",
          "[midi][buffer][sysex][issue-493][issue-641][issue-645]") {
    MidiBuffer buf;
    std::vector<uint8_t> payload{0xF0, 0x7D, 0x00, 0x7F, 0xF7};

    buf.add_sysex(std::move(payload), 240, 1.25);
    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x00, 0x7F, 0xF7});
    REQUIRE(buf.sysex()[0].sample_offset == 240);
    REQUIRE(buf.sysex()[0].timestamp == 1.25);

    buf.sysex()[0].sample_offset = 256;
    buf.sysex()[0].timestamp = 1.5;
    buf.sysex()[0].data.push_back(0x00);
    REQUIRE(buf.sysex()[0].sample_offset == 256);
    REQUIRE(buf.sysex()[0].timestamp == 1.5);
    REQUIRE(buf.sysex()[0].data.back() == 0x00);
}

TEST_CASE("MidiBuffer::sort only orders short MIDI messages",
          "[midi][buffer][sysex][issue-493][issue-641][issue-645]") {
    MidiBuffer buf;
    auto late = MidiEvent::note_on(0, 60, 100);
    late.sample_offset = 96;
    auto early = MidiEvent::note_off(0, 60, 0);
    early.sample_offset = 12;

    buf.add(late);
    buf.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 128, 2.0);
    buf.add(early);
    buf.add_sysex({0xF0, 0x7D, 0x02, 0xF7}, 32, 0.5);

    buf.sort();

    REQUIRE(buf.size() == 2);
    REQUIRE(buf[0].sample_offset == 12);
    REQUIRE(buf[1].sample_offset == 96);
    REQUIRE(buf.sysex_size() == 2);
    REQUIRE(buf.sysex()[0].sample_offset == 128);
    REQUIRE(buf.sysex()[0].timestamp == 2.0);
    REQUIRE(buf.sysex()[0].data[2] == 0x01);
    REQUIRE(buf.sysex()[1].sample_offset == 32);
    REQUIRE(buf.sysex()[1].timestamp == 0.5);
    REQUIRE(buf.sysex()[1].data[2] == 0x02);
}

TEST_CASE("clear_sysex is idempotent and sidecar accepts later events",
          "[midi][buffer][sysex][issue-493][issue-641][issue-645]") {
    MidiBuffer buf;
    buf.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 4, 0.25);

    buf.clear_sysex();
    buf.clear_sysex();
    REQUIRE(buf.sysex().empty());

    buf.add_sysex({0xF0, 0xF7}, 8, 0.5);
    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.sysex()[0].data == std::vector<uint8_t>{0xF0, 0xF7});
    REQUIRE(buf.sysex()[0].sample_offset == 8);
    REQUIRE(buf.sysex()[0].timestamp == 0.5);
}

TEST_CASE("MidiBuffer copies sysex sidecar independently",
          "[midi][buffer][sysex][issue-493][issue-641][issue-645]") {
    MidiBuffer original;
    original.add(MidiEvent::note_on(0, 64, 90));
    original.add_sysex({0xF0, 0x7D, 0x03, 0xF7}, 48, 0.75);

    MidiBuffer copy = original;
    copy.sysex()[0].data[2] = 0x04;
    copy.sysex()[0].sample_offset = 64;
    copy.add(MidiEvent::note_off(0, 64, 0));

    REQUIRE(original.size() == 1);
    REQUIRE(original.sysex_size() == 1);
    REQUIRE(original.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x03, 0xF7});
    REQUIRE(original.sysex()[0].sample_offset == 48);

    REQUIRE(copy.size() == 2);
    REQUIRE(copy.sysex_size() == 1);
    REQUIRE(copy.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x04, 0xF7});
    REQUIRE(copy.sysex()[0].sample_offset == 64);
}

TEST_CASE("MidiBuffer move construction preserves sidecar contents",
          "[midi][buffer][sysex][coverage][phase3]") {
    MidiBuffer original;
    original.add(MidiEvent::note_on(0, 72, 100));
    original.add_sysex({0xF0, 0x7D, 0x05, 0xF7}, 96, 1.25);

    MidiBuffer moved(std::move(original));

    REQUIRE(moved.size() == 1);
    REQUIRE(moved[0].note() == 72);
    REQUIRE(moved.sysex_size() == 1);
    REQUIRE(moved.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x05, 0xF7});
    REQUIRE(moved.sysex()[0].sample_offset == 96);
    REQUIRE(moved.sysex()[0].timestamp == 1.25);
}
