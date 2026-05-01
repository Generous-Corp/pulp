// Tests for the MIDI 1.0 running-status byte-stream parser
// (workstream 02 slice 2.5).

#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/running_status.hpp>

#include <vector>

using namespace pulp::midi;

namespace {

struct Captured {
    uint8_t status, d1, d2;
};

std::vector<Captured> parse(std::initializer_list<uint8_t> bytes) {
    RunningStatusParser p;
    std::vector<Captured> out;
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        out.push_back({m.data()[0],
                       m.length() > 1 ? m.data()[1] : uint8_t(0),
                       m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });
    std::vector<uint8_t> buf(bytes);
    p.feed(buf.data(), buf.size());
    return out;
}

} // namespace

TEST_CASE("single note-on is parsed", "[midi][running-status]") {
    auto v = parse({0x90, 0x3C, 0x7F});
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].status == 0x90);
    REQUIRE(v[0].d1 == 0x3C);
    REQUIRE(v[0].d2 == 0x7F);
}

TEST_CASE("running status repeats the previous status",
          "[midi][running-status]") {
    // 90 3C 7F   3D 7F   3E 7F  → three note-ons on channel 0
    auto v = parse({0x90, 0x3C, 0x7F, 0x3D, 0x7F, 0x3E, 0x7F});
    REQUIRE(v.size() == 3);
    REQUIRE(v[0].d1 == 0x3C);
    REQUIRE(v[1].status == 0x90);
    REQUIRE(v[1].d1 == 0x3D);
    REQUIRE(v[2].d1 == 0x3E);
}

TEST_CASE("program change has a single data byte",
          "[midi][running-status]") {
    auto v = parse({0xC0, 0x05, 0x07});  // PC=5 then PC=7 under running status
    REQUIRE(v.size() == 2);
    REQUIRE(v[0].status == 0xC0);
    REQUIRE(v[0].d1 == 0x05);
    REQUIRE(v[1].status == 0xC0);
    REQUIRE(v[1].d1 == 0x07);
}

TEST_CASE("running status handles one and two byte channel messages",
          "[midi][running-status][issue-645]") {
    auto v = parse({
        0xA2, 0x40, 0x20,  // poly pressure, two data bytes
        0x41, 0x21,        // running poly pressure
        0xD3, 0x7F,        // channel pressure, one data byte
        0x70,              // running channel pressure
    });

    REQUIRE(v.size() == 4);
    REQUIRE(v[0].status == 0xA2);
    REQUIRE(v[0].d1 == 0x40);
    REQUIRE(v[0].d2 == 0x20);
    REQUIRE(v[1].status == 0xA2);
    REQUIRE(v[1].d1 == 0x41);
    REQUIRE(v[1].d2 == 0x21);
    REQUIRE(v[2].status == 0xD3);
    REQUIRE(v[2].d1 == 0x7F);
    REQUIRE(v[3].status == 0xD3);
    REQUIRE(v[3].d1 == 0x70);
}

TEST_CASE("system common messages use their status-specific data lengths",
          "[midi][running-status][issue-645]") {
    auto v = parse({
        0xF1, 0x05,        // MTC quarter-frame
        0xF2, 0x10, 0x20,  // song position pointer
        0xF3, 0x07,        // song select
    });

    REQUIRE(v.size() == 3);
    REQUIRE(v[0].status == 0xF1);
    REQUIRE(v[0].d1 == 0x05);
    REQUIRE(v[1].status == 0xF2);
    REQUIRE(v[1].d1 == 0x10);
    REQUIRE(v[1].d2 == 0x20);
    REQUIRE(v[2].status == 0xF3);
    REQUIRE(v[2].d1 == 0x07);
}

TEST_CASE("real-time clock interleaves without breaking running status",
          "[midi][running-status]") {
    // Note-on, clock, next note-on sharing running status.
    auto v = parse({0x90, 0x3C, 0x7F, 0xF8, 0x3D, 0x7F});
    REQUIRE(v.size() == 3);
    REQUIRE(v[0].status == 0x90);
    REQUIRE(v[1].status == 0xF8);  // clock, single-byte
    REQUIRE(v[2].status == 0x90);
    REQUIRE(v[2].d1 == 0x3D);
}

TEST_CASE("sysex is delivered separately; cancels running status",
          "[midi][running-status]") {
    RunningStatusParser p;
    std::vector<uint8_t> sx;
    std::vector<uint8_t> shorts;
    p.on_sysex([&](const uint8_t* d, std::size_t s) {
        sx.assign(d, d + s);
    });
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });
    std::vector<uint8_t> stream = {
        0x90, 0x3C, 0x7F,          // note-on
        0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7,   // sysex (universal identity request)
        0x3D, 0x7F,                 // data bytes w/ no status — dropped because
                                    // sysex canceled running status
    };
    p.feed(stream.data(), stream.size());
    REQUIRE(sx.size() == 4);     // 7E 7F 06 01 between F0 and F7
    REQUIRE(sx[0] == 0x7E);
    REQUIRE(shorts.size() == 1);   // only the first note-on
    REQUIRE(shorts[0] == 0x90);
}

TEST_CASE("real-time bytes inside sysex are emitted without ending sysex",
          "[midi][running-status][issue-645]") {
    RunningStatusParser p;
    std::vector<uint8_t> sx;
    std::vector<uint8_t> shorts;
    p.on_sysex([&](const uint8_t* d, std::size_t s) {
        sx.assign(d, d + s);
    });
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });

    std::vector<uint8_t> stream = {0xF0, 0x01, 0xF8, 0x02, 0xF7};
    p.feed(stream.data(), stream.size());

    REQUIRE(shorts == std::vector<uint8_t>{0xF8});
    REQUIRE(sx == std::vector<uint8_t>{0x01, 0x02});
}

TEST_CASE("unexpected status inside sysex restarts short-message parsing",
          "[midi][running-status][issue-645]") {
    RunningStatusParser p;
    std::vector<uint8_t> sx;
    std::vector<Captured> shorts;
    p.on_sysex([&](const uint8_t* d, std::size_t s) {
        sx.assign(d, d + s);
    });
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        shorts.push_back({m.data()[0],
                          m.length() > 1 ? m.data()[1] : uint8_t(0),
                          m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    std::vector<uint8_t> stream = {0xF0, 0x01, 0x90, 0x3C, 0x7F};
    p.feed(stream.data(), stream.size());

    REQUIRE(sx.empty());
    REQUIRE(shorts.size() == 1);
    REQUIRE(shorts[0].status == 0x90);
    REQUIRE(shorts[0].d1 == 0x3C);
    REQUIRE(shorts[0].d2 == 0x7F);
}

TEST_CASE("empty sysex and unknown statuses are ignored",
          "[midi][running-status][issue-645]") {
    RunningStatusParser p;
    int sysex_count = 0;
    std::vector<uint8_t> shorts;
    p.on_sysex([&](const uint8_t*, std::size_t) {
        ++sysex_count;
    });
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });

    std::vector<uint8_t> stream = {
        0xF0, 0xF7,  // empty sysex should not call the sysex sink
        0xF4, 0x01,  // undefined system-common status and stray data
        0xF5, 0x02,  // another undefined status and stray data
        0xF6,        // tune request still emits
    };
    p.feed(stream.data(), stream.size());

    REQUIRE(sysex_count == 0);
    REQUIRE(shorts == std::vector<uint8_t>{0xF6});
}

TEST_CASE("undefined system statuses clear stale running state",
          "[midi][running-status][issue-645]") {
    auto v = parse({
        0x90, 0x3C, 0x7F,  // establish note-on running status
        0xF4,              // undefined system-common status cancels it
        0x3D, 0x7F,        // would emit a stale note if not cleared
        0x90, 0x40,        // partial note-on data
        0xF5,              // undefined status also clears partial data
        0x41,              // must not complete the interrupted note
    });

    REQUIRE(v.size() == 1);
    REQUIRE(v[0].status == 0x90);
    REQUIRE(v[0].d1 == 0x3C);
    REQUIRE(v[0].d2 == 0x7F);
}

TEST_CASE("undefined system statuses cancel pending system common data",
          "[midi][running-status][issue-645]") {
    auto v = parse({
        0xF2, 0x10,  // pending song-position pointer with one missing byte
        0xF4,        // undefined system-common status cancels the pending F2
        0x20,        // must not complete the interrupted F2
        0xF1,        // pending MTC quarter-frame
        0xF5,        // undefined system-common status cancels the pending F1
        0x05,        // must not complete the interrupted F1
        0x90, 0x3C, 0x7F,
        0xF4,        // also cancels established channel running status
        0x3D, 0x7F,
    });

    REQUIRE(v.size() == 1);
    REQUIRE(v[0].status == 0x90);
    REQUIRE(v[0].d1 == 0x3C);
    REQUIRE(v[0].d2 == 0x7F);
}

TEST_CASE("system common interruption inside sysex is reprocessed",
          "[midi][running-status][issue-645]") {
    RunningStatusParser p;
    std::vector<uint8_t> sx;
    std::vector<Captured> shorts;
    p.on_sysex([&](const uint8_t* d, std::size_t s) {
        sx.assign(d, d + s);
    });
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        shorts.push_back({m.data()[0],
                          m.length() > 1 ? m.data()[1] : uint8_t(0),
                          m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    std::vector<uint8_t> stream = {0xF0, 0x01, 0xF2, 0x10, 0x20};
    p.feed(stream.data(), stream.size());

    REQUIRE(sx.empty());
    REQUIRE(shorts.size() == 1);
    REQUIRE(shorts[0].status == 0xF2);
    REQUIRE(shorts[0].d1 == 0x10);
    REQUIRE(shorts[0].d2 == 0x20);
}

TEST_CASE("feed tolerates missing sinks and empty buffers",
          "[midi][running-status][issue-645]") {
    RunningStatusParser p;
    p.feed(nullptr, 0);

    std::vector<uint8_t> stream = {0xF6, 0x90, 0x3C, 0x7F, 0xF0, 0x01, 0xF7};
    p.feed(stream.data(), stream.size());

    SUCCEED("missing callbacks are ignored");
}

TEST_CASE("reset clears running status", "[midi][running-status]") {
    RunningStatusParser p;
    std::vector<uint8_t> shorts;
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });
    std::vector<uint8_t> a = {0x90, 0x3C, 0x7F};
    p.feed(a.data(), a.size());
    p.reset();
    // Without a status byte, these data bytes should be dropped.
    std::vector<uint8_t> b = {0x3D, 0x7F};
    p.feed(b.data(), b.size());
    REQUIRE(shorts.size() == 1);
}

TEST_CASE("one-byte system common doesn't leak into running state",
          "[midi][running-status]") {
    // #202 P1 regression: F6 Tune Request is one byte; an immediately
    // following stray data byte must be dropped, not emitted as a
    // phantom second F6.
    auto v = parse({0xF6, 0x42});
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].status == 0xF6);
}

TEST_CASE("reset() clears pending system-common state",
          "[midi][running-status]") {
    // #202 P2 regression: if reset fires while F1/F2/F3 is partially
    // accumulated, the first data byte after reset must be dropped.
    RunningStatusParser p;
    std::vector<uint8_t> shorts;
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });
    std::vector<uint8_t> a = {0xF2, 0x10};   // song-position pointer lsb only
    p.feed(a.data(), a.size());
    p.reset();
    std::vector<uint8_t> b = {0x20};         // stray byte — must drop
    p.feed(b.data(), b.size());
    REQUIRE(shorts.empty());
}
