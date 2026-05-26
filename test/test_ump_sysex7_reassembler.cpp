// Tests for UmpSysex7Reassembler — shared UMP Type-0x3 sysex7
// reassembler extracted from the AUv3 / CoreMIDI inline state machines
// (macOS plugin authoring plan item 8.2).
//
// Pinned regressions:
//   - #239 / #292 P1: per-packet word cursor must advance correctly;
//     the second word's top nibble can match a continuation packet's
//     payload byte. Tested by feeding contrived packet pairs where
//     word1 begins with what looks like a status byte.
//   - #292 P2: orphaned continue/end packets are dropped silently;
//     they must not corrupt an unrelated in-progress accumulator or
//     emit a partial payload.
//   - Interleaved single-packet sysex (status 0x0) on top of an open
//     start (status 0x1) accumulator must not consume the open state.
//   - Reserved status nibbles (0x4..0xF) drop without disturbing the
//     accumulator.

#include <pulp/midi/ump_sysex7_reassembler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using pulp::midi::UmpSysex7Reassembler;
using pulp::midi::feed_collect;
using Status = UmpSysex7Reassembler::Status;

namespace {

// Build a UMP type-0x3 word0 given status nibble, payload size, and up
// to two payload bytes packed into word0's low 16 bits. Group is 0.
constexpr std::uint32_t make_word0(std::uint8_t status,
                                   std::uint8_t size,
                                   std::uint8_t b0 = 0,
                                   std::uint8_t b1 = 0) {
    const std::uint32_t type    = 0x3u << 28;
    const std::uint32_t group   = 0u   << 24;
    const std::uint32_t s_field = static_cast<std::uint32_t>(status & 0x0F) << 20;
    const std::uint32_t z_field = static_cast<std::uint32_t>(size   & 0x0F) << 16;
    return type | group | s_field | z_field |
           (static_cast<std::uint32_t>(b0) << 8) |
            static_cast<std::uint32_t>(b1);
}

// Build word1 from up to four payload bytes.
constexpr std::uint32_t make_word1(std::uint8_t b2 = 0,
                                   std::uint8_t b3 = 0,
                                   std::uint8_t b4 = 0,
                                   std::uint8_t b5 = 0) {
    return (static_cast<std::uint32_t>(b2) << 24)
         | (static_cast<std::uint32_t>(b3) << 16)
         | (static_cast<std::uint32_t>(b4) <<  8)
         |  static_cast<std::uint32_t>(b5);
}

struct EmitSink {
    std::vector<std::vector<std::uint8_t>> sysex;
};

void capture(const std::vector<std::uint8_t>& payload, void* user) {
    static_cast<EmitSink*>(user)->sysex.push_back(payload);
}

} // namespace

TEST_CASE("UmpSysex7Reassembler: single-packet sysex (status 0x0)",
          "[midi][ump][sysex7][issue-292]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    // Universal Device Inquiry payload bytes (no F0/F7 framing in UMP).
    const std::uint8_t bytes[6] = {0x7E, 0x7F, 0x06, 0x01, 0x02, 0x03};
    const std::uint32_t w0 = make_word0(0x0, 6, bytes[0], bytes[1]);
    const std::uint32_t w1 = make_word1(bytes[2], bytes[3], bytes[4], bytes[5]);

    auto status = r.feed_packet(w0, w1, capture, &sink);
    REQUIRE(status == Status::single_packet);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]});
}

TEST_CASE("UmpSysex7Reassembler: start → end multi-packet sysex",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    // First packet: start, 6 bytes.
    auto s1 = r.feed_packet(make_word0(0x1, 6, 0x10, 0x11),
                            make_word1(0x12, 0x13, 0x14, 0x15),
                            capture, &sink);
    REQUIRE(s1 == Status::start);
    REQUIRE(r.in_progress());
    REQUIRE(r.partial_size() == 6);
    REQUIRE(sink.sysex.empty());

    // End packet: 4 bytes.
    auto s2 = r.feed_packet(make_word0(0x3, 4, 0x20, 0x21),
                            make_word1(0x22, 0x23, 0x00, 0x00),
                            capture, &sink);
    REQUIRE(s2 == Status::ended);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x20, 0x21, 0x22, 0x23});
}

TEST_CASE("UmpSysex7Reassembler: start → continue* → end span",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    REQUIRE(r.feed_packet(make_word0(0x1, 6, 0x01, 0x02),
                          make_word1(0x03, 0x04, 0x05, 0x06),
                          capture, &sink) == Status::start);
    REQUIRE(r.feed_packet(make_word0(0x2, 6, 0x07, 0x08),
                          make_word1(0x09, 0x0A, 0x0B, 0x0C),
                          capture, &sink) == Status::continued);
    REQUIRE(r.feed_packet(make_word0(0x2, 3, 0x0D, 0x0E),
                          make_word1(0x0F, 0x00, 0x00, 0x00),
                          capture, &sink) == Status::continued);
    REQUIRE(r.feed_packet(make_word0(0x3, 2, 0x10, 0x11),
                          make_word1(0x00, 0x00, 0x00, 0x00),
                          capture, &sink) == Status::ended);

    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0].size() == 6 + 6 + 3 + 2);
    REQUIRE(sink.sysex[0].front() == 0x01);
    REQUIRE(sink.sysex[0].back() == 0x11);
}

TEST_CASE("UmpSysex7Reassembler: orphan continue is dropped silently (#292 P2)",
          "[midi][ump][sysex7][issue-292]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    auto s = r.feed_packet(make_word0(0x2, 4, 0xAA, 0xBB),
                           make_word1(0xCC, 0xDD, 0, 0),
                           capture, &sink);
    REQUIRE(s == Status::dropped);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(sink.sysex.empty());
}

TEST_CASE("UmpSysex7Reassembler: orphan end is dropped silently (#292 P2)",
          "[midi][ump][sysex7][issue-292]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    auto s = r.feed_packet(make_word0(0x3, 4, 0xAA, 0xBB),
                           make_word1(0xCC, 0xDD, 0, 0),
                           capture, &sink);
    REQUIRE(s == Status::dropped);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(sink.sysex.empty());
}

TEST_CASE("UmpSysex7Reassembler: interleaved single-packet preserves open accumulator",
          "[midi][ump][sysex7][issue-292]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    // Open an in-progress sysex.
    REQUIRE(r.feed_packet(make_word0(0x1, 4, 0xAA, 0xBB),
                          make_word1(0xCC, 0xDD, 0, 0),
                          capture, &sink) == Status::start);
    REQUIRE(r.in_progress());
    REQUIRE(r.partial_size() == 4);

    // Interleave a complete single-packet sysex (different logical
    // stream, e.g. different UMP group). The single-packet path must
    // not consume our open accumulator.
    REQUIRE(r.feed_packet(make_word0(0x0, 2, 0x55, 0x66),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::single_packet);
    REQUIRE(r.in_progress());
    REQUIRE(r.partial_size() == 4);  // unchanged
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{0x55, 0x66});

    // Close the original accumulator.
    REQUIRE(r.feed_packet(make_word0(0x3, 2, 0xEE, 0xFF),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::ended);
    REQUIRE(sink.sysex.size() == 2);
    REQUIRE(sink.sysex[1] == std::vector<std::uint8_t>{
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
}

TEST_CASE("UmpSysex7Reassembler: nested start resets accumulator (no nest)",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    REQUIRE(r.feed_packet(make_word0(0x1, 4, 0x01, 0x02),
                          make_word1(0x03, 0x04, 0, 0),
                          capture, &sink) == Status::start);
    // Another start before end — drop the half-built sysex, begin again.
    REQUIRE(r.feed_packet(make_word0(0x1, 3, 0xAA, 0xBB),
                          make_word1(0xCC, 0, 0, 0),
                          capture, &sink) == Status::start);
    REQUIRE(r.partial_size() == 3);
    REQUIRE(sink.sysex.empty());

    REQUIRE(r.feed_packet(make_word0(0x3, 1, 0xDD, 0x00),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::ended);
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{
        0xAA, 0xBB, 0xCC, 0xDD});
}

TEST_CASE("UmpSysex7Reassembler: reset() drops in-progress state",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    REQUIRE(r.feed_packet(make_word0(0x1, 3, 0x01, 0x02),
                          make_word1(0x03, 0, 0, 0),
                          capture, &sink) == Status::start);
    REQUIRE(r.in_progress());
    r.reset();
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(r.partial_size() == 0);

    // After reset, a continue packet must be treated as orphan.
    REQUIRE(r.feed_packet(make_word0(0x2, 1, 0xFF, 0),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::dropped);
    REQUIRE(sink.sysex.empty());
}

TEST_CASE("UmpSysex7Reassembler: reserved status nibble drops cleanly",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    // Open a real sysex so we can prove reserved-status doesn't touch it.
    REQUIRE(r.feed_packet(make_word0(0x1, 3, 0x01, 0x02),
                          make_word1(0x03, 0, 0, 0),
                          capture, &sink) == Status::start);

    // status nibble 0x7 — reserved per the UMP spec.
    REQUIRE(r.feed_packet(make_word0(0x7, 6, 0x99, 0x99),
                          make_word1(0x99, 0x99, 0x99, 0x99),
                          capture, &sink) == Status::dropped);
    REQUIRE(r.in_progress());
    REQUIRE(r.partial_size() == 3);

    // Original sysex still closes cleanly.
    REQUIRE(r.feed_packet(make_word0(0x3, 1, 0x04, 0),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::ended);
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{
        0x01, 0x02, 0x03, 0x04});
}

TEST_CASE("UmpSysex7Reassembler: #292 P1 — word1's top nibble must not be reparsed",
          "[midi][ump][sysex7][issue-292]") {
    // Regression for the #292 P1 bug: a multi-word UMP message must be
    // consumed in full, never re-entered word-by-word. We verify the
    // reassembler does not look at word1 as if it were a fresh word0
    // by constructing a continue packet whose word1's top nibble is
    // 0x3 (matching a sysex7 message-type) and whose status-nibble bits
    // would name "start". The reassembler must treat the whole 2-word
    // packet as ONE continue, appending all 6 bytes verbatim.
    UmpSysex7Reassembler r;
    EmitSink sink;

    REQUIRE(r.feed_packet(make_word0(0x1, 6, 0xAA, 0xBB),
                          make_word1(0xCC, 0xDD, 0xEE, 0xFF),
                          capture, &sink) == Status::start);

    // word1 = 0x31_60_00_00 — top nibble looks like a sysex7 type,
    // status-bits would look like a start. The reassembler must ignore
    // that and treat the whole packet as a single continue.
    const std::uint32_t w0 = make_word0(0x2, 6, 0x11, 0x22);
    const std::uint32_t w1 = 0x31'60'00'00u;
    REQUIRE(r.feed_packet(w0, w1, capture, &sink) == Status::continued);
    REQUIRE(r.partial_size() == 6 + 6);
    REQUIRE(sink.sysex.empty());

    REQUIRE(r.feed_packet(make_word0(0x3, 0, 0, 0),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::ended);
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        0x11, 0x22, 0x31, 0x60, 0x00, 0x00});
}

TEST_CASE("UmpSysex7Reassembler: size > 6 is clamped defensively",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;
    std::vector<std::uint8_t> out;
    // size = 0xF (15) — spec only allows 0..6. We clamp to 6.
    auto s = feed_collect(r,
        make_word0(0x0, 0xF, 0x01, 0x02),
        make_word1(0x03, 0x04, 0x05, 0x06), out);
    REQUIRE(s == Status::single_packet);
    REQUIRE(out.size() == 6);
}

TEST_CASE("UmpSysex7Reassembler: feed_collect convenience returns the payload",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    std::vector<std::uint8_t> out;
    auto s = feed_collect(r,
        make_word0(0x0, 3, 0x7E, 0x7F),
        make_word1(0x06, 0, 0, 0), out);
    REQUIRE(s == Status::single_packet);
    REQUIRE(out == std::vector<std::uint8_t>{0x7E, 0x7F, 0x06});
}

TEST_CASE("UmpSysex7Reassembler: reserve() does not change state",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    r.reserve(1024);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(r.partial_size() == 0);
}
