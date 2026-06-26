// Tests for pulp::midi::parse_raw_midi_bytes — the shared raw-MIDI
// byte-stream parser used by ALSA (and future transports).
//
// The canonical happy-path cases (single-packet sysex, short messages,
// realtime passthrough) are covered by the higher-level ALSA path
// when real hardware is present; these tests focus on behaviors that
// can only be exercised with a synthetic byte stream:
//
//   - multi-read sysex spanning separate parse() calls
//   - realtime bytes interleaved INSIDE a sysex run (MIDI 1.0 §3.1)
//   - aborted sysex recovery (#406): a non-realtime status
//     byte arriving mid-F0 must not swallow the next real message
//   - runaway sysex cap (kRawMidiSysexLimit)

#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/raw_midi_parser.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

using namespace pulp::midi;

namespace {

struct Captured {
    struct Short { uint8_t status, d1, d2; };
    std::vector<Short> shorts;
    std::vector<std::vector<uint8_t>> sysex;
};

struct RtRawMidiSink {
    static constexpr std::size_t kMaxShorts = 8;
    static constexpr std::size_t kMaxSysexMessages = 2;
    static constexpr std::size_t kMaxSysexBytes = 8;

    std::array<Captured::Short, kMaxShorts> shorts{};
    std::size_t short_count = 0;

    struct Sysex {
        std::array<uint8_t, kMaxSysexBytes> payload{};
        std::size_t size = 0;
    };
    std::array<Sysex, kMaxSysexMessages> sysex{};
    std::size_t sysex_count = 0;

    void capture_short(uint8_t status, uint8_t d1, uint8_t d2) noexcept {
        REQUIRE(short_count < shorts.size());
        shorts[short_count++] = {status, d1, d2};
    }

    void capture_sysex(const std::vector<uint8_t>& payload) noexcept {
        REQUIRE(sysex_count < sysex.size());
        REQUIRE(payload.size() <= kMaxSysexBytes);
        auto& message = sysex[sysex_count++];
        message.size = payload.size();
        std::copy(payload.begin(), payload.end(), message.payload.begin());
    }
};

Captured parse(std::initializer_list<uint8_t> bytes) {
    Captured c;
    RawMidiParserState state;
    std::vector<uint8_t> buf(bytes);
    parse_raw_midi_bytes(
        buf.data(), buf.size(), state,
        [&c](uint8_t s, uint8_t d1, uint8_t d2) {
            c.shorts.push_back({s, d1, d2});
        },
        [&c](const std::vector<uint8_t>& sx) { c.sysex.push_back(sx); });
    return c;
}

} // namespace

TEST_CASE("raw_midi_parser reserved feed path is allocation-free",
          "[midi][raw_midi_parser][rt-safety]") {
    RawMidiParserState state;
    state.reserve_sysex(RtRawMidiSink::kMaxSysexBytes);
    REQUIRE(state.sysex_buffer.capacity() >= RtRawMidiSink::kMaxSysexBytes);

    RtRawMidiSink sink;
    RawMidiShortCallback on_short =
        [&sink](uint8_t status, uint8_t d1, uint8_t d2) noexcept {
            sink.capture_short(status, d1, d2);
        };
    RawMidiSysexCallback on_sysex =
        [&sink](const std::vector<uint8_t>& payload) noexcept {
            sink.capture_sysex(payload);
        };

    const uint8_t first[] = {
        0x90, 0x3C, 0x7F,
        0x3D, 0x40,
        0xF0, 0x7D, 0x01,
    };
    const uint8_t second[] = {
        0xF8,
        0x02, 0xF7,
        0xF0, 0x41, 0x10,
        0x90, 0x40, 0x7F,
    };

    {
        pulp::test::RtAllocationProbe probe;
        parse_raw_midi_bytes(first, sizeof(first), state, on_short, on_sysex);
        parse_raw_midi_bytes(second, sizeof(second), state, on_short, on_sysex);
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(sink.short_count == 4);
    REQUIRE(sink.shorts[0].status == 0x90);
    REQUIRE(sink.shorts[0].d1 == 0x3C);
    REQUIRE(sink.shorts[1].status == 0x90);
    REQUIRE(sink.shorts[1].d1 == 0x3D);
    REQUIRE(sink.shorts[2].status == 0xF8);
    REQUIRE(sink.shorts[3].status == 0x90);
    REQUIRE(sink.shorts[3].d1 == 0x40);
    REQUIRE(sink.sysex_count == 1);
    REQUIRE(sink.sysex[0].size == 5);
    REQUIRE(sink.sysex[0].payload[0] == 0xF0);
    REQUIRE(sink.sysex[0].payload[4] == 0xF7);
    REQUIRE_FALSE(state.sysex_in_progress);
    REQUIRE(state.sysex_buffer.empty());
}

TEST_CASE("raw_midi_parser emits short messages cleanly",
          "[midi][raw_midi_parser]") {
    // Note On + Program Change (2-byte) + Song Position (3-byte)
    auto c = parse({0x90, 0x40, 0x7F,
                    0xC0, 0x05,
                    0xF2, 0x10, 0x20});
    REQUIRE(c.shorts.size() == 3);
    REQUIRE(c.shorts[0].status == 0x90);
    REQUIRE(c.shorts[0].d1 == 0x40);
    REQUIRE(c.shorts[0].d2 == 0x7F);
    REQUIRE(c.shorts[1].status == 0xC0);
    REQUIRE(c.shorts[1].d1 == 0x05);
    REQUIRE(c.shorts[1].d2 == 0x00);
    REQUIRE(c.shorts[2].status == 0xF2);
    REQUIRE(c.shorts[2].d1 == 0x10);
    REQUIRE(c.shorts[2].d2 == 0x20);
    REQUIRE(c.sysex.empty());
}

TEST_CASE("raw_midi_parser emits remaining short-message forms",
          "[midi][raw_midi_parser][issue-645]") {
    auto c = parse({0xD0, 0x45, // Channel Pressure
                    0xF3, 0x09, // Song Select
                    0xF6,       // Tune Request
                    0xF7});      // Stray End-of-Exclusive

    REQUIRE(c.shorts.size() == 4);
    REQUIRE(c.shorts[0].status == 0xD0);
    REQUIRE(c.shorts[0].d1 == 0x45);
    REQUIRE(c.shorts[0].d2 == 0x00);
    REQUIRE(c.shorts[1].status == 0xF3);
    REQUIRE(c.shorts[1].d1 == 0x09);
    REQUIRE(c.shorts[1].d2 == 0x00);
    REQUIRE(c.shorts[2].status == 0xF6);
    REQUIRE(c.shorts[2].d1 == 0x00);
    REQUIRE(c.shorts[2].d2 == 0x00);
    REQUIRE(c.shorts[3].status == 0xF7);
    REQUIRE(c.sysex.empty());
}

TEST_CASE("raw_midi_parser drops stray data and incomplete short messages",
          "[midi][raw_midi_parser][issue-645]") {
    auto c = parse({0x00, 0x7F,       // stray data bytes
                    0x90, 0x40,       // incomplete Note On
                    0x80});           // incomplete Note Off

    REQUIRE(c.shorts.empty());
    REQUIRE(c.sysex.empty());
}

TEST_CASE("raw_midi_parser completes short messages split across calls",
          "[midi][raw_midi_parser][issue-645]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };
    auto on_sysex = [&c](const std::vector<uint8_t>& sx) {
        c.sysex.push_back(sx);
    };

    uint8_t chunk1[] = {0x90, 0x40};
    uint8_t chunk2[] = {0x7F};
    parse_raw_midi_bytes(chunk1, sizeof(chunk1), state, on_short, on_sysex);
    REQUIRE(c.shorts.empty());

    parse_raw_midi_bytes(chunk2, sizeof(chunk2), state, on_short, on_sysex);
    REQUIRE(c.shorts.size() == 1);
    REQUIRE(c.shorts[0].status == 0x90);
    REQUIRE(c.shorts[0].d1 == 0x40);
    REQUIRE(c.shorts[0].d2 == 0x7F);
}

TEST_CASE("raw_midi_parser preserves running status across chunks",
          "[midi][raw_midi_parser][issue-645]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };

    uint8_t first[] = {0x90, 0x3C, 0x64, 0x3D};
    uint8_t second[] = {0x65, 0x3E, 0x66};
    parse_raw_midi_bytes(first, sizeof(first), state, on_short, {});
    REQUIRE(c.shorts.size() == 1);

    parse_raw_midi_bytes(second, sizeof(second), state, on_short, {});
    REQUIRE(c.shorts.size() == 3);
    REQUIRE(c.shorts[1].status == 0x90);
    REQUIRE(c.shorts[1].d1 == 0x3D);
    REQUIRE(c.shorts[1].d2 == 0x65);
    REQUIRE(c.shorts[2].d1 == 0x3E);
    REQUIRE(c.shorts[2].d2 == 0x66);
}

TEST_CASE("raw_midi_parser handles one-byte running status messages",
          "[midi][raw_midi_parser][issue-645]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };

    uint8_t bytes[] = {0xC2, 0x05, 0x06, 0xD3, 0x40, 0x41};
    parse_raw_midi_bytes(bytes, sizeof(bytes), state, on_short, {});

    REQUIRE(c.shorts.size() == 4);
    REQUIRE(c.shorts[0].status == 0xC2);
    REQUIRE(c.shorts[0].d1 == 0x05);
    REQUIRE(c.shorts[1].status == 0xC2);
    REQUIRE(c.shorts[1].d1 == 0x06);
    REQUIRE(c.shorts[2].status == 0xD3);
    REQUIRE(c.shorts[2].d1 == 0x40);
    REQUIRE(c.shorts[3].status == 0xD3);
    REQUIRE(c.shorts[3].d1 == 0x41);
}

TEST_CASE("raw_midi_parser realtime bytes do not disturb pending data",
          "[midi][raw_midi_parser][issue-645]") {
    auto c = parse({0x90, 0x40, 0xF8, 0x7F,
                    0x41, 0xF9, 0x70});

    REQUIRE(c.shorts.size() == 4);
    REQUIRE(c.shorts[0].status == 0xF8);
    REQUIRE(c.shorts[1].status == 0x90);
    REQUIRE(c.shorts[1].d1 == 0x40);
    REQUIRE(c.shorts[1].d2 == 0x7F);
    REQUIRE(c.shorts[2].status == 0xF9);
    REQUIRE(c.shorts[3].status == 0x90);
    REQUIRE(c.shorts[3].d1 == 0x41);
    REQUIRE(c.shorts[3].d2 == 0x70);
}

TEST_CASE("raw_midi_parser reprocesses status that interrupts pending data",
          "[midi][raw_midi_parser][issue-645]") {
    auto c = parse({0x90, 0x40,       // partial note-on
                    0xF1, 0x55,       // system-common status interrupts it
                    0x90, 0x41, 0x7F});

    REQUIRE(c.shorts.size() == 2);
    REQUIRE(c.shorts[0].status == 0xF1);
    REQUIRE(c.shorts[0].d1 == 0x55);
    REQUIRE(c.shorts[1].status == 0x90);
    REQUIRE(c.shorts[1].d1 == 0x41);
    REQUIRE(c.shorts[1].d2 == 0x7F);
}

TEST_CASE("raw_midi_parser splits system-common messages across calls",
          "[midi][raw_midi_parser][issue-645]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };

    uint8_t first[] = {0xF2, 0x10};
    uint8_t second[] = {0x20, 0x90, 0x40};
    uint8_t third[] = {0x7F};
    parse_raw_midi_bytes(first, sizeof(first), state, on_short, {});
    REQUIRE(c.shorts.empty());
    parse_raw_midi_bytes(second, sizeof(second), state, on_short, {});
    REQUIRE(c.shorts.size() == 1);
    parse_raw_midi_bytes(third, sizeof(third), state, on_short, {});

    REQUIRE(c.shorts.size() == 2);
    REQUIRE(c.shorts[0].status == 0xF2);
    REQUIRE(c.shorts[0].d1 == 0x10);
    REQUIRE(c.shorts[0].d2 == 0x20);
    REQUIRE(c.shorts[1].status == 0x90);
    REQUIRE(c.shorts[1].d1 == 0x40);
    REQUIRE(c.shorts[1].d2 == 0x7F);
}

TEST_CASE("raw_midi_parser sysex start clears pending short state",
          "[midi][raw_midi_parser][issue-645]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };
    auto on_sysex = [&c](const std::vector<uint8_t>& sx) {
        c.sysex.push_back(sx);
    };

    uint8_t bytes[] = {0x90, 0x40, 0xF0, 0x7D, 0xF7, 0x41, 0x7F,
                       0x90, 0x42, 0x7F};
    parse_raw_midi_bytes(bytes, sizeof(bytes), state, on_short, on_sysex);

    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x7D, 0xF7});
    REQUIRE(c.shorts.size() == 1);
    REQUIRE(c.shorts[0].status == 0x90);
    REQUIRE(c.shorts[0].d1 == 0x42);
    REQUIRE(c.shorts[0].d2 == 0x7F);
}

TEST_CASE("raw_midi_parser undefined system statuses clear running state",
          "[midi][raw_midi_parser][issue-645]") {
    auto c = parse({0x90, 0x3C, 0x7F,
                    0xF4,              // undefined status cancels running status
                    0x3D, 0x7F,        // must be ignored
                    0x90, 0x40, 0x7F});

    REQUIRE(c.shorts.size() == 2);
    REQUIRE(c.shorts[0].d1 == 0x3C);
    REQUIRE(c.shorts[1].d1 == 0x40);
}

TEST_CASE("raw_midi_parser recovers when short-message data is another status",
          "[midi][raw_midi_parser][codecov]") {
    auto c = parse({0x90, 0x40,       // malformed Note On: next byte is status
                    0x91, 0x41, 0x7F,
                    0xC2,             // malformed Program Change: next byte is status
                    0xF6});

    REQUIRE(c.shorts.size() == 2);
    REQUIRE(c.shorts[0].status == 0x91);
    REQUIRE(c.shorts[0].d1 == 0x41);
    REQUIRE(c.shorts[0].d2 == 0x7F);
    REQUIRE(c.shorts[1].status == 0xF6);
    REQUIRE(c.shorts[1].d1 == 0x00);
    REQUIRE(c.shorts[1].d2 == 0x00);
    REQUIRE(c.sysex.empty());
}

TEST_CASE("raw_midi_parser recovers after status interrupts short data",
          "[midi][raw_midi_parser][coverage][phase3-large]") {
    auto c = parse({
        0x90, 0x40, 0xF8,  // realtime byte does not interrupt pending Note On
        0x41, 0x7F,        // first byte completes the Note On; second starts running status
        0x80, 0x40, 0x00,  // fresh Note Off still parses normally
        0xF2, 0x10, 0xF6,  // Tune Request interrupts incomplete Song Position
        0x20,              // stray data must not finish stale 0xF2
        0xF3, 0x05,        // Song Select still parses normally
    });

    REQUIRE(c.shorts.size() == 5);
    REQUIRE(c.shorts[0].status == 0xF8);
    REQUIRE(c.shorts[1].status == 0x90);
    REQUIRE(c.shorts[1].d1 == 0x40);
    REQUIRE(c.shorts[1].d2 == 0x41);
    REQUIRE(c.shorts[2].status == 0x80);
    REQUIRE(c.shorts[2].d1 == 0x40);
    REQUIRE(c.shorts[2].d2 == 0x00);
    REQUIRE(c.shorts[3].status == 0xF6);
    REQUIRE(c.shorts[4].status == 0xF3);
    REQUIRE(c.shorts[4].d1 == 0x05);
    REQUIRE(c.sysex.empty());
}

TEST_CASE("raw_midi_parser accumulates sysex across calls",
          "[midi][raw_midi_parser][issue-406]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };
    auto on_sysex = [&c](const std::vector<uint8_t>& sx) {
        c.sysex.push_back(sx);
    };

    uint8_t chunk1[] = {0xF0, 0x7E, 0x00, 0x06};
    uint8_t chunk2[] = {0x01, 0x02, 0xF7};
    parse_raw_midi_bytes(chunk1, sizeof(chunk1), state, on_short, on_sysex);
    REQUIRE(c.sysex.empty());
    REQUIRE(state.sysex_in_progress);

    parse_raw_midi_bytes(chunk2, sizeof(chunk2), state, on_short, on_sysex);
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0].size() == 7);
    REQUIRE(c.sysex[0].front() == 0xF0);
    REQUIRE(c.sysex[0].back() == 0xF7);
    REQUIRE_FALSE(state.sysex_in_progress);
}

TEST_CASE("raw_midi_parser carries partial short messages across calls",
          "[midi][raw_midi_parser][coverage]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };
    auto on_sysex = [&c](const std::vector<uint8_t>& sx) {
        c.sysex.push_back(sx);
    };

    uint8_t chunk1[] = {0x90, 0x40};
    uint8_t chunk2[] = {0x7F, 0x80, 0x40, 0x00};
    parse_raw_midi_bytes(chunk1, sizeof(chunk1), state, on_short, on_sysex);
    parse_raw_midi_bytes(chunk2, sizeof(chunk2), state, on_short, on_sysex);

    REQUIRE(c.shorts.size() == 2);
    REQUIRE(c.shorts[0].status == 0x90);
    REQUIRE(c.shorts[0].d1 == 0x40);
    REQUIRE(c.shorts[0].d2 == 0x7F);
    REQUIRE(c.shorts[1].status == 0x80);
    REQUIRE(c.shorts[1].d1 == 0x40);
    REQUIRE(c.shorts[1].d2 == 0x00);
    REQUIRE(c.sysex.empty());
    REQUIRE_FALSE(state.sysex_in_progress);
}

TEST_CASE("raw_midi_parser accepts null zero-length input",
          "[midi][raw_midi_parser][coverage]") {
    RawMidiParserState state;
    state.sysex_in_progress = true;
    state.sysex_buffer = {0xF0, 0x7D};
    Captured c;

    parse_raw_midi_bytes(nullptr, 0, state,
                         [&c](uint8_t s, uint8_t d1, uint8_t d2) {
                             c.shorts.push_back({s, d1, d2});
                         },
                         [&c](const std::vector<uint8_t>& sx) {
                             c.sysex.push_back(sx);
                         });

    REQUIRE(c.shorts.empty());
    REQUIRE(c.sysex.empty());
    REQUIRE(state.sysex_in_progress);
    REQUIRE((state.sysex_buffer == std::vector<uint8_t>{0xF0, 0x7D}));
}

TEST_CASE("raw_midi_parser empty reads preserve pending sysex state",
          "[midi][raw_midi_parser][coverage][phase3]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };
    auto on_sysex = [&c](const std::vector<uint8_t>& sx) {
        c.sysex.push_back(sx);
    };

    uint8_t start[] = {0xF0, 0x7D};
    parse_raw_midi_bytes(start, sizeof(start), state, on_short, on_sysex);
    REQUIRE(state.sysex_in_progress);

    parse_raw_midi_bytes(nullptr, 0, state, on_short, on_sysex);
    REQUIRE(state.sysex_in_progress);
    REQUIRE(state.sysex_buffer == std::vector<uint8_t>{0xF0, 0x7D});
    REQUIRE(c.shorts.empty());
    REQUIRE(c.sysex.empty());

    uint8_t finish[] = {0x01, 0xF7};
    parse_raw_midi_bytes(finish, sizeof(finish), state, on_short, on_sysex);
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});
}

TEST_CASE("raw_midi_parser restarts sysex when F0 interrupts partial sysex",
          "[midi][raw_midi_parser][coverage]") {
    auto c = parse({0xF0, 0x7D, 0x01,
                    0xF0, 0x7E, 0x7F, 0xF7});

    REQUIRE(c.shorts.empty());
    REQUIRE(c.sysex.size() == 1);
    REQUIRE((c.sysex[0] == std::vector<uint8_t>{0xF0, 0x7E, 0x7F, 0xF7}));
}

TEST_CASE("raw_midi_parser emits realtime inside sysex without terminating",
          "[midi][raw_midi_parser][issue-239]") {
    // F0 7E <clock F8 interleaved> 05 F7
    auto c = parse({0xF0, 0x7E, 0xF8, 0x05, 0xF7});
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x7E, 0x05, 0xF7});
    REQUIRE(c.shorts.size() == 1);
    REQUIRE(c.shorts[0].status == 0xF8); // MIDI Clock
}

TEST_CASE("raw_midi_parser restarts sysex when a new F0 arrives mid-stream",
          "[midi][raw_midi_parser][coverage][phase3]") {
    auto c = parse({
        0xF0, 0x7D, 0x01,  // abandoned vendor sysex
        0xF0, 0x7E, 0x02, 0xF7,
    });

    REQUIRE(c.shorts.empty());
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x7E, 0x02, 0xF7});
}

TEST_CASE("raw_midi_parser tolerates omitted callbacks",
          "[midi][raw_midi_parser][issue-645]") {
    RawMidiParserState state;
    uint8_t bytes[] = {0xF0, 0x7E, 0xF8, 0x01, 0xF7,
                       0x90, 0x40, 0x7F};

    parse_raw_midi_bytes(bytes, sizeof(bytes), state,
                         RawMidiShortCallback{},
                         RawMidiSysexCallback{});

    REQUIRE_FALSE(state.sysex_in_progress);
    REQUIRE(state.sysex_buffer.empty());
}

TEST_CASE("raw_midi_parser recovers from aborted sysex (#406)",
          "[midi][raw_midi_parser][issue-406]") {
    // Device sends F0 7E <partial> then sends a Note On (0x90) without
    // ever emitting F7. Before the fix, the Note On + its data bytes
    // got swallowed as sysex payload until the NEXT F7 arrived. After
    // the fix, the aborted F0 is dropped and the Note On fires normally.
    auto c = parse({0xF0, 0x7E, 0x00,         // aborted sysex start
                    0x90, 0x40, 0x7F,         // Note On 0x90 that used to be eaten
                    0xF0, 0x01, 0xF7});       // clean sysex afterwards
    REQUIRE(c.shorts.size() == 1);
    REQUIRE(c.shorts[0].status == 0x90);
    REQUIRE(c.shorts[0].d1 == 0x40);
    REQUIRE(c.shorts[0].d2 == 0x7F);
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x01, 0xF7});
}

TEST_CASE("raw_midi_parser restarts sysex on nested start byte",
          "[midi][raw_midi_parser][coverage][phase3]") {
    auto c = parse({
        0xF0, 0x7E, 0x00,  // abandoned sysex prefix
        0xF0, 0x01, 0xF7,  // fresh complete sysex
    });

    REQUIRE(c.shorts.empty());
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x01, 0xF7});
}

TEST_CASE("raw_midi_parser aborted sysex handles system-common too",
          "[midi][raw_midi_parser][issue-406]") {
    // 0xF1 (MTC Quarter Frame) is a 2-byte system-common status that
    // also aborts an in-progress sysex. Ensure its single data byte is
    // parsed correctly after the abort.
    auto c = parse({0xF0, 0x7E,         // F0 stream, never terminated
                    0xF1, 0x55,         // MTC quarter frame after abort
                    0x90, 0x40, 0x7F}); // Note On afterwards
    REQUIRE(c.shorts.size() == 2);
    REQUIRE(c.shorts[0].status == 0xF1);
    REQUIRE(c.shorts[0].d1 == 0x55);
    REQUIRE(c.shorts[1].status == 0x90);
}

TEST_CASE("raw_midi_parser caps runaway sysex at kRawMidiSysexLimit",
          "[midi][raw_midi_parser]") {
    RawMidiParserState state;
    Captured c;
    auto on_short = [&c](uint8_t s, uint8_t d1, uint8_t d2) {
        c.shorts.push_back({s, d1, d2});
    };
    auto on_sysex = [&c](const std::vector<uint8_t>& sx) {
        c.sysex.push_back(sx);
    };

    // Feed F0 + kRawMidiSysexLimit bytes with no F7 — accumulator must
    // drop the run rather than grow unbounded.
    std::vector<uint8_t> huge;
    huge.reserve(kRawMidiSysexLimit + 2);
    huge.push_back(0xF0);
    for (std::size_t i = 0; i < kRawMidiSysexLimit + 1; ++i) {
        huge.push_back(0x42);
    }
    parse_raw_midi_bytes(huge.data(), huge.size(), state, on_short, on_sysex);

    REQUIRE(c.sysex.empty());
    REQUIRE_FALSE(state.sysex_in_progress);
    REQUIRE(state.sysex_buffer.empty());

    uint8_t clean[] = {0xF0, 0x01, 0xF7};
    parse_raw_midi_bytes(clean, sizeof(clean), state, on_short, on_sysex);
    REQUIRE(c.sysex.size() == 1);
    REQUIRE(c.sysex[0] == std::vector<uint8_t>{0xF0, 0x01, 0xF7});
}
