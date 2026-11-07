// Joining of the buffers a driver returns for one long SysEx message.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/sysex_chunk_assembler.hpp>

using pulp::midi::SysexChunkAssembler;
using Bytes = std::vector<std::uint8_t>;

namespace {
bool feed(SysexChunkAssembler& a, const Bytes& b) {
    return a.append(b.data(), b.size());
}
} // namespace

TEST_CASE("SysexChunkAssembler delivers a message that fits one buffer", "[midi][sysex]") {
    SysexChunkAssembler a;
    REQUIRE(feed(a, {0xF0, 0x7E, 0x01, 0xF7}));
    REQUIRE(a.take() == Bytes{0xF0, 0x7E, 0x01, 0xF7});
    REQUIRE_FALSE(a.in_progress());
}

TEST_CASE("SysexChunkAssembler joins a message spread over several buffers", "[midi][sysex]") {
    SysexChunkAssembler a;
    REQUIRE_FALSE(feed(a, {0xF0, 0x01, 0x02}));
    REQUIRE(a.in_progress());
    REQUIRE_FALSE(feed(a, {0x03, 0x04}));
    REQUIRE(feed(a, {0x05, 0xF7}));
    REQUIRE(a.take() == Bytes{0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7});
}

TEST_CASE("SysexChunkAssembler discards an unterminated message when a new one starts",
          "[midi][sysex]") {
    SysexChunkAssembler a;
    REQUIRE_FALSE(feed(a, {0xF0, 0x01, 0x02}));
    REQUIRE(feed(a, {0xF0, 0x09, 0xF7}));
    REQUIRE(a.take() == Bytes{0xF0, 0x09, 0xF7});
}

TEST_CASE("SysexChunkAssembler ignores stray bytes and real-time bytes", "[midi][sysex]") {
    SysexChunkAssembler a;
    REQUIRE_FALSE(feed(a, {0x10, 0x20})); // continuation with no start
    REQUIRE_FALSE(feed(a, {0xF7}));       // lone terminator
    REQUIRE_FALSE(a.in_progress());
    REQUIRE_FALSE(feed(a, {0xF0, 0x01, 0xF8})); // clock inside the message
    REQUIRE(feed(a, {0x02, 0xF7}));
    REQUIRE(a.take() == Bytes{0xF0, 0x01, 0x02, 0xF7});
}

TEST_CASE("SysexChunkAssembler aborts on another status byte", "[midi][sysex]") {
    SysexChunkAssembler a;
    REQUIRE_FALSE(feed(a, {0xF0, 0x01, 0x90, 0x02}));
    REQUIRE_FALSE(a.in_progress());
    REQUIRE_FALSE(feed(a, {0x03, 0xF7})); // the old message is gone
}

TEST_CASE("SysexChunkAssembler drops an oversized message whole", "[midi][sysex]") {
    SysexChunkAssembler a;
    Bytes chunk(64 * 1024, 0x11);
    chunk.front() = 0xF0;
    REQUIRE_FALSE(feed(a, chunk));
    chunk.assign(64 * 1024, 0x11);
    for (std::size_t i = 0; i < SysexChunkAssembler::kMaxMessageBytes / chunk.size() + 1; ++i) {
        REQUIRE_FALSE(feed(a, chunk));
    }
    // The terminator ends the dropped message without delivering it.
    REQUIRE_FALSE(feed(a, {0xF7}));
    REQUIRE_FALSE(a.in_progress());
    REQUIRE(feed(a, {0xF0, 0x05, 0xF7}));
    REQUIRE(a.take() == Bytes{0xF0, 0x05, 0xF7});
}

TEST_CASE("SysexChunkAssembler reset drops partial state", "[midi][sysex]") {
    SysexChunkAssembler a;
    feed(a, {0xF0, 0x01});
    a.reset();
    REQUIRE_FALSE(a.in_progress());
    REQUIRE_FALSE(feed(a, {0x02, 0xF7}));
}
