// Tests for SysExAccumulator (#86).
//
// Verifies the state machine handles the cases called out in the header
// and referenced by the Codex audit on #406 (aborted F0 recovery):
//
//   - single-packet sysex
//   - sysex spanning multiple feed() calls
//   - aborted sysex (new status before F7)
//   - realtime bytes interleaved with sysex
//   - short messages outside sysex pass through
//   - reset() drops partial state

#include <pulp/midi/sysex_accumulator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using pulp::midi::SysexAccumulator;
using Classification = SysexAccumulator::Classification;

namespace {

struct Emit {
    std::vector<std::vector<std::uint8_t>> complete;
    std::vector<std::vector<std::uint8_t>> aborted;
};

auto make_callback(Emit& e) {
    return [&](const std::vector<std::uint8_t>& payload, bool complete) {
        if (complete) e.complete.push_back(payload);
        else e.aborted.push_back(payload);
    };
}

} // namespace

TEST_CASE("SysexAccumulator: single-packet complete sysex",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    // Standard Universal Device Inquiry: F0 7E 7F 06 01 F7
    std::uint8_t packet[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    acc.feed(packet, sizeof(packet), make_callback(e));
    REQUIRE(e.complete.size() == 1);
    REQUIRE(e.aborted.empty());
    REQUIRE(e.complete[0].size() == 6);
    REQUIRE(e.complete[0][0] == 0xF0);
    REQUIRE(e.complete[0][5] == 0xF7);
}

TEST_CASE("SysexAccumulator: sysex spanning multiple feed calls",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    std::uint8_t part1[] = {0xF0, 0x41, 0x10};
    std::uint8_t part2[] = {0x42, 0x12, 0x40, 0x00, 0x7F};
    std::uint8_t part3[] = {0x41, 0xF7};
    acc.feed(part1, sizeof(part1), cb);
    REQUIRE(e.complete.empty());
    REQUIRE(acc.in_progress());
    acc.feed(part2, sizeof(part2), cb);
    REQUIRE(e.complete.empty());
    REQUIRE(acc.in_progress());
    acc.feed(part3, sizeof(part3), cb);
    REQUIRE(e.complete.size() == 1);
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(e.complete[0].size() == 3 + 5 + 2);
    REQUIRE(e.complete[0].back() == 0xF7);
}

TEST_CASE("SysexAccumulator: aborted by new status byte",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    // F0 41 10 [Note On 90 3C 7F]  — the 0x90 aborts the sysex.
    std::uint8_t packet[] = {0xF0, 0x41, 0x10, 0x90, 0x3C, 0x7F};
    acc.feed(packet, sizeof(packet), cb);
    REQUIRE(e.aborted.size() == 1);
    REQUIRE(e.aborted[0].size() == 3);
    REQUIRE(e.aborted[0][0] == 0xF0);
    // After the abort the accumulator re-fed the status byte as a fresh
    // classification — it should now be idle, with the Note On bytes
    // classified as passthrough (short message for the caller).
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(e.complete.empty());
}

TEST_CASE("SysexAccumulator: fresh F0 aborts and restarts sysex",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    // A transport may drop an EOX and immediately deliver the next F0.
    // The range feed should emit the partial payload as aborted, then
    // reclassify the same F0 as the start of a fresh SysEx message.
    std::uint8_t stream[] = {
        0xF0, 0x41, 0x10,
        0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7
    };

    acc.feed(stream, sizeof(stream), cb);

    REQUIRE(e.aborted.size() == 1);
    REQUIRE((e.aborted[0] == std::vector<std::uint8_t>{0xF0, 0x41, 0x10}));
    REQUIRE(e.complete.size() == 1);
    REQUIRE((e.complete[0] ==
             std::vector<std::uint8_t>{0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}));
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
}

TEST_CASE("SysexAccumulator: single-byte abort leaves aborter unconsumed",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    REQUIRE(acc.feed(0xF0, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x41, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x90, cb) == Classification::aborted);

    REQUIRE(e.aborted.size() == 1);
    REQUIRE((e.aborted[0] == std::vector<std::uint8_t>{0xF0, 0x41}));
    REQUIRE(e.complete.empty());
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);

    // The caller owns routing the aborting status after single-byte feed().
    REQUIRE(acc.feed(0x90, cb) == Classification::passthrough);
    REQUIRE(acc.feed(0x3C, cb) == Classification::passthrough);
    REQUIRE(acc.feed(0x7F, cb) == Classification::passthrough);
}

TEST_CASE("SysexAccumulator: realtime bytes pass through mid-sysex",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    REQUIRE(acc.feed(0xF0, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x41, cb) == Classification::in_sysex);
    // Timing Clock (0xF8) arrives — pass through, no effect on sysex.
    REQUIRE(acc.feed(0xF8, cb) == Classification::passthrough);
    REQUIRE(acc.in_progress());
    REQUIRE(acc.feed(0x10, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0xF7, cb) == Classification::completed);

    REQUIRE(e.complete.size() == 1);
    // Realtime 0xF8 was NOT buffered into the sysex payload.
    REQUIRE(e.complete[0].size() == 4);
    REQUIRE(e.complete[0][0] == 0xF0);
    REQUIRE(e.complete[0][1] == 0x41);
    REQUIRE(e.complete[0][2] == 0x10);
    REQUIRE(e.complete[0][3] == 0xF7);
}

TEST_CASE("SysexAccumulator: short messages pass through when idle",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    // Note On — not a sysex, no F0 to open.
    REQUIRE(acc.feed(0x90, cb) == Classification::passthrough);
    REQUIRE(acc.feed(0x3C, cb) == Classification::passthrough);
    REQUIRE(acc.feed(0x7F, cb) == Classification::passthrough);
    REQUIRE(e.complete.empty());
    REQUIRE(e.aborted.empty());
}

TEST_CASE("SysexAccumulator: EOX passes through when idle",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    REQUIRE(acc.feed(0xF7, cb) == Classification::passthrough);
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
    REQUIRE(e.complete.empty());
    REQUIRE(e.aborted.empty());
}

TEST_CASE("SysexAccumulator: zero-length range feed is a no-op",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    const std::uint8_t* no_bytes = nullptr;

    acc.feed(no_bytes, 0, cb);
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
    REQUIRE(e.complete.empty());
    REQUIRE(e.aborted.empty());

    REQUIRE(acc.feed(0xF0, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x7D, cb) == Classification::in_sysex);

    acc.feed(no_bytes, 0, cb);
    REQUIRE(acc.in_progress());
    REQUIRE(acc.partial_size() == 2);
    REQUIRE(e.complete.empty());
    REQUIRE(e.aborted.empty());

    REQUIRE(acc.feed(0xF7, cb) == Classification::completed);
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
    REQUIRE(e.aborted.empty());
    REQUIRE(e.complete.size() == 1);
    REQUIRE((e.complete[0] == std::vector<std::uint8_t>{0xF0, 0x7D, 0xF7}));
}

TEST_CASE("SysexAccumulator: duplicate EOX tails stay idle",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    std::uint8_t stream[] = {
        0xF0, 0x7D, 0x01, 0xF7,
        0xF7, 0xF7,
        0xF0, 0x7E, 0x00, 0xF7
    };

    acc.feed(stream, sizeof(stream), cb);

    REQUIRE(e.aborted.empty());
    REQUIRE(e.complete.size() == 2);
    REQUIRE((e.complete[0] ==
             std::vector<std::uint8_t>{0xF0, 0x7D, 0x01, 0xF7}));
    REQUIRE((e.complete[1] ==
             std::vector<std::uint8_t>{0xF0, 0x7E, 0x00, 0xF7}));
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
}

TEST_CASE("SysexAccumulator: system-common abort tail stays idle",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    std::uint8_t stream[] = {
        0xF0, 0x7D, 0x01,
        0xF2, 0x34, 0x12, 0xF7,
        0xF0, 0x7E, 0x7F, 0xF7
    };

    acc.feed(stream, sizeof(stream), cb);

    REQUIRE(e.aborted.size() == 1);
    REQUIRE((e.aborted[0] == std::vector<std::uint8_t>{0xF0, 0x7D, 0x01}));
    REQUIRE(e.complete.size() == 1);
    REQUIRE((e.complete[0] ==
             std::vector<std::uint8_t>{0xF0, 0x7E, 0x7F, 0xF7}));
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
}

TEST_CASE("SysexAccumulator: reset is idempotent after idle and abort",
          "[midi][sysex][issue-645]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    acc.reset();
    acc.reset();
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
    REQUIRE(e.complete.empty());
    REQUIRE(e.aborted.empty());

    REQUIRE(acc.feed(0xF0, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x7D, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x90, cb) == Classification::aborted);
    REQUIRE(e.aborted.size() == 1);
    REQUIRE((e.aborted[0] == std::vector<std::uint8_t>{0xF0, 0x7D}));

    acc.reset();
    acc.reset();
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
    REQUIRE(acc.feed(0xF7, cb) == Classification::passthrough);
    REQUIRE(e.complete.empty());

    std::uint8_t fresh[] = {0xF0, 0x7D, 0x02, 0xF7};
    acc.feed(fresh, sizeof(fresh), cb);
    REQUIRE(e.complete.size() == 1);
    REQUIRE((e.complete[0] ==
             std::vector<std::uint8_t>{0xF0, 0x7D, 0x02, 0xF7}));
}

TEST_CASE("SysexAccumulator: reset drops partial state",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    acc.feed(0xF0, cb);
    acc.feed(0x41, cb);
    acc.feed(0x10, cb);
    REQUIRE(acc.in_progress());
    REQUIRE(acc.partial_size() == 3);
    acc.reset();
    REQUIRE_FALSE(acc.in_progress());
    REQUIRE(acc.partial_size() == 0);
    // After reset, subsequent F0 starts a fresh sysex.
    std::uint8_t fresh[] = {0xF0, 0x7E, 0x7F, 0xF7};
    acc.feed(fresh, sizeof(fresh), cb);
    REQUIRE(e.complete.size() == 1);
    REQUIRE(e.complete[0].size() == 4);
}

TEST_CASE("SysexAccumulator: back-to-back sysex messages",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    std::uint8_t stream[] = {
        0xF0, 0x41, 0x10, 0xF7,
        0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7,
        0xF0, 0x42, 0xF7
    };
    acc.feed(stream, sizeof(stream), cb);
    REQUIRE(e.complete.size() == 3);
    REQUIRE(e.complete[0].size() == 4);
    REQUIRE(e.complete[1].size() == 6);
    REQUIRE(e.complete[2].size() == 3);
    REQUIRE_FALSE(acc.in_progress());
}

TEST_CASE("SysexAccumulator: empty sysex (F0 F7)",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    std::uint8_t empty[] = {0xF0, 0xF7};
    acc.feed(empty, sizeof(empty), cb);
    REQUIRE(e.complete.size() == 1);
    REQUIRE(e.complete[0].size() == 2);
    REQUIRE(e.complete[0][0] == 0xF0);
    REQUIRE(e.complete[0][1] == 0xF7);
}

TEST_CASE("SysexAccumulator: large sysex (>1KB) accumulates correctly",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    std::vector<std::uint8_t> big;
    big.push_back(0xF0);
    for (int i = 0; i < 1500; ++i) {
        big.push_back(static_cast<std::uint8_t>(i & 0x7F));
    }
    big.push_back(0xF7);
    acc.feed(big.data(), big.size(), cb);
    REQUIRE(e.complete.size() == 1);
    REQUIRE(e.complete[0].size() == 1502);
    REQUIRE(e.complete[0].front() == 0xF0);
    REQUIRE(e.complete[0].back() == 0xF7);
}

TEST_CASE("SysexAccumulator: reserved 0xF9/0xFD pass through mid-sysex",
          "[midi][sysex][issue-500]") {
    // Regression for the Codex P2 finding on #484 / #500: 0xF9 and 0xFD
    // are MIDI-1.0-undefined codes in the System Realtime range. The
    // accumulator's pass-through contract covers all of F8-FF except F7,
    // but an earlier is_realtime() carved out 0xF9 and 0xFD, which sent
    // them down the aborted-status path and truncated otherwise-valid
    // SysEx streams on transports that emitted the reserved codes.
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);

    REQUIRE(acc.feed(0xF0, cb) == Classification::in_sysex);
    REQUIRE(acc.feed(0x41, cb) == Classification::in_sysex);
    // Reserved realtime 0xF9 — treat as pass-through, NOT an abort.
    REQUIRE(acc.feed(0xF9, cb) == Classification::passthrough);
    REQUIRE(acc.in_progress());
    REQUIRE(acc.feed(0x10, cb) == Classification::in_sysex);
    // Reserved realtime 0xFD — same story.
    REQUIRE(acc.feed(0xFD, cb) == Classification::passthrough);
    REQUIRE(acc.in_progress());
    REQUIRE(acc.feed(0xF7, cb) == Classification::completed);

    REQUIRE(e.aborted.empty());
    REQUIRE(e.complete.size() == 1);
    // Reserved bytes are NOT buffered into the payload — only F0 41 10 F7.
    REQUIRE(e.complete[0].size() == 4);
    REQUIRE(e.complete[0][0] == 0xF0);
    REQUIRE(e.complete[0][1] == 0x41);
    REQUIRE(e.complete[0][2] == 0x10);
    REQUIRE(e.complete[0][3] == 0xF7);
}

TEST_CASE("SysexAccumulator: aborted then recovered sysex",
          "[midi][sysex][issue-86]") {
    SysexAccumulator acc;
    Emit e;
    auto cb = make_callback(e);
    // Partial sysex interrupted by a Note On, then a fresh sysex.
    std::uint8_t stream[] = {
        0xF0, 0x41, 0x10,                   // partial
        0x90, 0x3C, 0x7F,                   // Note On — aborts
        0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7  // complete
    };
    acc.feed(stream, sizeof(stream), cb);
    REQUIRE(e.aborted.size() == 1);
    REQUIRE(e.aborted[0].size() == 3);  // F0 41 10 without F7
    REQUIRE(e.complete.size() == 1);
    REQUIRE(e.complete[0].size() == 6);
}
