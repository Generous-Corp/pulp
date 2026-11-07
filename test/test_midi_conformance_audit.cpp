// Conformance checks found while auditing MIDI input and output handling:
// timestamp wrap in the BLE-MIDI codec, system common messages and running
// status in both byte-stream parsers, and channel mode messages in the
// keyboard state tracker.

#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/ble_midi.hpp>
#include <pulp/midi/keyboard_state.hpp>
#include <pulp/midi/raw_midi_parser.hpp>
#include <pulp/midi/running_status.hpp>

#include <cstdint>
#include <vector>

using namespace pulp::midi;

namespace {

struct Short {
    uint8_t status, d1, d2;
    bool operator==(const Short& o) const {
        return status == o.status && d1 == o.d1 && d2 == o.d2;
    }
};

std::vector<Short> run_raw(const std::vector<uint8_t>& bytes) {
    std::vector<Short> out;
    RawMidiParserState state;
    parse_raw_midi_bytes(
        bytes.data(), bytes.size(), state,
        [&](uint8_t s, uint8_t a, uint8_t b) { out.push_back({s, a, b}); }, nullptr);
    return out;
}

std::vector<Short> run_running(const std::vector<uint8_t>& bytes) {
    std::vector<Short> out;
    RunningStatusParser parser;
    parser.on_short_message(
        [&](const MidiEvent& e) { out.push_back({e.data()[0], e.data()[1], e.data()[2]}); });
    parser.feed(bytes.data(), bytes.size());
    return out;
}

} // namespace

// ── 32-bit timestamps in BLE-MIDI output ────────────────────────────────

TEST_CASE("BLE-MIDI encoder keeps only 13 bits of a 32-bit timestamp", "[midi][audit][ble-midi]") {
    const uint8_t msg[] = {0x90, 60, 100};
    // 0xFFFFFFFF is 8191 ms into the 8192 ms cycle.
    auto p = encode_ble_midi_packet(msg, sizeof(msg), 0xFFFFFFFFu);
    REQUIRE(p[0] == (0x80 | 0x3F));
    REQUIRE(p[1] == (0x80 | 0x7F));

    // One millisecond later the 32-bit counter wraps to 0 and the 13-bit
    // field wraps with it: the sequence stays continuous.
    auto q = encode_ble_midi_packet(msg, sizeof(msg), 0u);
    REQUIRE(q[0] == 0x80);
    REQUIRE(q[1] == 0x80);

    // Any value maps to its remainder over 8192, including ones above 2^31.
    auto r = encode_ble_midi_packet(msg, sizeof(msg), 0x80002001u);
    BleMidiPacketDecoder dec;
    uint32_t seen = 0xFFFFFFFFu;
    dec.set_message_callback([&](const std::vector<uint8_t>&, uint32_t ts) { seen = ts; });
    REQUIRE(dec.decode(r.data(), r.size()));
    REQUIRE(seen == 0x2001u % 8192u);
}

// ── F1-F7 cancel running status ─────────────────────────────────────────

TEST_CASE("System common messages cancel running status in the raw parser",
          "[midi][audit][parser]") {
    const std::vector<uint8_t> after_common = {0x3C, 0x40}; // data with no status
    struct Case {
        std::vector<uint8_t> common;
        Short expected;
    };
    const std::vector<Case> cases = {
        {{0xF1, 0x12}, {0xF1, 0x12, 0}},
        {{0xF2, 0x01, 0x02}, {0xF2, 0x01, 0x02}},
        {{0xF3, 0x05}, {0xF3, 0x05, 0}},
        {{0xF6}, {0xF6, 0, 0}},
    };
    for (const auto& c : cases) {
        std::vector<uint8_t> bytes = {0x90, 0x3C, 0x64};
        bytes.insert(bytes.end(), c.common.begin(), c.common.end());
        bytes.insert(bytes.end(), after_common.begin(), after_common.end());
        const auto got = run_raw(bytes);
        INFO("system common 0x" << std::hex << int(c.common[0]));
        REQUIRE(got.size() == 2); // the note, then the system common; nothing from stray data
        REQUIRE(got[1] == c.expected);
    }
}

TEST_CASE("Undefined system common F4/F5 and stray EOX cancel running status in the raw parser",
          "[midi][audit][parser]") {
    for (uint8_t status : {uint8_t{0xF4}, uint8_t{0xF5}, uint8_t{0xF7}}) {
        const auto got = run_raw({0x90, 0x3C, 0x64, status, 0x3C, 0x40});
        INFO("status 0x" << std::hex << int(status));
        // Exactly one note-on, no second one rebuilt from the stray data.
        int notes = 0;
        for (const auto& m : got)
            if (m.status == 0x90)
                ++notes;
        REQUIRE(notes == 1);
    }
}

TEST_CASE("System common messages cancel running status in the running-status parser",
          "[midi][audit][parser]") {
    struct Case {
        std::vector<uint8_t> common;
        Short expected;
    };
    const std::vector<Case> cases = {
        {{0xF1, 0x12}, {0xF1, 0x12, 0}},
        {{0xF2, 0x01, 0x02}, {0xF2, 0x01, 0x02}},
        {{0xF3, 0x05}, {0xF3, 0x05, 0}},
        {{0xF6}, {0xF6, 0, 0}},
    };
    for (const auto& c : cases) {
        std::vector<uint8_t> bytes = {0x90, 0x3C, 0x64};
        bytes.insert(bytes.end(), c.common.begin(), c.common.end());
        bytes.push_back(0x3C); // stray data
        bytes.push_back(0x40);
        const auto got = run_running(bytes);
        INFO("system common 0x" << std::hex << int(c.common[0]));
        REQUIRE(got.size() == 2);
        REQUIRE(got[0] == Short{0x90, 0x3C, 0x64});
        REQUIRE(got[1] == c.expected);
    }
}

TEST_CASE("Undefined system common F4/F5 and stray EOX cancel running status in the running-status "
          "parser",
          "[midi][audit][parser]") {
    for (uint8_t status : {uint8_t{0xF4}, uint8_t{0xF5}, uint8_t{0xF7}}) {
        const auto got = run_running({0x90, 0x3C, 0x64, status, 0x3C, 0x40});
        INFO("status 0x" << std::hex << int(status));
        int notes = 0;
        for (const auto& m : got)
            if (m.status == 0x90)
                ++notes;
        REQUIRE(notes == 1);
        for (const auto& m : got)
            REQUIRE(m.status != 0x00);
    }
}

TEST_CASE("A new channel message after system common starts fresh running status",
          "[midi][audit][parser]") {
    const std::vector<uint8_t> bytes = {0xF3, 0x05, 0x80, 0x3C, 0x40, 0x3D, 0x41};
    const std::vector<Short> want = {{0xF3, 0x05, 0}, {0x80, 0x3C, 0x40}, {0x80, 0x3D, 0x41}};
    REQUIRE(run_raw(bytes) == want);
    REQUIRE(run_running(bytes) == want);
}

// ── CC 120 / CC 123 release held keyboard-state notes ───────────────────

TEST_CASE("All Notes Off releases held notes on its channel only", "[midi][audit][keyboard]") {
    MidiKeyboardState keys;
    std::vector<std::pair<int, int>> released;
    keys.on_note_off = [&](uint8_t ch, uint8_t note) { released.push_back({ch, note}); };

    keys.process(MidiEvent::note_on(2, 60, 100));
    keys.process(MidiEvent::note_on(2, 64, 100));
    keys.process(MidiEvent::note_on(5, 67, 100));

    keys.process(MidiEvent::cc(2, 123, 0));

    REQUIRE_FALSE(keys.is_note_on(2, 60));
    REQUIRE_FALSE(keys.is_note_on(2, 64));
    REQUIRE(keys.is_note_on(5, 67));
    REQUIRE(released.size() == 2);
}

TEST_CASE("All Sound Off releases held notes", "[midi][audit][keyboard]") {
    MidiKeyboardState keys;
    int released = 0;
    keys.on_note_off = [&](uint8_t, uint8_t) { ++released; };
    keys.process(MidiEvent::note_on(0, 48, 90));
    keys.process(MidiEvent::note_on(0, 55, 90));

    keys.process(MidiEvent::cc(0, 120, 0));

    REQUIRE_FALSE(keys.any_notes_held());
    REQUIRE(released == 2);
}

TEST_CASE("Other controllers do not disturb held notes", "[midi][audit][keyboard]") {
    MidiKeyboardState keys;
    keys.process(MidiEvent::note_on(0, 60, 100));
    for (uint8_t cc :
         {uint8_t{1}, uint8_t{7}, uint8_t{64}, uint8_t{119}, uint8_t{121}, uint8_t{122}}) {
        keys.process(MidiEvent::cc(0, cc, 127));
    }
    REQUIRE(keys.is_note_on(0, 60));
    // Notes can be played again after a panic.
    keys.process(MidiEvent::cc(0, 123, 0));
    keys.process(MidiEvent::note_on(0, 62, 100));
    REQUIRE(keys.is_note_on(0, 62));
}
