// SysEx7 reassembly isolates UMP groups and rejects malformed packets and
// oversized whole messages. A complete packet replaces an unfinished message
// on its own group; other groups retain their independent partial payloads.
// Prepared storage bounds both accepted messages and overflow recovery.

#include <pulp/midi/ump_sysex7_reassembler.hpp>

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
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

struct RtEmitSink {
    std::array<std::array<std::uint8_t, 18>, 2> sysex{};
    std::array<std::size_t, 2> sizes{};
    std::size_t count = 0;
};

void capture_rt(const std::vector<std::uint8_t>& payload, void* user) {
    auto* sink = static_cast<RtEmitSink*>(user);
    const auto index = sink->count++;
    sink->sizes[index] = payload.size();
    for (std::size_t i = 0; i < payload.size(); ++i) {
        sink->sysex[index][i] = payload[i];
    }
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

    auto s = r.feed_packet(make_word0(0x2, 4, 0x2A, 0x3B),
                           make_word1(0x4C, 0x5D, 0, 0),
                           capture, &sink);
    REQUIRE(s == Status::dropped);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(sink.sysex.empty());
}

TEST_CASE("UmpSysex7Reassembler: orphan end is dropped silently (#292 P2)",
          "[midi][ump][sysex7][issue-292]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    auto s = r.feed_packet(make_word0(0x3, 4, 0x2A, 0x3B),
                           make_word1(0x4C, 0x5D, 0, 0),
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
    REQUIRE(r.feed_packet(make_word0(0x1, 4, 0x2A, 0x3B),
                          make_word1(0x4C, 0x5D, 0, 0),
                          capture, &sink) == Status::start);
    REQUIRE(r.in_progress());
    REQUIRE(r.partial_size() == 4);

    // Interleave a complete single-packet sysex on another UMP group.
    REQUIRE(r.feed_packet(make_word0(0x0, 2, 0x55, 0x66) | (1u << 24),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::single_packet);
    REQUIRE(r.in_progress());
    REQUIRE(r.partial_size() == 4);  // unchanged
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{0x55, 0x66});

    // Close the original accumulator.
    REQUIRE(r.feed_packet(make_word0(0x3, 2, 0x6E, 0x7F),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::ended);
    REQUIRE(sink.sysex.size() == 2);
    REQUIRE(sink.sysex[1] == std::vector<std::uint8_t>{
        0x2A, 0x3B, 0x4C, 0x5D, 0x6E, 0x7F});
}

TEST_CASE("UmpSysex7Reassembler: nested start resets accumulator (no nest)",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    REQUIRE(r.feed_packet(make_word0(0x1, 4, 0x01, 0x02),
                          make_word1(0x03, 0x04, 0, 0),
                          capture, &sink) == Status::start);
    // Another start before end — drop the half-built sysex, begin again.
    REQUIRE(r.feed_packet(make_word0(0x1, 3, 0x2A, 0x3B),
                          make_word1(0x4C, 0, 0, 0),
                          capture, &sink) == Status::start);
    REQUIRE(r.partial_size() == 3);
    REQUIRE(sink.sysex.empty());

    REQUIRE(r.feed_packet(make_word0(0x3, 1, 0x5D, 0x00),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::ended);
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0] == std::vector<std::uint8_t>{
        0x2A, 0x3B, 0x4C, 0x5D});
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
    REQUIRE(r.feed_packet(make_word0(0x2, 1, 0x7F, 0),
                          make_word1(0, 0, 0, 0),
                          capture, &sink) == Status::dropped);
    REQUIRE(sink.sysex.empty());
}

TEST_CASE("UmpSysex7Reassembler: reserved status nibble drops cleanly",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;

    // Open a real sysex so we can prove reserved-status discards it.
    REQUIRE(r.feed_packet(make_word0(0x1, 3, 0x01, 0x02),
                          make_word1(0x03, 0, 0, 0),
                          capture, &sink) == Status::start);

    // status nibble 0x7 — reserved per the UMP spec.
    REQUIRE(r.feed_packet(make_word0(0x7, 6, 0x19, 0x19),
                          make_word1(0x19, 0x19, 0x19, 0x19),
                          capture, &sink) == Status::dropped);
    REQUIRE(r.last_drop_reason() == UmpSysex7Reassembler::DropReason::reserved_status);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(r.partial_size() == 0);

    REQUIRE(r.feed_packet(make_word0(0x3, 1, 0x04, 0),
                          make_word1(0, 0, 0, 0), capture, &sink) == Status::dropped);
    REQUIRE(sink.sysex.empty());
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

    REQUIRE(r.feed_packet(make_word0(0x1, 6, 0x2A, 0x3B),
                          make_word1(0x4C, 0x5D, 0x6E, 0x7F),
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
        0x2A, 0x3B, 0x4C, 0x5D, 0x6E, 0x7F,
        0x11, 0x22, 0x31, 0x60, 0x00, 0x00});
}

TEST_CASE("UmpSysex7Reassembler: size > 6 is rejected",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    std::vector<std::uint8_t> out;
    // size = 0xF (15) — spec only allows 0..6. Reject the packet.
    auto s = feed_collect(r,
        make_word0(0x0, 0xF, 0x01, 0x02),
        make_word1(0x03, 0x04, 0x05, 0x06), out);
    REQUIRE(s == Status::dropped);
    REQUIRE(out.empty());
    REQUIRE(r.last_drop_reason() == UmpSysex7Reassembler::DropReason::invalid_size);
}

TEST_CASE("UmpSysex7Reassembler: high-bit payload is rejected and recovers",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    std::vector<std::uint8_t> out;
    REQUIRE(feed_collect(r, make_word0(0x0, 1, 0x80), 0, out) == Status::dropped);
    REQUIRE(r.last_drop_reason() == UmpSysex7Reassembler::DropReason::high_bit_payload);
    REQUIRE(feed_collect(r, make_word0(0x0, 1, 0x7F), 0, out) == Status::single_packet);
    REQUIRE(out == std::vector<std::uint8_t>{0x7F});
}

TEST_CASE("UmpSysex7Reassembler: groups retain independent partial messages",
          "[midi][ump][sysex7]") {
    UmpSysex7Reassembler r;
    EmitSink sink;
    auto word0 = [](std::uint8_t group, std::uint8_t status,
                    std::uint8_t size, std::uint8_t b0) {
        return (0x3u << 28) | (std::uint32_t(group) << 24)
             | (std::uint32_t(status) << 20) | (std::uint32_t(size) << 16)
             | (std::uint32_t(b0) << 8);
    };
    REQUIRE(r.feed_packet(word0(0, 1, 1, 0x01), 0, capture, &sink) == Status::start);
    REQUIRE(r.feed_packet(word0(1, 1, 1, 0x11), 0, capture, &sink) == Status::start);
    REQUIRE(r.feed_packet(word0(0, 3, 1, 0x02), 0, capture, &sink) == Status::ended);
    REQUIRE(r.feed_packet(word0(1, 3, 1, 0x12), 0, capture, &sink) == Status::ended);
    REQUIRE(sink.sysex == std::vector<std::vector<std::uint8_t>>{{0x01, 0x02}, {0x11, 0x12}});
}

TEST_CASE("UmpSysex7Reassembler: overflow discards whole message and recovers",
          "[midi][ump][sysex7][capacity]") {
    UmpSysex7Reassembler r(7);
    EmitSink sink;
    REQUIRE(r.feed_packet(make_word0(1, 6, 0x01, 0x02),
                          make_word1(0x03, 0x04, 0x05, 0x06), capture, &sink) == Status::start);
    REQUIRE(r.feed_packet(make_word0(2, 2, 0x07, 0x08), 0, capture, &sink) == Status::dropped);
    REQUIRE(r.last_drop_reason() == UmpSysex7Reassembler::DropReason::overflow);
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(r.feed_packet(make_word0(0, 1, 0x09), 0, capture, &sink) == Status::single_packet);
    REQUIRE(sink.sysex == std::vector<std::vector<std::uint8_t>>{{0x09}});
}

TEST_CASE("UmpSysex7Reassembler: exact configured boundaries are atomic",
          "[midi][ump][sysex7][capacity]") {
    auto packet = [](std::uint8_t status, std::uint8_t size,
                     std::uint8_t seed) {
        return std::pair<std::uint32_t, std::uint32_t>{
            make_word0(status, size, static_cast<std::uint8_t>(seed & 0x7F),
                       static_cast<std::uint8_t>((seed + 1) & 0x7F)),
            make_word1(static_cast<std::uint8_t>((seed + 2) & 0x7F),
                       static_cast<std::uint8_t>((seed + 3) & 0x7F),
                       static_cast<std::uint8_t>((seed + 4) & 0x7F),
                       static_cast<std::uint8_t>((seed + 5) & 0x7F))};
    };
    auto run = [&](std::size_t total, std::size_t limit) {
        UmpSysex7Reassembler r(limit);
        EmitSink sink;
        std::size_t remaining = total;
        std::uint8_t seed = 1;
        auto first = packet(0x1, 6, seed);
        REQUIRE(r.feed_packet(first.first, first.second, capture, &sink) == Status::start);
        remaining -= 6;
        while (remaining > 6) {
            const auto p = packet(0x2, 6, ++seed);
            REQUIRE(r.feed_packet(p.first, p.second, capture, &sink) == Status::continued);
            remaining -= 6;
        }
        const auto last = packet(0x3, static_cast<std::uint8_t>(remaining), ++seed);
        const auto status = r.feed_packet(last.first, last.second, capture, &sink);
        return std::pair{status, sink.sysex.size()};
    };
    REQUIRE(run(1023, 1024) == std::pair{Status::ended, std::size_t{1}});
    REQUIRE(run(1024, 1024) == std::pair{Status::ended, std::size_t{1}});
    REQUIRE(run(1025, 1024) == std::pair{Status::dropped, std::size_t{0}});
}

TEST_CASE("UmpSysex7Reassembler: deterministic 139 and 715 byte payloads",
          "[midi][ump][sysex7][capacity]") {
    auto run = [](std::size_t total) {
        UmpSysex7Reassembler r(total);
        EmitSink sink;
        std::vector<std::uint8_t> expected;
        expected.reserve(total);
        std::size_t remaining = total;
        std::uint8_t seed = 1;
        auto make = [&](std::uint8_t status, std::uint8_t size) {
            const auto b = static_cast<std::uint8_t>(seed++ & 0x7F);
            for (std::uint8_t i = 0; i < size; ++i)
                expected.push_back(static_cast<std::uint8_t>((b + i) & 0x7F));
            return std::pair{make_word0(status, size, b, static_cast<std::uint8_t>((b + 1) & 0x7F)),
                              make_word1(static_cast<std::uint8_t>((b + 2) & 0x7F),
                                         static_cast<std::uint8_t>((b + 3) & 0x7F),
                                         static_cast<std::uint8_t>((b + 4) & 0x7F),
                                         static_cast<std::uint8_t>((b + 5) & 0x7F))};
        };
        auto first = make(1, 6);
        REQUIRE(r.feed_packet(first.first, first.second, capture, &sink) == Status::start);
        remaining -= 6;
        while (remaining > 6) {
            auto p = make(2, 6);
            REQUIRE(r.feed_packet(p.first, p.second, capture, &sink) == Status::continued);
            remaining -= 6;
        }
        auto last = make(3, static_cast<std::uint8_t>(remaining));
        REQUIRE(r.feed_packet(last.first, last.second, capture, &sink) == Status::ended);
        REQUIRE(sink.sysex.size() == 1);
        REQUIRE(sink.sysex.front() == expected);
    };
    run(139);
    run(715);
}

TEST_CASE("UmpSysex7Reassembler: 64 KiB maximum and maximum plus one",
          "[midi][ump][sysex7][capacity]") {
    constexpr std::size_t kMax = 64u * 1024u;
    UmpSysex7Reassembler r(kMax);
    r.reserve(kMax);
    EmitSink sink;
    auto feed = [&](std::uint8_t status, std::uint8_t size) {
        return r.feed_packet(make_word0(status, size, 0x01, 0x02),
                              make_word1(0x03, 0x04, 0x05, 0x06), capture, &sink);
    };
    std::size_t remaining = kMax;
    REQUIRE(feed(1, 6) == Status::start);
    remaining -= 6;
    while (remaining > 6) {
        REQUIRE(feed(2, 6) == Status::continued);
        remaining -= 6;
    }
    REQUIRE(feed(3, static_cast<std::uint8_t>(remaining)) == Status::ended);
    REQUIRE(sink.sysex.size() == 1);
    REQUIRE(sink.sysex[0].size() == kMax);
    for (std::size_t i = 0; i < kMax; ++i)
        REQUIRE(sink.sysex[0][i] == (i % 6) + 1);

    r.reset();
    sink.sysex.clear();
    remaining = kMax + 1;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(feed(1, 6) == Status::start);
        remaining -= 6;
        while (remaining > 6) {
            REQUIRE(feed(2, 6) == Status::continued);
            remaining -= 6;
        }
        REQUIRE(feed(3, static_cast<std::uint8_t>(remaining)) == Status::dropped);
        REQUIRE_FALSE(probe.saw_allocation());
    }
    REQUIRE_FALSE(r.in_progress());
    REQUIRE(sink.sysex.empty());
    REQUIRE(r.last_drop_reason() == UmpSysex7Reassembler::DropReason::overflow);
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

TEST_CASE("UmpSysex7Reassembler: reserved hot path is allocation-free",
          "[midi][ump][sysex7][rt-safety]") {
    UmpSysex7Reassembler r;
    r.reserve(18);
    RtEmitSink sink;

    {
        pulp::test::RtAllocationProbe probe;

        REQUIRE(r.feed_packet(make_word0(0x1, 6, 0x01, 0x02),
                              make_word1(0x03, 0x04, 0x05, 0x06),
                              capture_rt, &sink) == Status::start);
        REQUIRE(r.feed_packet(make_word0(0x2, 6, 0x07, 0x08),
                              make_word1(0x09, 0x0A, 0x0B, 0x0C),
                              capture_rt, &sink) == Status::continued);

        // Complete single-packet sysex while the multi-packet accumulator is
        // open. This exercises the prepared scratch buffer without disturbing
        // the open accumulator.
        REQUIRE(r.feed_packet(make_word0(0x0, 3, 0x55, 0x66) | (1u << 24),
                              make_word1(0x77, 0, 0, 0),
                              capture_rt, &sink) == Status::single_packet);

        REQUIRE(r.feed_packet(make_word0(0x2, 3, 0x0D, 0x0E),
                              make_word1(0x0F, 0, 0, 0),
                              capture_rt, &sink) == Status::continued);
        REQUIRE(r.feed_packet(make_word0(0x3, 3, 0x10, 0x11),
                              make_word1(0x12, 0, 0, 0),
                              capture_rt, &sink) == Status::ended);

        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE_FALSE(r.in_progress());
    REQUIRE(r.partial_size() == 0);
    REQUIRE(sink.count == 2);
    REQUIRE(sink.sizes[0] == 3);
    REQUIRE(sink.sysex[0][0] == 0x55);
    REQUIRE(sink.sysex[0][1] == 0x66);
    REQUIRE(sink.sysex[0][2] == 0x77);
    REQUIRE(sink.sizes[1] == 18);
    REQUIRE(sink.sysex[1][0] == 0x01);
    REQUIRE(sink.sysex[1][17] == 0x12);
}

TEST_CASE("UmpSysex7Reassembler: copies preserve prepared storage and partial groups",
          "[midi][ump][sysex7][rt-safety]") {
    UmpSysex7Reassembler source;
    source.reserve(18);
    REQUIRE(source.feed_packet(make_word0(1, 6, 1, 2) | (15u << 24),
                               make_word1(3, 4, 5, 6), nullptr, nullptr) == Status::start);
    UmpSysex7Reassembler copied(source);
    UmpSysex7Reassembler assigned;
    assigned = source;
    for (auto* instance : {&copied, &assigned}) {
        RtEmitSink sink;
        {
            pulp::test::RtAllocationProbe probe;
            REQUIRE(instance->feed_packet(make_word0(0, 3, 7, 8),
                                           make_word1(9), capture_rt, &sink) == Status::single_packet);
            REQUIRE(instance->feed_packet(make_word0(3, 6, 10, 11) | (15u << 24),
                                           make_word1(12, 13, 14, 15), capture_rt, &sink) == Status::ended);
            REQUIRE_FALSE(probe.saw_allocation());
        }
        REQUIRE(sink.count == 2);
        REQUIRE(sink.sizes[1] == 12);
        REQUIRE(sink.sysex[1][0] == 1);
        REQUIRE(sink.sysex[1][11] == 15);
        REQUIRE(instance->maximum_payload_bytes() == 18);
    }
    REQUIRE(source.in_progress());
}

TEST_CASE("UmpSysex7Reassembler: moves transfer prepared partial state",
          "[midi][ump][sysex7][rt-safety]") {
    UmpSysex7Reassembler source;
    source.reserve(18);
    REQUIRE(source.feed_packet(make_word0(1, 1, 42), 0, nullptr, nullptr) == Status::start);
    UmpSysex7Reassembler moved(std::move(source));
    REQUIRE_FALSE(source.in_progress());
    UmpSysex7Reassembler assigned;
    assigned = std::move(moved);
    REQUIRE_FALSE(moved.in_progress());
    RtEmitSink sink;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(assigned.feed_packet(make_word0(3, 1, 43), 0, capture_rt, &sink) == Status::ended);
        REQUIRE(assigned.feed_packet(make_word0(0, 1, 44) | (15u << 24), 0, capture_rt, &sink) == Status::single_packet);
        REQUIRE_FALSE(probe.saw_allocation());
    }
    REQUIRE(sink.count == 2);
    REQUIRE(sink.sizes[0] == 2);
    REQUIRE(sink.sysex[0][0] == 42);
    REQUIRE(sink.sysex[0][1] == 43);
}
