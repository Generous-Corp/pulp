// MusicalTypingKeyboard — Ink & Signal catalog component. TWO faithful Figma
// mode frames (typing node 187:15 @732×266, piano node 187:349 @732×176)
// rendered via DesignFrameView's multi-frame swap. These pin: both embedded
// SVGs load, the component renders headlessly in each mode, the 🎹/⌨ toggle
// swaps the frame AND the intrinsic size, per-mode keys play, computer-keyboard
// typing works, and it is discoverable in the pulp::design catalog.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_system.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/screenshot.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <vector>

using namespace pulp::view;

namespace {
using K = DesignFrameElement::Kind;
using Mode = MusicalTypingKeyboard::Mode;

// DesignFrameView (a View) is non-copyable/non-movable, so hand back a pointer.
// Bounds == panel size → identity transform (scale 1, no letterbox), so SVG
// coords map straight to view coords for spatial clicks. Re-fit after a mode
// swap (the panel size changes).
std::unique_ptr<MusicalTypingKeyboard> make_playable_kb() {
    auto kb = std::make_unique<MusicalTypingKeyboard>();
    kb->set_bounds({0, 0, kb->panel_width(), kb->panel_height()});
    return kb;
}
void refit(MusicalTypingKeyboard& kb) {
    kb.set_bounds({0, 0, kb.panel_width(), kb.panel_height()});
}
int note_idx(const MusicalTypingKeyboard& kb, int note) {
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::momentary && kb.element_note(i) == note) return i;
    return -1;
}
int momentary_count(const MusicalTypingKeyboard& kb) {
    int n = 0;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::momentary) ++n;
    return n;
}
}  // namespace

TEST_CASE("MusicalTypingKeyboard loads both embedded faithful SVGs", "[view][musical-typing]") {
    MusicalTypingKeyboard kbd;
    REQUIRE(kbd.frame_count() == 2);
    REQUIRE(kbd.mode() == Mode::typing);          // typing is the default frame
    REQUIRE(kbd.panel_width() == 732.0f);
    REQUIRE(kbd.panel_height() == 266.0f);        // typing frame intrinsic
}

TEST_CASE("MusicalTypingKeyboard renders headlessly in both modes", "[view][musical-typing]") {
    MusicalTypingKeyboard kbd;
    refit(kbd);
    auto typing = render_to_png(kbd, 732, 266, 1.0f, ScreenshotBackend::skia);
    if (typing.empty()) SKIP("Skia raster screenshot backend unavailable");  // e.g. Windows CI
    REQUIRE(typing.size() > 1000);
    kbd.set_mode(Mode::piano);
    refit(kbd);
    auto piano = render_to_png(kbd, 732, 176, 1.0f, ScreenshotBackend::skia);
    REQUIRE(piano.size() > 1000);
}

TEST_CASE("MusicalTyping is registered in the pulp::design catalog", "[design][catalog]") {
    const auto* info = pulp::design::find("MusicalTyping");
    REQUIRE(info != nullptr);
    REQUIRE(info->native_class == "pulp::view::MusicalTypingKeyboard");
    REQUIRE(info->category == pulp::design::Category::audio);
}

TEST_CASE("MusicalTypingKeyboard: typing frame has 18 keys + 2 toggle buttons",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    REQUIRE(momentary_count(kb) == 18);            // relative semitones 0..17
    for (int i = 0; i < 18; ++i) REQUIRE(kb.element_note(i) == i);
    // Two swap-link buttons (the 🎹/⌨ toggle).
    int swaps = 0;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::swap) ++swaps;
    REQUIRE(swaps == 2);
}

TEST_CASE("MusicalTypingKeyboard: piano frame has the chromatic span C2..B4",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.set_mode(Mode::piano);
    REQUIRE(kb.mode() == Mode::piano);
    REQUIRE(momentary_count(kb) == 36);
    std::vector<int> piano;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::momentary) piano.push_back(kb.element_note(i));
    std::sort(piano.begin(), piano.end());
    REQUIRE(piano.front() == 48);   // C2
    REQUIRE(piano.back() == 83);    // B4
    for (int n = 48; n <= 83; ++n)
        REQUIRE(std::count(piano.begin(), piano.end(), n) == 1);
}

TEST_CASE("MusicalTypingKeyboard: toggle swaps the frame AND the intrinsic size",
          "[view][musical-typing][toggle]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    REQUIRE(kb.mode() == Mode::typing);
    REQUIRE(kb.panel_height() == 266.0f);

    // The 🎹 (piano-icon) button sits at SVG x[24,60] y[22,48] in BOTH frames;
    // with bounds == panel it maps 1:1. Clicking it swaps to piano mode + resize.
    std::vector<int> notes;
    kb.on_note_on = [&](int n, float) { notes.push_back(n); };
    kb.on_mouse_down({40.0f, 35.0f});
    REQUIRE(kb.mode() == Mode::piano);
    REQUIRE(kb.panel_height() == 176.0f);          // intrinsic size changed
    REQUIRE(kb.panel_width() == 732.0f);
    REQUIRE(notes.empty());                        // a toggle click plays no note

    // The ⌨ (keyboard-icon) button at x[62,98] y[22,48] swaps back to typing.
    refit(kb);
    kb.on_mouse_down({80.0f, 35.0f});
    REQUIRE(kb.mode() == Mode::typing);
    REQUIRE(kb.panel_height() == 266.0f);
}

TEST_CASE("MusicalTypingKeyboard: set_mode swaps programmatically",
          "[view][musical-typing][toggle]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.set_mode(Mode::piano);
    REQUIRE(kb.mode() == Mode::piano);
    REQUIRE(kb.active_frame() == kPianoFrame);
    kb.set_mode(Mode::typing);
    REQUIRE(kb.mode() == Mode::typing);
    REQUIRE(kb.active_frame() == kTypingFrame);
}

TEST_CASE("MusicalTypingKeyboard: typing white-key click plays + lights",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> begins, ends;
    kb.on_gesture_begin = [&](int i) { begins.push_back(kb.element_note(i)); };
    kb.on_gesture_end = [&](int i) { ends.push_back(kb.element_note(i)); };

    // White key 'a' (note 0): rect x[102,157] y[117,195]. Click low-centre (below
    // the black keys, which end at y=171) so only the white key is under it.
    const int a = note_idx(kb, 0);
    kb.on_mouse_down({120.0f, 185.0f});
    REQUIRE(begins == std::vector<int>{0});
    REQUIRE(kb.element_value(a) == 1.0f);
    kb.on_mouse_up({120.0f, 185.0f});
    REQUIRE(ends == std::vector<int>{0});
    REQUIRE(kb.element_value(a) == 0.0f);
}

TEST_CASE("MusicalTypingKeyboard: black key wins the overlap (smallest area)",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> begins;
    kb.on_gesture_begin = [&](int i) { begins.push_back(kb.element_note(i)); };
    // 'w' (note 1) rect x[142,172] y[117,171] overlaps white 'a' (x[102,157]) in
    // the band x[142,157]. A click there must pick the narrower black key.
    kb.on_mouse_down({150.0f, 140.0f});
    REQUIRE(begins == std::vector<int>{1});
    kb.on_mouse_up({150.0f, 140.0f});
}

TEST_CASE("MusicalTypingKeyboard: piano white-key click plays its MIDI note",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.set_mode(Mode::piano);
    refit(kb);
    std::vector<int> begins;
    kb.on_gesture_begin = [&](int i) { begins.push_back(kb.element_note(i)); };
    // Leftmost piano white key C2 (MIDI 48): rect x[28,60] y[70,148]; click
    // low-centre (below the black keys) so only the white key is under it.
    kb.on_mouse_down({44.0f, 140.0f});
    REQUIRE(begins == std::vector<int>{48});
    kb.on_mouse_up({44.0f, 140.0f});
}

TEST_CASE("MusicalTypingKeyboard: set_element_value lights without firing change",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    bool changed = false;
    kb.on_element_changed = [&](int, float) { changed = true; };
    const int f = note_idx(kb, 5);                 // 'f' (note 5)
    kb.set_element_value(f, 1.0f);
    REQUIRE(kb.element_value(f) == 1.0f);
    kb.set_element_value(f, 0.0f);
    REQUIRE(kb.element_value(f) == 0.0f);
    REQUIRE_FALSE(changed);                         // host->view push must not echo
}

// ── Wiring: computer-keyboard play, octave, click→note, focus release ───────

TEST_CASE("MusicalTypingKeyboard: computer keyboard plays + lights typing keys",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons, offs;
    kb.on_note_on  = [&](int n, float) { ons.push_back(n); };
    kb.on_note_off = [&](int n) { offs.push_back(n); };

    KeyEvent a{}; a.key = KeyCode::a; a.is_down = true;      // 'a' = C2 = MIDI 48
    REQUIRE(kb.on_key_event(a));
    REQUIRE(ons == std::vector<int>{48});
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 1.0f);     // note-0 key lit
    a.is_down = false;
    REQUIRE(kb.on_key_event(a));
    REQUIRE(offs == std::vector<int>{48});
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 0.0f);

    // Cmd-chords are NOT consumed (host keeps its shortcuts).
    KeyEvent cmdA{}; cmdA.key = KeyCode::a; cmdA.is_down = true; cmdA.modifiers = kModCmd;
    REQUIRE_FALSE(kb.on_key_event(cmdA));
}

TEST_CASE("MusicalTypingKeyboard: z/x shift the typed octave",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    auto down = [&](KeyCode k) { KeyEvent e{}; e.key = k; e.is_down = true; kb.on_key_event(e); };
    auto tap_a = [&] { KeyEvent e{}; e.key = KeyCode::a; e.is_down = true; kb.on_key_event(e);
                       e.is_down = false; kb.on_key_event(e); };

    down(KeyCode::x);  tap_a();                     // octave +1 → C3 (60)
    REQUIRE(ons.back() == 60);
    down(KeyCode::z);  down(KeyCode::z); tap_a();   // back to -1 → C1 (36)
    REQUIRE(ons.back() == 36);
}

TEST_CASE("MusicalTypingKeyboard: clicking a key emits its MIDI note (both modes)",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    kb.on_mouse_down({120.0f, 185.0f});             // typing 'a' (note 0) → C2 48
    kb.on_mouse_up({120.0f, 185.0f});
    REQUIRE(ons == std::vector<int>{48});
    ons.clear();
    kb.set_mode(Mode::piano);
    refit(kb);
    kb.on_mouse_down({495.0f, 140.0f});             // piano C4 (note 72, absolute)
    kb.on_mouse_up({495.0f, 140.0f});
    REQUIRE(ons == std::vector<int>{72});
}

TEST_CASE("MusicalTypingKeyboard: focus loss releases held notes + clears highlights",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> offs;
    kb.on_note_off = [&](int n) { offs.push_back(n); };
    KeyEvent a{}; a.key = KeyCode::a; a.is_down = true; kb.on_key_event(a);  // hold C2
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 1.0f);
    kb.on_focus_changed(false);
    REQUIRE(offs == std::vector<int>{48});                  // note released
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 0.0f);     // highlight cleared
}

TEST_CASE("MusicalTypingKeyboard: set_input_capture(false) stops QWERTY capture",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    kb.set_input_capture(false);
    REQUIRE_FALSE(kb.input_capture());

    KeyEvent a{}; a.key = KeyCode::a; a.is_down = true;
    REQUIRE_FALSE(kb.on_key_event(a));   // not consumed → host keeps the key
    REQUIRE(ons.empty());                // no double-trigger from our path

    kb.on_mouse_down({120.0f, 185.0f});  // clicks still play
    kb.on_mouse_up({120.0f, 185.0f});
    REQUIRE(ons == std::vector<int>{48});
}

TEST_CASE("MusicalTypingKeyboard: set_active_notes lights from an external held set",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    // Typing mode: C2 (48) held externally → lights the typing 'a' (semitone 0 @
    // base C2). The piano keys live in the other frame, so only 'a' lights here.
    const int held[] = {48, 72};
    kb.set_active_notes(held);
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 1.0f);
    kb.set_active_notes(std::span<const int>{});            // clear
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 0.0f);

    // Piano mode: the same held set lights the piano C2 + C4 keys.
    kb.set_mode(Mode::piano);
    kb.set_active_notes(held);
    REQUIRE(kb.element_value(note_idx(kb, 48)) == 1.0f);
    REQUIRE(kb.element_value(note_idx(kb, 72)) == 1.0f);
    REQUIRE(kb.element_value(note_idx(kb, 60)) == 0.0f);    // not held → dark
}
