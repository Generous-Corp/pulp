#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/rpn_parser.hpp>
#include <pulp/midi/keyboard_state.hpp>
#include <vector>

using namespace pulp::midi;

// ── RpnParser ────────────────────────────────────────────────────────────

TEST_CASE("RpnParser detects RPN sequence", "[midi][rpn]") {
    RpnParser rpn;
    uint16_t received_param = 0;
    uint16_t received_value = 0;

    rpn.on_rpn = [&](uint8_t, uint16_t param, uint16_t value) {
        received_param = param;
        received_value = value;
    };

    // RPN: CC 101=0 (MSB), CC 100=0 (LSB) → param 0 (pitch bend range)
    rpn.process(MidiEvent::cc(0, 101, 0));   // param MSB
    rpn.process(MidiEvent::cc(0, 100, 0));   // param LSB
    rpn.process(MidiEvent::cc(0, 6, 2));     // data MSB (2 semitones)
    rpn.process(MidiEvent::cc(0, 38, 0));    // data LSB

    REQUIRE(received_param == 0);
    REQUIRE(received_value == (2 << 7)); // MSB=2, LSB=0
}

TEST_CASE("RpnParser detects NRPN sequence", "[midi][rpn]") {
    RpnParser rpn;
    uint16_t received_param = 0;
    uint16_t received_value = 0;

    rpn.on_nrpn = [&](uint8_t, uint16_t param, uint16_t value) {
        received_param = param;
        received_value = value;
    };

    // NRPN: CC 99=1 (MSB), CC 98=5 (LSB) → param 133
    rpn.process(MidiEvent::cc(0, 99, 1));    // NRPN param MSB
    rpn.process(MidiEvent::cc(0, 98, 5));    // NRPN param LSB
    rpn.process(MidiEvent::cc(0, 6, 64));    // data MSB
    rpn.process(MidiEvent::cc(0, 38, 0));    // data LSB

    REQUIRE(received_param == ((1 << 7) | 5));
    REQUIRE(received_value == (64 << 7));
}

TEST_CASE("RpnParser increment callback", "[midi][rpn]") {
    RpnParser rpn;
    bool incremented = false;

    rpn.on_increment = [&](uint8_t, uint16_t, bool) { incremented = true; };

    rpn.process(MidiEvent::cc(0, 101, 0));
    rpn.process(MidiEvent::cc(0, 100, 0));
    rpn.process(MidiEvent::cc(0, 96, 0));   // increment

    REQUIRE(incremented);
}

TEST_CASE("RpnParser decrement callback", "[midi][rpn]") {
    RpnParser rpn;
    bool decremented = false;

    rpn.on_decrement = [&](uint8_t, uint16_t, bool) { decremented = true; };

    rpn.process(MidiEvent::cc(0, 101, 0));
    rpn.process(MidiEvent::cc(0, 100, 0));
    rpn.process(MidiEvent::cc(0, 97, 0));   // decrement

    REQUIRE(decremented);
}

TEST_CASE("RpnParser ignores non-CC events", "[midi][rpn]") {
    RpnParser rpn;
    bool called = false;
    rpn.on_rpn = [&](uint8_t, uint16_t, uint16_t) { called = true; };

    rpn.process(MidiEvent::note_on(0, 60, 100));
    REQUIRE_FALSE(called);
}

TEST_CASE("RpnParser reset clears state", "[midi][rpn]") {
    RpnParser rpn;
    rpn.process(MidiEvent::cc(0, 101, 0));
    rpn.process(MidiEvent::cc(0, 100, 0));

    rpn.reset();

    uint16_t received = 9999;
    rpn.on_rpn = [&](uint8_t, uint16_t, uint16_t v) { received = v; };
    rpn.process(MidiEvent::cc(0, 6, 1));
    rpn.process(MidiEvent::cc(0, 38, 0));

    // Should NOT fire because param_set was cleared by reset
    REQUIRE(received == 9999);
}

TEST_CASE("RpnParser per-channel isolation", "[midi][rpn]") {
    RpnParser rpn;
    uint8_t received_ch = 255;

    rpn.on_rpn = [&](uint8_t ch, uint16_t, uint16_t) { received_ch = ch; };

    rpn.process(MidiEvent::cc(3, 101, 0));
    rpn.process(MidiEvent::cc(3, 100, 0));
    rpn.process(MidiEvent::cc(3, 6, 1));
    rpn.process(MidiEvent::cc(3, 38, 0));

    REQUIRE(received_ch == 3);
}

TEST_CASE("RpnParser ignores incomplete selections and unrelated CCs",
          "[midi][rpn][issue-645]") {
    RpnParser rpn;
    int calls = 0;
    rpn.on_rpn = [&](uint8_t, uint16_t, uint16_t) { ++calls; };

    rpn.process(MidiEvent::cc(0, 6, 2));
    rpn.process(MidiEvent::cc(0, 38, 0));
    rpn.process(MidiEvent::cc(0, 101, 1));
    rpn.process(MidiEvent::cc(0, 6, 3));
    rpn.process(MidiEvent::cc(0, 38, 4));
    rpn.process(MidiEvent::cc(0, 7, 100));
    rpn.process(MidiEvent::cc(0, 96, 0));
    rpn.process(MidiEvent::cc(0, 97, 0));

    REQUIRE(calls == 0);
}

TEST_CASE("RpnParser tolerates omitted callbacks",
          "[midi][rpn][issue-645]") {
    RpnParser rpn;

    REQUIRE_NOTHROW([&] {
        rpn.process(MidiEvent::cc(0, 101, 0));
        rpn.process(MidiEvent::cc(0, 100, 1));
        rpn.process(MidiEvent::cc(0, 6, 2));
        rpn.process(MidiEvent::cc(0, 38, 3));
        rpn.process(MidiEvent::cc(0, 96, 0));
        rpn.process(MidiEvent::cc(0, 97, 0));

        rpn.process(MidiEvent::cc(0, 99, 4));
        rpn.process(MidiEvent::cc(0, 98, 5));
        rpn.process(MidiEvent::cc(0, 6, 6));
        rpn.process(MidiEvent::cc(0, 38, 7));
    }());
}

TEST_CASE("RpnParser reports NRPN increment and decrement metadata",
          "[midi][rpn][issue-645]") {
    RpnParser rpn;
    std::vector<uint16_t> params;
    std::vector<bool> is_rpn_flags;

    rpn.on_increment = [&](uint8_t ch, uint16_t param, bool is_rpn) {
        REQUIRE(ch == 2);
        params.push_back(param);
        is_rpn_flags.push_back(is_rpn);
    };
    rpn.on_decrement = [&](uint8_t ch, uint16_t param, bool is_rpn) {
        REQUIRE(ch == 2);
        params.push_back(param);
        is_rpn_flags.push_back(is_rpn);
    };

    rpn.process(MidiEvent::cc(2, 99, 3));
    rpn.process(MidiEvent::cc(2, 98, 9));
    rpn.process(MidiEvent::cc(2, 96, 0));
    rpn.process(MidiEvent::cc(2, 97, 0));

    REQUIRE(params == std::vector<uint16_t>{static_cast<uint16_t>((3 << 7) | 9),
                                            static_cast<uint16_t>((3 << 7) | 9)});
    REQUIRE(is_rpn_flags == std::vector<bool>{false, false});
}

TEST_CASE("RpnParser reports maximum 14-bit RPN parameter and value",
          "[midi][rpn][coverage]") {
    RpnParser rpn;
    uint8_t received_ch = 255;
    uint16_t received_param = 0;
    uint16_t received_value = 0;

    rpn.on_rpn = [&](uint8_t ch, uint16_t param, uint16_t value) {
        received_ch = ch;
        received_param = param;
        received_value = value;
    };

    rpn.process(MidiEvent::cc(15, 101, 127));
    rpn.process(MidiEvent::cc(15, 100, 127));
    rpn.process(MidiEvent::cc(15, 6, 127));
    rpn.process(MidiEvent::cc(15, 38, 127));

    REQUIRE(received_ch == 15);
    REQUIRE(received_param == 0x3fff);
    REQUIRE(received_value == 0x3fff);
}

TEST_CASE("RpnParser suppresses increment while parameter is being reselected",
          "[midi][rpn][coverage]") {
    RpnParser rpn;
    std::vector<uint16_t> params;

    rpn.on_increment = [&](uint8_t ch, uint16_t param, bool is_rpn) {
        REQUIRE(ch == 4);
        REQUIRE(is_rpn);
        params.push_back(param);
    };

    rpn.process(MidiEvent::cc(4, 101, 1));
    rpn.process(MidiEvent::cc(4, 100, 2));
    rpn.process(MidiEvent::cc(4, 96, 0));

    rpn.process(MidiEvent::cc(4, 101, 3));
    rpn.process(MidiEvent::cc(4, 96, 0));
    rpn.process(MidiEvent::cc(4, 100, 5));
    rpn.process(MidiEvent::cc(4, 96, 0));

    REQUIRE(params == std::vector<uint16_t>{static_cast<uint16_t>((1 << 7) | 2),
                                            static_cast<uint16_t>((3 << 7) | 5)});
}

// ── MidiKeyboardState ────────────────────────────────────────────────────

TEST_CASE("MidiKeyboardState tracks note on/off", "[midi][keyboard]") {
    MidiKeyboardState keys;

    keys.process(MidiEvent::note_on(0, 60, 100));
    REQUIRE(keys.is_note_on(0, 60));
    REQUIRE(keys.velocity(0, 60) == 100);

    keys.process(MidiEvent::note_off(0, 60));
    REQUIRE_FALSE(keys.is_note_on(0, 60));
}

TEST_CASE("MidiKeyboardState note on vel=0 is note off", "[midi][keyboard]") {
    MidiKeyboardState keys;
    keys.process(MidiEvent::note_on(0, 64, 80));
    REQUIRE(keys.is_note_on(0, 64));

    keys.process(MidiEvent::note_on(0, 64, 0)); // vel=0 = note off
    REQUIRE_FALSE(keys.is_note_on(0, 64));
}

TEST_CASE("MidiKeyboardState notes_held count", "[midi][keyboard]") {
    MidiKeyboardState keys;
    REQUIRE(keys.notes_held(0) == 0);

    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_on(0, 64, 100));
    keys.process(MidiEvent::note_on(0, 67, 100));
    REQUIRE(keys.notes_held(0) == 3);
    REQUIRE(keys.total_notes_held() == 3);
}

TEST_CASE("MidiKeyboardState lowest/highest note", "[midi][keyboard]") {
    MidiKeyboardState keys;
    REQUIRE(keys.lowest_note(0) == -1);

    keys.process(MidiEvent::note_on(0, 48, 80));
    keys.process(MidiEvent::note_on(0, 72, 80));
    keys.process(MidiEvent::note_on(0, 60, 80));

    REQUIRE(keys.lowest_note(0) == 48);
    REQUIRE(keys.highest_note(0) == 72);
}

TEST_CASE("MidiKeyboardState channel isolation", "[midi][keyboard]") {
    MidiKeyboardState keys;
    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_on(1, 64, 100));

    REQUIRE(keys.is_note_on(0, 60));
    REQUIRE_FALSE(keys.is_note_on(1, 60));
    REQUIRE(keys.is_note_on(1, 64));
}

TEST_CASE("MidiKeyboardState on_note_on callback", "[midi][keyboard]") {
    MidiKeyboardState keys;
    uint8_t cb_note = 0;
    uint8_t cb_vel = 0;
    keys.on_note_on = [&](uint8_t, uint8_t n, uint8_t v) { cb_note = n; cb_vel = v; };

    keys.process(MidiEvent::note_on(0, 72, 110));
    REQUIRE(cb_note == 72);
    REQUIRE(cb_vel == 110);
}

TEST_CASE("MidiKeyboardState on_note_off callback", "[midi][keyboard]") {
    MidiKeyboardState keys;
    uint8_t cb_note = 0;
    keys.on_note_off = [&](uint8_t, uint8_t n) { cb_note = n; };

    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_off(0, 60));
    REQUIRE(cb_note == 60);
}

TEST_CASE("MidiKeyboardState all_notes_off clears all", "[midi][keyboard]") {
    MidiKeyboardState keys;
    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_on(0, 64, 100));
    keys.process(MidiEvent::note_on(1, 48, 80));

    keys.all_notes_off();
    REQUIRE(keys.total_notes_held() == 0);
    REQUIRE_FALSE(keys.any_notes_held());
}

TEST_CASE("MidiKeyboardState all_notes_off per channel", "[midi][keyboard]") {
    MidiKeyboardState keys;
    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::note_on(1, 64, 100));

    keys.all_notes_off(0);
    REQUIRE(keys.notes_held(0) == 0);
    REQUIRE(keys.notes_held(1) == 1); // channel 1 unaffected
}

TEST_CASE("MidiKeyboardState reset without callbacks", "[midi][keyboard]") {
    MidiKeyboardState keys;
    int off_count = 0;
    keys.on_note_off = [&](uint8_t, uint8_t) { ++off_count; };

    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.reset(); // should NOT fire callbacks
    REQUIRE(off_count == 0);
    REQUIRE_FALSE(keys.any_notes_held());
}

TEST_CASE("MidiKeyboardState ignores non-note events and invalid queries", "[midi][keyboard][issue-645]") {
    MidiKeyboardState keys;
    int on_count = 0;
    int off_count = 0;
    keys.on_note_on = [&](uint8_t, uint8_t, uint8_t) { ++on_count; };
    keys.on_note_off = [&](uint8_t, uint8_t) { ++off_count; };

    keys.process(MidiEvent::cc(0, 64, 127));
    keys.process(MidiEvent::pitch_bend(1, 8192));
    keys.process(MidiEvent::program_change(2, 10));

    REQUIRE_FALSE(keys.any_notes_held());
    REQUIRE(on_count == 0);
    REQUIRE(off_count == 0);
    REQUIRE_FALSE(keys.is_note_on(16, 60));
    REQUIRE(keys.velocity(16, 60) == 0);
    REQUIRE(keys.velocity(0, 128) == 0);
    REQUIRE(keys.notes_held(16) == 0);
    REQUIRE(keys.lowest_note(16) == -1);
    REQUIRE(keys.highest_note(16) == -1);

    keys.all_notes_off(16);
    REQUIRE(off_count == 0);
}

TEST_CASE("MidiKeyboardState reports released keys when clearing", "[midi][keyboard][issue-645]") {
    MidiKeyboardState keys;
    std::vector<int> released;
    keys.on_note_off = [&](uint8_t channel, uint8_t note) {
        released.push_back(static_cast<int>(channel) * 128 + static_cast<int>(note));
    };

    keys.process(MidiEvent::note_on(1, 60, 100));
    keys.process(MidiEvent::note_on(1, 62, 90));
    keys.process(MidiEvent::note_on(2, 65, 80));

    keys.all_notes_off(1);
    REQUIRE(keys.notes_held(1) == 0);
    REQUIRE(keys.notes_held(2) == 1);
    REQUIRE(released == std::vector<int>{188, 190});

    keys.all_notes_off();
    REQUIRE_FALSE(keys.any_notes_held());
    REQUIRE(released == std::vector<int>{188, 190, 321});
}

TEST_CASE("MidiKeyboardState retrigger updates velocity without duplicating held count", "[midi][keyboard][issue-645]") {
    MidiKeyboardState keys;
    int on_count = 0;
    keys.on_note_on = [&](uint8_t, uint8_t, uint8_t) { ++on_count; };

    keys.process(MidiEvent::note_on(0, 60, 40));
    keys.process(MidiEvent::note_on(0, 60, 120));

    REQUIRE(keys.notes_held(0) == 1);
    REQUIRE(keys.total_notes_held() == 1);
    REQUIRE(keys.velocity(0, 60) == 120);
    REQUIRE(on_count == 2);
}
