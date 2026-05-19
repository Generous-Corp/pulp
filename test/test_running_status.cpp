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

TEST_CASE("running status completes messages split across feed calls",
          "[midi][running-status][coverage][phase3]") {
    RunningStatusParser p;
    std::vector<Captured> out;
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        out.push_back({m.data()[0],
                       m.length() > 1 ? m.data()[1] : uint8_t(0),
                       m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    std::vector<uint8_t> first = {0x90, 0x3C};
    std::vector<uint8_t> second = {0x7F, 0x3D};
    std::vector<uint8_t> third = {0x40};

    p.feed(first.data(), first.size());
    REQUIRE(out.empty());

    p.feed(second.data(), second.size());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].status == 0x90);
    REQUIRE(out[0].d1 == 0x3C);
    REQUIRE(out[0].d2 == 0x7F);

    p.feed(third.data(), third.size());
    REQUIRE(out.size() == 2);
    REQUIRE(out[1].status == 0x90);
    REQUIRE(out[1].d1 == 0x3D);
    REQUIRE(out[1].d2 == 0x40);
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

TEST_CASE("running status repeats control-change and pitch-bend messages",
          "[midi][running-status][coverage][phase3]") {
    auto v = parse({
        0xB4, 0x40, 0x7F,  // sustain pedal on channel 4
        0x41, 0x00,        // running CC
        0xE5, 0x00, 0x40,  // pitch bend center on channel 5
        0x7F, 0x7F,        // running pitch bend maximum
    });

    REQUIRE(v.size() == 4);
    REQUIRE(v[0].status == 0xB4);
    REQUIRE(v[0].d1 == 0x40);
    REQUIRE(v[0].d2 == 0x7F);
    REQUIRE(v[1].status == 0xB4);
    REQUIRE(v[1].d1 == 0x41);
    REQUIRE(v[1].d2 == 0x00);
    REQUIRE(v[2].status == 0xE5);
    REQUIRE(v[2].d1 == 0x00);
    REQUIRE(v[2].d2 == 0x40);
    REQUIRE(v[3].status == 0xE5);
    REQUIRE(v[3].d1 == 0x7F);
    REQUIRE(v[3].d2 == 0x7F);
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

TEST_CASE("running status completes channel messages across feed boundaries",
          "[midi][running-status][coverage][phase3]") {
    RunningStatusParser p;
    std::vector<Captured> shorts;
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        shorts.push_back({m.data()[0],
                          m.length() > 1 ? m.data()[1] : uint8_t(0),
                          m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    const uint8_t status_and_first_data[] = {0x90, 0x3C};
    p.feed(status_and_first_data, sizeof(status_and_first_data));
    REQUIRE(shorts.empty());

    const uint8_t second_data_and_running[] = {0x7F, 0x3D, 0x40};
    p.feed(second_data_and_running, sizeof(second_data_and_running));

    REQUIRE(shorts.size() == 2);
    REQUIRE(shorts[0].status == 0x90);
    REQUIRE(shorts[0].d1 == 0x3C);
    REQUIRE(shorts[0].d2 == 0x7F);
    REQUIRE(shorts[1].status == 0x90);
    REQUIRE(shorts[1].d1 == 0x3D);
    REQUIRE(shorts[1].d2 == 0x40);
}

TEST_CASE("new channel status cancels partial running-status data",
          "[midi][running-status][coverage][phase3]") {
    auto v = parse({
        0x90, 0x3C, 0x7F,  // establish note-on running status
        0x3D,              // partial running-status note, missing velocity
        0x80, 0x3D, 0x00,  // new note-off must discard the partial note-on
    });

    REQUIRE(v.size() == 2);
    REQUIRE(v[0].status == 0x90);
    REQUIRE(v[0].d1 == 0x3C);
    REQUIRE(v[0].d2 == 0x7F);
    REQUIRE(v[1].status == 0x80);
    REQUIRE(v[1].d1 == 0x3D);
    REQUIRE(v[1].d2 == 0x00);
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

TEST_CASE("system common messages complete across feed boundaries without running",
          "[midi][running-status][coverage][phase3]") {
    RunningStatusParser p;
    std::vector<Captured> shorts;
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        shorts.push_back({m.data()[0],
                          m.length() > 1 ? m.data()[1] : uint8_t(0),
                          m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    const uint8_t start[] = {0xF2, 0x10};
    p.feed(start, sizeof(start));
    REQUIRE(shorts.empty());

    const uint8_t finish_and_stray[] = {0x20, 0x30};
    p.feed(finish_and_stray, sizeof(finish_and_stray));

    REQUIRE(shorts.size() == 1);
    REQUIRE(shorts[0].status == 0xF2);
    REQUIRE(shorts[0].d1 == 0x10);
    REQUIRE(shorts[0].d2 == 0x20);
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

TEST_CASE("all realtime status bytes emit immediately",
          "[midi][running-status][coverage][phase3]") {
    auto v = parse({0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF});

    REQUIRE(v.size() == 8);
    REQUIRE(v[0].status == 0xF8);
    REQUIRE(v[1].status == 0xF9);
    REQUIRE(v[2].status == 0xFA);
    REQUIRE(v[3].status == 0xFB);
    REQUIRE(v[4].status == 0xFC);
    REQUIRE(v[5].status == 0xFD);
    REQUIRE(v[6].status == 0xFE);
    REQUIRE(v[7].status == 0xFF);
}

TEST_CASE("reserved real-time bytes preserve running status",
          "[midi][running-status][codecov]") {
    auto v = parse({
        0x90, 0x3C, 0x7F,
        0xF9,
        0x3D, 0x40,
        0xFD,
        0x3E, 0x41,
    });

    REQUIRE(v.size() == 5);
    REQUIRE(v[0].status == 0x90);
    REQUIRE(v[0].d1 == 0x3C);
    REQUIRE(v[1].status == 0xF9);
    REQUIRE(v[2].status == 0x90);
    REQUIRE(v[2].d1 == 0x3D);
    REQUIRE(v[2].d2 == 0x40);
    REQUIRE(v[3].status == 0xFD);
    REQUIRE(v[4].status == 0x90);
    REQUIRE(v[4].d1 == 0x3E);
    REQUIRE(v[4].d2 == 0x41);
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

TEST_CASE("unterminated sysex is dropped on reset",
          "[midi][running-status][coverage][phase3]") {
    RunningStatusParser p;
    std::vector<uint8_t> sx;
    std::vector<uint8_t> shorts;
    p.on_sysex([&](const uint8_t* d, std::size_t s) {
        sx.assign(d, d + s);
    });
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });

    std::vector<uint8_t> partial = {0xF0, 0x7D, 0x01, 0x02};
    p.feed(partial.data(), partial.size());
    p.reset();

    std::vector<uint8_t> after = {0xF7, 0x90, 0x40, 0x7F};
    p.feed(after.data(), after.size());

    REQUIRE(sx.empty());
    REQUIRE(shorts == std::vector<uint8_t>{0x90});
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

TEST_CASE("empty feed and reset drop partial running-status messages",
          "[midi][running-status][coverage][phase3]") {
    RunningStatusParser parser;
    std::vector<Captured> shorts;
    parser.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        shorts.push_back({m.data()[0],
                          m.length() > 1 ? m.data()[1] : uint8_t(0),
                          m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    parser.feed(nullptr, 0);
    REQUIRE(shorts.empty());

    std::vector<uint8_t> partial = {0x90, 0x3C};
    parser.feed(partial.data(), partial.size());
    REQUIRE(shorts.empty());

    parser.reset();
    std::vector<uint8_t> trailing_data = {0x7F};
    parser.feed(trailing_data.data(), trailing_data.size());
    REQUIRE(shorts.empty());

    std::vector<uint8_t> complete = {0x90, 0x40, 0x64};
    parser.feed(complete.data(), complete.size());
    REQUIRE(shorts.size() == 1);
    REQUIRE(shorts[0].status == 0x90);
    REQUIRE(shorts[0].d1 == 0x40);
    REQUIRE(shorts[0].d2 == 0x64);
}

TEST_CASE("reset drops partial sysex before the next complete sysex",
          "[midi][running-status][coverage][phase3]") {
    RunningStatusParser parser;
    std::vector<std::vector<uint8_t>> sysex;
    parser.on_sysex([&](const uint8_t* data, std::size_t size) {
        sysex.emplace_back(data, data + size);
    });

    std::vector<uint8_t> partial = {0xF0, 0x7D, 0x01};
    parser.feed(partial.data(), partial.size());
    parser.reset();

    std::vector<uint8_t> stray_end = {0xF7};
    parser.feed(stray_end.data(), stray_end.size());
    REQUIRE(sysex.empty());

    std::vector<uint8_t> complete = {0xF0, 0x7E, 0x02, 0xF7};
    parser.feed(complete.data(), complete.size());
    REQUIRE(sysex == std::vector<std::vector<uint8_t>>{{0x7E, 0x02}});
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

TEST_CASE("feed preserves partial channel message across calls",
          "[midi][running-status][coverage]") {
    RunningStatusParser p;
    std::vector<Captured> shorts;
    p.on_short_message([&](const MidiEvent& e) {
        const auto& m = e.message;
        shorts.push_back({m.data()[0],
                          m.length() > 1 ? m.data()[1] : uint8_t(0),
                          m.length() > 2 ? m.data()[2] : uint8_t(0)});
    });

    std::vector<uint8_t> first = {0x90, 0x3c};
    std::vector<uint8_t> second = {0x7f, 0x3d};
    std::vector<uint8_t> third = {0x40};

    p.feed(first.data(), first.size());
    REQUIRE(shorts.empty());

    p.feed(second.data(), second.size());
    REQUIRE(shorts.size() == 1);
    REQUIRE(shorts[0].status == 0x90);
    REQUIRE(shorts[0].d1 == 0x3c);
    REQUIRE(shorts[0].d2 == 0x7f);

    p.feed(third.data(), third.size());
    REQUIRE(shorts.size() == 2);
    REQUIRE(shorts[1].status == 0x90);
    REQUIRE(shorts[1].d1 == 0x3d);
    REQUIRE(shorts[1].d2 == 0x40);
}

TEST_CASE("feed preserves sysex payload across calls",
          "[midi][running-status][coverage]") {
    RunningStatusParser p;
    std::vector<uint8_t> sysex;
    std::vector<uint8_t> shorts;
    p.on_sysex([&](const uint8_t* d, std::size_t s) {
        sysex.assign(d, d + s);
    });
    p.on_short_message([&](const MidiEvent& e) {
        shorts.push_back(e.message.data()[0]);
    });

    std::vector<uint8_t> first = {0xF0, 0x7D, 0x01};
    std::vector<uint8_t> second = {0xF8, 0x02};
    std::vector<uint8_t> third = {0x03, 0xF7};

    p.feed(first.data(), first.size());
    REQUIRE(sysex.empty());
    REQUIRE(shorts.empty());

    p.feed(second.data(), second.size());
    REQUIRE(sysex.empty());
    REQUIRE(shorts == std::vector<uint8_t>{0xF8});

    p.feed(third.data(), third.size());
    REQUIRE(sysex == std::vector<uint8_t>{0x7D, 0x01, 0x02, 0x03});
    REQUIRE(shorts == std::vector<uint8_t>{0xF8});
}
