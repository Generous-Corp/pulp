#include <catch2/catch_test_macros.hpp>
#include <pulp/view/musical_typing.hpp>

#include <utility>
#include <vector>

using namespace pulp::view;

namespace {
KeyEvent key(KeyCode k, bool down, bool repeat = false, uint16_t mods = 0) {
    KeyEvent e;
    e.key = k;
    e.is_down = down;
    e.is_repeat = repeat;
    e.modifiers = mods;
    return e;
}
}  // namespace

TEST_CASE("MusicalTyping maps the QWERTY row to chromatic notes", "[view][musical-typing]") {
    MusicalTypingController mt;
    mt.set_base_note(60);  // 'a' = middle C
    std::vector<std::pair<int, bool>> ev;
    mt.on_note_on = [&](int n, float) { ev.emplace_back(n, true); };
    mt.on_note_off = [&](int n) { ev.emplace_back(n, false); };

    REQUIRE(mt.handle_key(key(KeyCode::a, true)));  // C
    REQUIRE(mt.handle_key(key(KeyCode::w, true)));  // C#
    REQUIRE(mt.handle_key(key(KeyCode::s, true)));  // D
    REQUIRE(mt.handle_key(key(KeyCode::e, true)));  // D#
    REQUIRE(mt.handle_key(key(KeyCode::d, true)));  // E
    REQUIRE(ev.size() == 5);
    CHECK(ev[0] == std::make_pair(60, true));
    CHECK(ev[1] == std::make_pair(61, true));
    CHECK(ev[2] == std::make_pair(62, true));
    CHECK(ev[3] == std::make_pair(63, true));
    CHECK(ev[4] == std::make_pair(64, true));
}

TEST_CASE("MusicalTyping covers the full 18-key span incl. ; and ' (top two keys)",
          "[view][musical-typing]") {
    // The Logic-style layout is a..' = 18 semitones (0..17). `;` and `'` are the
    // two keys past `p` — previously unmapped, so they silently did nothing while
    // their on-screen keys still drew + clicked (the slice-unreachable bug).
    CHECK(MusicalTypingController::semitone_for_key(KeyCode::p) == 15);
    CHECK(MusicalTypingController::semitone_for_key(KeyCode::semicolon) == 16);
    CHECK(MusicalTypingController::semitone_for_key(KeyCode::apostrophe) == 17);

    MusicalTypingController mt;
    mt.set_base_note(60);
    std::vector<std::pair<int, bool>> ev;
    mt.on_note_on = [&](int n, float) { ev.emplace_back(n, true); };
    mt.on_note_off = [&](int n) { ev.emplace_back(n, false); };
    REQUIRE(mt.handle_key(key(KeyCode::semicolon, true)));   // 60 + 16
    REQUIRE(mt.handle_key(key(KeyCode::apostrophe, true)));  // 60 + 17
    REQUIRE(ev.size() == 2);
    CHECK(ev[0] == std::make_pair(76, true));
    CHECK(ev[1] == std::make_pair(77, true));
}

TEST_CASE("MusicalTyping de-dups auto-repeat and releases on key-up", "[view][musical-typing]") {
    MusicalTypingController mt;
    mt.set_base_note(48);
    int ons = 0, offs = 0;
    mt.on_note_on = [&](int, float) { ++ons; };
    mt.on_note_off = [&](int) { ++offs; };

    mt.handle_key(key(KeyCode::a, true));               // note on
    mt.handle_key(key(KeyCode::a, true, /*repeat=*/true));  // auto-repeat -> ignored
    mt.handle_key(key(KeyCode::a, true));               // still held -> ignored
    REQUIRE(ons == 1);
    REQUIRE(mt.any_held());
    mt.handle_key(key(KeyCode::a, false));              // key up -> note off
    REQUIRE(offs == 1);
    REQUIRE_FALSE(mt.any_held());
}

TEST_CASE("MusicalTyping rejects modifier chords + unmapped keys (host keeps them)",
          "[view][musical-typing]") {
    MusicalTypingController mt;
    bool fired = false;
    mt.on_note_on = [&](int, float) { fired = true; };

    REQUIRE_FALSE(mt.handle_key(key(KeyCode::a, true, false, kModCmd)));   // Cmd+A -> host
    REQUIRE_FALSE(mt.handle_key(key(KeyCode::s, true, false, kModCtrl)));  // Ctrl+S -> host
    REQUIRE_FALSE(mt.handle_key(key(KeyCode::q, true)));                   // unmapped key
    REQUIRE_FALSE(fired);
}

TEST_CASE("MusicalTyping octave shift releases the note actually played",
          "[view][musical-typing]") {
    MusicalTypingController mt;
    mt.set_base_note(60);
    std::vector<int> ons, offs;
    mt.on_note_on = [&](int n, float) { ons.push_back(n); };
    mt.on_note_off = [&](int n) { offs.push_back(n); };

    REQUIRE(mt.handle_key(key(KeyCode::x, true)));  // octave up
    mt.handle_key(key(KeyCode::a, true));           // C, one octave up = 72
    REQUIRE(ons.back() == 72);

    mt.handle_key(key(KeyCode::z, true));           // octave down WHILE 'a' held
    mt.all_notes_off();                             // must release the 72 it played
    REQUIRE(offs.size() == 1);
    REQUIRE(offs[0] == 72);
}

TEST_CASE("MusicalTyping octave-shift low bound is base-relative (reaches C-2)",
          "[view][musical-typing][octave]") {
    // The play-window low note must be draggable down to C-2 (MIDI 0) regardless
    // of base note. A fixed −4 floor bottoms out at C-1 when base > C2 (e.g. a
    // sampler root of C3) — the bug behind the overview highlight not reaching
    // C-2. The floor is the most-negative shift that lands base+shift*12 on MIDI 0.
    SECTION("base C2 (48) → floor −4") {
        MusicalTypingController mt;
        mt.set_base_note(48);
        mt.set_octave_shift(-99);                       // clamp to the floor
        REQUIRE(mt.octave_shift() == -4);               // 48 − 48 = MIDI 0 = C-2
        REQUIRE(mt.base_note() + mt.octave_shift() * 12 == 0);
    }
    SECTION("base C3 (60) → floor −5 (regression: was hard-clamped to −4)") {
        MusicalTypingController mt;
        mt.set_base_note(60);
        mt.set_octave_shift(-99);
        REQUIRE(mt.octave_shift() == -5);               // 60 − 60 = MIDI 0 = C-2
        REQUIRE(mt.base_note() + mt.octave_shift() * 12 == 0);
        mt.set_octave_shift(-4);                        // a less-negative shift still holds
        REQUIRE(mt.octave_shift() == -4);
    }
    SECTION("top bound stays +5") {
        MusicalTypingController mt;
        mt.set_base_note(60);
        mt.set_octave_shift(99);
        REQUIRE(mt.octave_shift() == 5);
    }
}
