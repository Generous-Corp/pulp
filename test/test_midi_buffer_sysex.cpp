// Verifies MidiBuffer::add_sysex sidecar — the variable-length parallel
// stream for F0 .. F7 payloads that don't fit in choc::midi::ShortMessage.
// Full MIDI vocabulary (sysex).

#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/buffer.hpp>
#include <type_traits>
#include <array>

using namespace pulp::midi;

TEST_CASE("realtime SysEx pool recycles payloads so SysEx is never dropped across blocks",
          "[midi][buffer][sysex]") {
    // RT contract for the pooled SysEx-copy path the VST3 adapter (PR #4564)
    // relies on: in realtime-capacity mode add_sysex_copy() reuses a reserved
    // payload pool that is replenished by clear_sysex() each block. The critical
    // correctness property is that this never silently DROPS SysEx across blocks —
    // i.e. the pool does not deplete cycle over cycle. (Steady-state allocation
    // freedom holds too, but the exact alloc count is build/allocator-dependent
    // — first-reuse lazy growth, Debug vs Release — so it isn't asserted here;
    // the VST3 adapter's RtAllocationProbe test covers the on-thread alloc path.)
    MidiBuffer buf;
    buf.reserve(/*events*/ 8, /*sysex slots*/ 4, /*payload bytes*/ 64);
    buf.set_realtime_capacity_limit(true);
    const std::array<uint8_t, 4> sysex{{0xF0, 0x7D, 0x01, 0xF7}};

    // Many more fill→reset cycles than the pool has slots: if clear_sysex() did
    // not recycle, the pool would deplete and later appends would be dropped.
    for (int cycle = 0; cycle < 64; ++cycle) {
        REQUIRE(buf.add_sysex_copy(sysex.data(), sysex.size(), 0, 0.0));
        REQUIRE(buf.sysex_size() == 1);
        REQUIRE(buf.sysex()[0].data.size() == sysex.size());
        REQUIRE(buf.dropped_sysex_count() == 0);
        buf.clear();
        buf.clear_sysex();
    }
}

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

TEST_CASE("realtime capacity limit drops MIDI and sysex without growing",
          "[midi][buffer][sysex][realtime]") {
    MidiBuffer buf;
    buf.reserve(1, 1);
    buf.set_realtime_capacity_limit(true);

    REQUIRE(buf.add(MidiEvent::note_on(0, 60, 100)));
    REQUIRE_FALSE(buf.add(MidiEvent::note_on(0, 61, 100)));
    REQUIRE(buf.size() == 1);
    REQUIRE(buf.event_capacity() == 1);
    REQUIRE(buf.dropped_event_count() == 1);

    REQUIRE(buf.add_sysex({0xF0, 0x7D, 0x01, 0xF7}));
    REQUIRE_FALSE(buf.add_sysex({0xF0, 0x7D, 0x02, 0xF7}));
    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.sysex_capacity() == 1);
    REQUIRE(buf.dropped_sysex_count() == 1);

    buf.clear();
    REQUIRE(buf.dropped_event_count() == 0);
    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.dropped_sysex_count() == 1);

    buf.clear_sysex();
    REQUIRE(buf.dropped_sysex_count() == 0);
}

TEST_CASE("realtime capacity limit drops copy-based sysex without allocating",
          "[midi][buffer][sysex][realtime]") {
    MidiBuffer buf;
    buf.reserve(1, 1);
    buf.set_realtime_capacity_limit(true);

    const uint8_t payload[] = {0xF0, 0x7D, 0x01, 0xF7};
    REQUIRE_FALSE(buf.add_sysex_copy(payload, sizeof(payload), 12, 0.25));
    REQUIRE(buf.sysex_size() == 0);
    REQUIRE(buf.sysex_capacity() == 1);
    REQUIRE(buf.dropped_sysex_count() == 1);
}

static_assert(!std::is_move_constructible_v<MidiBuffer::SysexPayload>);
static_assert(!std::is_move_assignable_v<MidiBuffer::SysexPayload>);

TEST_CASE("realtime copy-based sysex can be copied into prepared storage",
          "[midi][buffer][sysex][realtime]") {
    MidiBuffer buf;
    buf.reserve(1, 1, 8);
    buf.set_realtime_capacity_limit(true);
    MidiBuffer captured;
    captured.reserve(1, 1, 8);
    captured.set_realtime_capacity_limit(true);

    const uint8_t first[] = {0xF0, 0x7D, 0x01, 0xF7};
    REQUIRE(buf.add_sysex_copy(first, sizeof(first), 12, 0.25));
    REQUIRE(buf.sysex_size() == 1);

    REQUIRE(captured.add_sysex_copy(
        buf.sysex()[0].data.data(),
        buf.sysex()[0].data.size(),
        buf.sysex()[0].sample_offset,
        buf.sysex()[0].timestamp));
    REQUIRE(captured.sysex()[0].data
            == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});
    REQUIRE(buf.sysex()[0].data
            == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});

    buf.clear_sysex();
    REQUIRE(buf.dropped_sysex_count() == 0);
    REQUIRE(captured.sysex()[0].data
            == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});

    const uint8_t second[] = {0xF0, 0x7D, 0x02, 0xF7};
    REQUIRE(buf.add_sysex_copy(second, sizeof(second), 24, 0.5));
    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.sysex()[0].data
            == std::vector<uint8_t>{0xF0, 0x7D, 0x02, 0xF7});
    REQUIRE(buf.dropped_sysex_count() == 0);
}

TEST_CASE("failed rvalue sysex append leaves caller payload intact",
          "[midi][buffer][sysex][realtime]") {
    MidiBuffer buf;
    buf.reserve(1, 0);
    buf.set_realtime_capacity_limit(true);

    MidiBuffer::SysexEvent event;
    event.data = {0xF0, 0x7D, 0x03, 0xF7};
    event.sample_offset = 32;

    REQUIRE_FALSE(buf.add_sysex(std::move(event)));
    REQUIRE(event.data == std::vector<uint8_t>{0xF0, 0x7D, 0x03, 0xF7});
    REQUIRE(event.sample_offset == 32);
    REQUIRE(buf.dropped_sysex_count() == 1);
}

TEST_CASE("copied realtime sysex payload pool preserves reserved capacity",
          "[midi][buffer][sysex][realtime]") {
    MidiBuffer prepared;
    prepared.reserve(1, 1, 8);
    prepared.set_realtime_capacity_limit(true);

    MidiBuffer copy = prepared;
    const uint8_t payload[] = {0xF0, 0x7D, 0x04, 0xF7};
    REQUIRE(copy.add_sysex_copy(payload, sizeof(payload), 48, 0.75));
    REQUIRE(copy.sysex_size() == 1);
    REQUIRE(copy.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x04, 0xF7});
    REQUIRE(copy.sysex()[0].data.capacity() >= 8);
}

TEST_CASE("direct sysex vector clear recycles realtime payloads",
          "[midi][buffer][sysex][realtime]") {
    MidiBuffer buf;
    buf.reserve(1, 1, 8);
    buf.set_realtime_capacity_limit(true);

    const uint8_t first[] = {0xF0, 0x7D, 0x05, 0xF7};
    REQUIRE(buf.add_sysex_copy(first, sizeof(first), 12, 0.25));
    REQUIRE(buf.sysex_size() == 1);

    buf.sysex().clear();
    REQUIRE(buf.sysex_size() == 0);

    const uint8_t second[] = {0xF0, 0x7D, 0x06, 0xF7};
    REQUIRE(buf.add_sysex_copy(second, sizeof(second), 24, 0.5));
    REQUIRE(buf.sysex_size() == 1);
    REQUIRE(buf.sysex()[0].data
            == std::vector<uint8_t>{0xF0, 0x7D, 0x06, 0xF7});
    REQUIRE(buf.dropped_sysex_count() == 0);
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

TEST_CASE("MidiBuffer moves sysex sidecar with short messages",
          "[midi][buffer][sysex][coverage][phase3]") {
    MidiBuffer original;
    auto note = MidiEvent::note_on(1, 72, 110);
    note.sample_offset = 24;
    original.add(note);
    original.add_sysex({0xF0, 0x7D, 0x55, 0xF7}, 96, 1.25);

    MidiBuffer moved(std::move(original));
    REQUIRE(moved.size() == 1);
    REQUIRE(moved[0].sample_offset == 24);
    REQUIRE(moved.sysex_size() == 1);
    REQUIRE(moved.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x55, 0xF7});
    REQUIRE(moved.sysex()[0].sample_offset == 96);
    REQUIRE(moved.sysex()[0].timestamp == 1.25);

    MidiBuffer assigned;
    assigned.add_sysex({0xF0, 0x00, 0xF7}, 4, 0.125);
    assigned = std::move(moved);

    REQUIRE(assigned.size() == 1);
    REQUIRE(assigned[0].sample_offset == 24);
    REQUIRE(assigned.sysex_size() == 1);
    REQUIRE(assigned.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x55, 0xF7});
    REQUIRE(assigned.sysex()[0].sample_offset == 96);
    REQUIRE(assigned.sysex()[0].timestamp == 1.25);
}
