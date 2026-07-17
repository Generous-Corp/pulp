#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/ump.hpp>

#include <cstdint>
#include <vector>

using namespace pulp::midi;

// A message header word: message-type nibble in bits 28-31, the rest is
// payload the cursor must skip without interpreting.
static uint32_t header(uint8_t mt, uint32_t payload = 0) {
    return (static_cast<uint32_t>(mt & 0x0F) << 28) | (payload & 0x0FFFFFFFu);
}

// Record of one visited message: message type and its offset from the start of
// the word array, so a test can assert the cursor advanced by spec length.
struct Visited {
    uint8_t mt;
    uint32_t offset;
    uint32_t word_count;
};

static std::vector<Visited> walk(const std::vector<uint32_t>& words) {
    std::vector<Visited> out;
    const uint32_t* base = words.data();
    walk_ump_packet(base, static_cast<uint32_t>(words.size()),
                    [&](uint8_t mt, const uint32_t* mw, uint32_t n) {
                        out.push_back({mt, static_cast<uint32_t>(mw - base), n});
                    });
    return out;
}

TEST_CASE("ump_words_for_message_type covers every spec message type",
          "[midi][ump][cursor]") {
    // 32-bit (1 word)
    for (uint8_t mt : {0x0, 0x1, 0x2, 0x6, 0x7})
        REQUIRE(ump_words_for_message_type(mt) == 1);
    // 64-bit (2 words)
    for (uint8_t mt : {0x3, 0x4, 0x8, 0x9, 0xA})
        REQUIRE(ump_words_for_message_type(mt) == 2);
    // 96-bit (3 words)
    for (uint8_t mt : {0xB, 0xC})
        REQUIRE(ump_words_for_message_type(mt) == 3);
    // 128-bit (4 words) — including the UMP-Stream type 0xF the older
    // per-adapter tables mis-sized as a single word.
    for (uint8_t mt : {0x5, 0xD, 0xE, 0xF})
        REQUIRE(ump_words_for_message_type(mt) == 4);
}

TEST_CASE("size_for_type delegates to the spec word-length table",
          "[midi][ump][cursor]") {
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Utility) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::System) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Midi1ChannelVoice) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::DataSysEx) == 2);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Midi2ChannelVoice) == 2);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Data128) == 4);
}

TEST_CASE("walk_ump_packet advances a 96-bit type-0xB message by three words",
          "[midi][ump][cursor]") {
    // A type-0xB message is 3 words. If the cursor advanced by 1 instead, its
    // two trailing words would be re-read as fresh 32-bit headers — the
    // UMP-cursor bug class. Follow it with a type-0x2 (1-word) message and
    // assert the second message is decoded at offset 3, not offset 1.
    std::vector<uint32_t> words = {
        header(0xB, 0x0AAAAA),  // word 0: 0xB header
        0x11111111,             // word 1: payload (must NOT be read as a header)
        0x22222222,             // word 2: payload
        header(0x2, 0x0090344), // word 3: a real 1-word MIDI 1.0 message
    };

    const auto visited = walk(words);
    REQUIRE(visited.size() == 2);

    REQUIRE(visited[0].mt == 0xB);
    REQUIRE(visited[0].offset == 0);
    REQUIRE(visited[0].word_count == 3);

    REQUIRE(visited[1].mt == 0x2);
    REQUIRE(visited[1].offset == 3);
    REQUIRE(visited[1].word_count == 1);
}

TEST_CASE("walk_ump_packet stops on a truncated final message",
          "[midi][ump][cursor]") {
    // The buffer ends mid-message: a type-0x4 header (declares 2 words) sits in
    // the last word with no room for its second. The cursor must NOT visit it
    // (which would let the visitor over-read past the buffer) and must stop the
    // walk after the preceding complete message.
    std::vector<uint32_t> words = {
        header(0x2, 0x0090344),  // complete 1-word message
        header(0x4, 0x0090344),  // declares 2 words, only 1 remains → truncated
    };

    const auto visited = walk(words);
    REQUIRE(visited.size() == 1);
    REQUIRE(visited[0].mt == 0x2);
    REQUIRE(visited[0].offset == 0);
    REQUIRE(visited[0].word_count == 1);
}

TEST_CASE("walk_ump_packet visits a full type-0x3 sysex7 pair",
          "[midi][ump][cursor]") {
    // Type-0x3 (Data/SysEx7) is 2 words; the visitor reads both. Two back-to-
    // back sysex7 messages must be visited at offsets 0 and 2.
    std::vector<uint32_t> words = {
        header(0x3), 0xDEADBEEF,
        header(0x3), 0xFEEDFACE,
    };

    const auto visited = walk(words);
    REQUIRE(visited.size() == 2);
    REQUIRE(visited[0].mt == 0x3);
    REQUIRE(visited[0].offset == 0);
    REQUIRE(visited[0].word_count == 2);
    REQUIRE(visited[1].mt == 0x3);
    REQUIRE(visited[1].offset == 2);
    REQUIRE(visited[1].word_count == 2);
}

TEST_CASE("walk_ump_packet tolerates an empty or null buffer",
          "[midi][ump][cursor]") {
    int calls = 0;
    walk_ump_packet(nullptr, 0, [&](uint8_t, const uint32_t*, uint32_t) { ++calls; });
    std::vector<uint32_t> empty;
    walk_ump_packet(empty.data(), 0,
                    [&](uint8_t, const uint32_t*, uint32_t) { ++calls; });
    REQUIRE(calls == 0);
}
