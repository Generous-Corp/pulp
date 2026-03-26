#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/rpn_parser.hpp>
#include <pulp/midi/keyboard_state.hpp>

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
