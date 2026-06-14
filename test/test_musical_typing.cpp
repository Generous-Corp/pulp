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
