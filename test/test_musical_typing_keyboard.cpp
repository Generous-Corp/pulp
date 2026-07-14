// MusicalTypingKeyboard — Ink & Signal catalog component. TWO faithful Figma
// mode frames (typing node 187:15 @732×266, piano node 187:349 @732×176)
// rendered via DesignFrameView's multi-frame swap. These pin: both embedded
// SVGs load, the component renders headlessly in each mode, the 🎹/⌨ toggle
// swaps the frame AND the intrinsic size, per-mode keys play, computer-keyboard
// typing works, and it is discoverable in the pulp::design catalog.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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
// Note-bearing momentary keys only (note >= 0) — excludes the tagged control
// momentary (sustain / pitch-bend / modulation, which carry note == -1).
int note_key_count(const MusicalTypingKeyboard& kb) {
    int n = 0;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::momentary && kb.element_note(i) >= 0) ++n;
    return n;
}
// Index of the first element with the given action/value tag, or -1.
int tag_idx(const MusicalTypingKeyboard& kb, const std::string& tag) {
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_action(i) == tag) return i;
    return -1;
}
// Press + release a number-row / tab key (momentary control).
void tap_key(MusicalTypingKeyboard& kb, KeyCode k) {
    KeyEvent e{}; e.key = k; e.is_down = true;  kb.on_key_event(e);
                  e.is_down = false;            kb.on_key_event(e);
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
    REQUIRE(note_key_count(kb) == 18);             // relative semitones 0..17
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
    REQUIRE(note_key_count(kb) == 36);              // 36 piano keys
    REQUIRE(momentary_count(kb) == 36 + 2);         // + the < > octave arrows (tap-flash momentary)
    std::vector<int> piano;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::momentary && kb.element_note(i) >= 0)
            piano.push_back(kb.element_note(i));   // note keys only (skip arrow momentary)
    std::sort(piano.begin(), piano.end());
    REQUIRE(piano.front() == 48);   // C2
    REQUIRE(piano.back() == 83);    // B4
    for (int n = 48; n <= 83; ++n)
        REQUIRE(std::count(piano.begin(), piano.end(), n) == 1);
}

// Regression: the piano playable window must stay a contiguous 36-key window over
// C-2..G8 regardless of the controller base note. midi_for_element used to offset
// by (note - base_note), so a sampler root of C3 (60) shifted the whole window
// down 12 semitones — the top reached only ~D#7 instead of G8, and the lowest
// visible key was C-1 not C-2. It must map by the frame-intrinsic low note (48).
TEST_CASE("MusicalTypingKeyboard: piano window is base-independent (C-2..G8)",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.controller().set_base_note(60);   // C3 root, like the tempo sampler
    kb.set_mode(Mode::piano);
    auto window = [&] {
        int lo = 999, hi = -1;
        for (int i = 0; i < kb.element_count(); ++i)
            if (kb.element_kind(i) == K::momentary && kb.element_note(i) >= 0) {
                const int m = kb.midi_for_element(i);
                lo = std::min(lo, m); hi = std::max(hi, m);
            }
        return std::pair<int, int>{lo, hi};
    };
    // Far LEFT: window bottoms on C-2 (MIDI 0); still 36 contiguous keys.
    kb.controller().set_octave_shift(-99);
    auto [lo1, hi1] = window();
    REQUIRE(lo1 == 0);                 // C-2
    REQUIRE(hi1 - lo1 == 35);          // 36-key contiguous window
    // Far RIGHT: window tops out on G8 (MIDI 127).
    kb.controller().set_octave_shift(99);
    auto [lo2, hi2] = window();
    REQUIRE(hi2 == 127);               // G8
    REQUIRE(hi2 - lo2 == 35);
}

// MusicalTypingKeyboard::set_base_note is the wrapper the sampler calls when the
// ROOT note changes: unlike a bare controller().set_base_note(), it re-clamps the
// octave shift to the NEW base's range (and refreshes the readout/overview), so a
// shift that was valid for the old base can't push the play-window off C-2..G8.
TEST_CASE("MusicalTypingKeyboard: set_base_note re-clamps the octave shift",
          "[view][musical-typing]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.set_base_note(60);                          // C3 root
    kb.controller().set_octave_shift(-99);         // pin to base-60's floor
    const int floor60 = kb.controller().octave_shift();
    REQUIRE(floor60 < 0);                           // base 60 can shift below 0 (reaches C-2)

    kb.set_base_note(0);                            // re-base via the wrapper
    // Base 0's floor is 0 (no negative shift), so the stale -ve shift must be
    // re-clamped up. A bare controller().set_base_note would have left floor60.
    REQUIRE(kb.controller().octave_shift() >= 0);
    REQUIRE(kb.controller().octave_shift() != floor60);
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

    // White key 'a' (note 0): rect x[102,157] y[117,195]. Click low-center (below
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
    // Leftmost piano white key C2 (MIDI 48): rect x[28,60] y[62,140] (−8 post-#82);
    // click low-center (below the black keys) so only the white key is under it.
    kb.on_mouse_down({44.0f, 130.0f});
    REQUIRE(begins == std::vector<int>{48});
    kb.on_mouse_up({44.0f, 130.0f});
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

// Regression: input_capture gates whether the keyboard plays its OWN typed keys.
// The tempo sampler's popout keyboard window had it set to false (display-only),
// so when that window was focused EVERY typed key was dropped (the dead-keyboard
// bug). With it true the keyboard self-captures: letters play notes and numbers
// 1–8 route to pitch-bend / modulation controls.
TEST_CASE("MusicalTypingKeyboard: input_capture gates self-play of typed keys",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    std::vector<float> bends;
    kb.on_note_on    = [&](int n, float) { ons.push_back(n); };
    kb.on_pitch_bend = [&](float b) { bends.push_back(b); };

    auto press = [](KeyCode k) { KeyEvent e{}; e.key = k; e.is_down = true; return e; };

    // Display-only: on_key_event is a no-op and plays nothing.
    kb.set_input_capture(false);
    REQUIRE_FALSE(kb.on_key_event(press(KeyCode::a)));
    REQUIRE_FALSE(kb.on_key_event(press(KeyCode::num2)));
    REQUIRE(ons.empty());
    REQUIRE(bends.empty());

    // Self-capture: a letter plays its note; number 2 drives pitch-bend up.
    kb.set_input_capture(true);
    REQUIRE(kb.on_key_event(press(KeyCode::a)));
    REQUIRE(ons == std::vector<int>{48});          // 'a' = C2
    REQUIRE(kb.on_key_event(press(KeyCode::num2)));
    REQUIRE_FALSE(bends.empty());                  // pitch-bend control fired
}

// Regression: dropping self-capture must RELEASE any note still held, firing
// on_note_off. The tempo sampler calls set_input_capture(false) when the keyboard
// window is dismissed; without the release a key held at close time never got its
// key-up (a hidden window receives no keys) and its voice sustained forever — the
// "QWERTY keeps sending MIDI after the keyboard is closed" bug.
TEST_CASE("MusicalTypingKeyboard: dropping capture releases held notes (close path)",
          "[view][musical-typing][wiring]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons, offs;
    kb.on_note_on  = [&](int n, float) { ons.push_back(n); };
    kb.on_note_off = [&](int n) { offs.push_back(n); };

    KeyEvent a{}; a.key = KeyCode::a; a.is_down = true;
    REQUIRE(kb.on_key_event(a));                    // 'a' held, no key-up sent
    REQUIRE(ons == std::vector<int>{48});
    REQUIRE(offs.empty());

    kb.set_input_capture(false);                    // dismiss → must release the held note
    REQUIRE(offs == std::vector<int>{48});
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
    kb.on_mouse_down({495.0f, 130.0f});             // piano C4 (note 72, absolute; y −8)
    kb.on_mouse_up({495.0f, 130.0f});
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

TEST_CASE("MusicalTypingKeyboard: the piano window shifts its range with the octave",
          "[view][musical-typing][piano][overview]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.set_mode(Mode::piano); refit(kb);
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    // Leftmost piano white key (rect x[28,60] y[62,140]); low-center click.
    auto click_left = [&] { kb.on_mouse_down({44.0f, 130.0f}); kb.on_mouse_up({44.0f, 130.0f}); };
    auto shift = [&](KeyCode k, int n) { for (int i = 0; i < n; ++i) {
        KeyEvent e{}; e.key = k; e.is_down = true; kb.on_key_event(e); } };

    click_left();
    REQUIRE(ons.back() == 48);            // octave 0 → window starts at C2 (48)
    shift(KeyCode::z, 4); click_left();
    REQUIRE(ons.back() == 0);             // −4 → window starts at C-2 (0)
    shift(KeyCode::x, 8); click_left();   // back to +4 (top)
    REQUIRE(ons.back() == 92);            // clamped: window ends on G8 (127), lo = 92
}

// ── Review-hardening: frame swap must not strand notes or lit state ─────────

TEST_CASE("MusicalTypingKeyboard: toggling mode releases a QWERTY-held note",
          "[view][musical-typing][toggle][regression]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons, offs;
    kb.on_note_on  = [&](int n, float) { ons.push_back(n); };
    kb.on_note_off = [&](int n) { offs.push_back(n); };

    KeyEvent a{}; a.key = KeyCode::a; a.is_down = true;   // hold 'a' = C2 (48)
    kb.on_key_event(a);
    REQUIRE(ons == std::vector<int>{48});
    REQUIRE(offs.empty());

    // Switch to piano mode while 'a' is still held — the typing frame goes away,
    // so the note MUST be released (no stuck MIDI note).
    kb.set_mode(Mode::piano);
    REQUIRE(offs == std::vector<int>{48});
}

TEST_CASE("MusicalTypingKeyboard: external held set re-applies across a mode swap",
          "[view][musical-typing][toggle][regression]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    const int held[] = {48, 72};   // C2 + C4 held by the host
    kb.set_active_notes(held);
    // Typing mode shows only the typing 'a' (C2 @ base) lit.
    REQUIRE(kb.element_value(note_idx(kb, 0)) == 1.0f);

    // Toggle to piano WITHOUT the host re-pushing — the still-held chord must
    // appear on the piano frame's keys (no vanish).
    kb.set_mode(Mode::piano);
    refit(kb);
    REQUIRE(kb.element_value(note_idx(kb, 48)) == 1.0f);   // C2 piano key lit
    REQUIRE(kb.element_value(note_idx(kb, 72)) == 1.0f);   // C4 piano key lit
    REQUIRE(kb.element_value(note_idx(kb, 60)) == 0.0f);   // not held → dark
}

// Guard the neutralization (the embedded SVGs must NOT bake the design's
// "selected keys shown" demo chord as lit key gradients — the live overlay owns
// all pressed-state lighting). The only legit accent-teal (#16DAC2) left is the
// toolbar active-icon + overview-strip highlight (3 each). A re-export that
// re-bakes a lit key chord adds ≥2 teal stops per lit key, tripping this.
namespace pulp::view::detail {
const char* musical_typing_typing_svg_b64();
const char* musical_typing_piano_svg_b64();
}
#include <pulp/runtime/base64.hpp>
TEST_CASE("MusicalTypingKeyboard: no baked-lit demo chord in the embedded SVGs",
          "[view][musical-typing][regression]") {
    auto count_teal = [](const char* b64) {
        auto bytes = pulp::runtime::base64_decode(b64);
        REQUIRE(bytes);
        std::string svg(bytes->begin(), bytes->end());
        int n = 0;
        for (size_t p = svg.find("#16DAC2"); p != std::string::npos;
             p = svg.find("#16DAC2", p + 1)) ++n;
        return n;
    };
    // Toolbar active-icon + overview highlight only — keys are resting.
    REQUIRE(count_teal(detail::musical_typing_typing_svg_b64()) <= 4);
    REQUIRE(count_teal(detail::musical_typing_piano_svg_b64()) <= 4);
}

// ── On-screen command controls (octave / velocity) ─────────────────────────
// ALL on-screen controls are now tagged Kind::momentary (so they tap-flash on
// press): octave/velocity steppers, the < > arrows, sustain, pitch-bend, and the
// modulation selector. There are no Kind::action elements.

TEST_CASE("MusicalTypingKeyboard: typing frame carries the command controls",
          "[view][musical-typing][controls]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    int actions = 0;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::action) ++actions;
    REQUIRE(actions == 0);   // everything is tap-flash momentary now
    // 18 note keys + 15 control momentary (octave ±×2 incl. < > arrows = 4,
    // velocity ± = 2, sustain = 1, pitch-bend ± = 2, modulation ×6).
    REQUIRE(note_key_count(kb) == 18);
    REQUIRE(momentary_count(kb) == 18 + 15);
    // Piano frame: 36 keys + the < > octave arrows.
    kb.set_mode(Mode::piano);
    int piano_actions = 0;
    for (int i = 0; i < kb.element_count(); ++i)
        if (kb.element_kind(i) == K::action) ++piano_actions;
    REQUIRE(piano_actions == 0);
    REQUIRE(note_key_count(kb) == 36);
    REQUIRE(momentary_count(kb) == 36 + 2);   // + < > arrows
}

TEST_CASE("MusicalTypingKeyboard: z/x/c/v flash the matching on-screen button",
          "[view][musical-typing][controls]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    auto lit = [&](const char* tag) {
        for (int i = 0; i < kb.element_count(); ++i)
            if (kb.element_action(i) == tag) return kb.element_value(i) > 0.5f;
        return false;
    };
    KeyEvent z{}; z.key = KeyCode::z; z.is_down = true; kb.on_key_event(z);
    REQUIRE(lit("octave_down"));                 // held → button lit
    z.is_down = false; kb.on_key_event(z);
    REQUIRE_FALSE(lit("octave_down"));           // released → cleared
    KeyEvent v{}; v.key = KeyCode::v; v.is_down = true; kb.on_key_event(v);
    REQUIRE(lit("vel_up"));
    v.is_down = false; kb.on_key_event(v);
    REQUIRE_FALSE(lit("vel_up"));
}

TEST_CASE("MusicalTypingKeyboard: on-screen octave −/+ shift the played note",
          "[view][musical-typing][controls]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    auto tap_a = [&] { KeyEvent e{}; e.key = KeyCode::a; e.is_down = true; kb.on_key_event(e);
                       e.is_down = false; kb.on_key_event(e); };

    kb.on_mouse_down({171, 229}); kb.on_mouse_up({171, 229});  // octave_up (155,213)
    tap_a();
    REQUIRE(ons.back() == 60);   // C3
    kb.on_mouse_down({130, 229}); kb.on_mouse_up({130, 229});  // octave_down (114,213)
    kb.on_mouse_down({130, 229}); kb.on_mouse_up({130, 229});
    tap_a();
    REQUIRE(ons.back() == 36);   // C1
    REQUIRE(kb.controller().octave_shift() == -1);
}

TEST_CASE("MusicalTypingKeyboard: on-screen velocity −/+ adjust the controller",
          "[view][musical-typing][controls]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    const float v0 = kb.controller().velocity;
    kb.on_mouse_down({378, 229}); kb.on_mouse_up({378, 229});  // vel_up (362,213)
    REQUIRE(kb.controller().velocity > v0);
    const float v1 = kb.controller().velocity;
    kb.on_mouse_down({337, 229}); kb.on_mouse_up({337, 229});  // vel_down (321,213)
    REQUIRE(kb.controller().velocity < v1);
}

// ── Logic-faithful controls: pitch-bend 1/2 momentary, modulation 3–8 latched,
//    sustain (tab) hold. On-screen buttons and number-row keys share one path. ─

TEST_CASE("MusicalTypingKeyboard: keys 1/2 are momentary pitch bend (down/up)",
          "[view][musical-typing][controls][pitchbend]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<float> bends;
    kb.on_pitch_bend = [&](float b) { bends.push_back(b); };
    const int down = tag_idx(kb, "pb_down");
    const int up   = tag_idx(kb, "pb_up");
    REQUIRE(down >= 0); REQUIRE(up >= 0);

    KeyEvent e{}; e.key = KeyCode::num1; e.is_down = true; kb.on_key_event(e);  // hold 1
    REQUIRE(bends.back() == -1.0f);             // full bend down while held
    REQUIRE(kb.element_value(down) == 1.0f);    // lit while held
    e.is_down = false; kb.on_key_event(e);                                     // release
    REQUIRE(bends.back() == 0.0f);              // springs back to center
    REQUIRE(kb.element_value(down) == 0.0f);    // unlit

    e.key = KeyCode::num2; e.is_down = true; kb.on_key_event(e);  // hold 2 → up
    REQUIRE(bends.back() == 1.0f);
    REQUIRE(kb.element_value(up) == 1.0f);
    e.is_down = false; kb.on_key_event(e);
    REQUIRE(bends.back() == 0.0f);
}

TEST_CASE("MusicalTypingKeyboard: pitch-bend pads mirror keys 1/2",
          "[view][musical-typing][controls][pitchbend]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<float> bends;
    kb.on_pitch_bend = [&](float b) { bends.push_back(b); };
    kb.on_mouse_down({126, 82});                 // pb_down pad (108,63,36,38)
    REQUIRE(bends.back() == -1.0f);
    kb.on_mouse_up({126, 82});
    REQUIRE(bends.back() == 0.0f);
    kb.on_mouse_down({168, 82}); kb.on_mouse_up({168, 82});  // pb_up pad (150,63)
    REQUIRE(bends == std::vector<float>{-1.0f, 0.0f, 1.0f, 0.0f});
}

TEST_CASE("MusicalTypingKeyboard: keys 3–8 are a latched modulation selector",
          "[view][musical-typing][controls][modulation]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<float> mods;
    kb.on_modulation = [&](float a) { mods.push_back(a); };
    // Default is step 0 ("off") — its pad lit, others dark, before any input.
    REQUIRE(kb.element_value(tag_idx(kb, "mod_0")) == 1.0f);
    REQUIRE(kb.element_value(tag_idx(kb, "mod_3")) == 0.0f);

    tap_key(kb, KeyCode::num8);                   // key 8 → step 5 (max)
    REQUIRE(mods.back() == 1.0f);
    REQUIRE(kb.element_value(tag_idx(kb, "mod_5")) == 1.0f);   // latched lit
    REQUIRE(kb.element_value(tag_idx(kb, "mod_0")) == 0.0f);   // previous cleared

    tap_key(kb, KeyCode::num5);                   // key 5 → step 2
    REQUIRE(mods.back() == Catch::Approx(2.0f / 5.0f));
    REQUIRE(kb.element_value(tag_idx(kb, "mod_2")) == 1.0f);   // selection persists
    REQUIRE(kb.element_value(tag_idx(kb, "mod_5")) == 0.0f);
}

TEST_CASE("MusicalTypingKeyboard: modulation pads mirror keys 3–8 and latch",
          "[view][musical-typing][controls][modulation]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<float> mods;
    kb.on_modulation = [&](float a) { mods.push_back(a); };
    // mod_3 pad sits at x=326 (200 + 3*42), y=63,36,38 → center ~ (344,82).
    kb.on_mouse_down({344, 82});
    REQUIRE(mods.back() == Catch::Approx(3.0f / 5.0f));
    kb.on_mouse_up({344, 82});
    // Latches: still lit after the mouse-up cleared the momentary press.
    REQUIRE(kb.element_value(tag_idx(kb, "mod_3")) == 1.0f);
}

TEST_CASE("MusicalTypingKeyboard: tab + sustain pad are a momentary hold",
          "[view][musical-typing][controls][sustain]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<bool> states;
    kb.on_sustain = [&](bool on) { states.push_back(on); };
    const int pad = tag_idx(kb, "sustain");

    KeyEvent e{}; e.key = KeyCode::tab; e.is_down = true; kb.on_key_event(e);  // hold tab
    REQUIRE(states.back() == true);
    REQUIRE(kb.element_value(pad) == 1.0f);     // lit while held
    e.is_down = false; kb.on_key_event(e);                                    // release
    REQUIRE(states.back() == false);
    REQUIRE(kb.element_value(pad) == 0.0f);

    // The pad mirrors the key: press-hold lights, release clears.
    kb.on_mouse_down({54, 156});                 // sustain pad (21,110,66,92)
    REQUIRE(states.back() == true);
    REQUIRE(kb.element_value(pad) == 1.0f);
    kb.on_mouse_up({54, 156});
    REQUIRE(states.back() == false);
    REQUIRE(kb.element_value(pad) == 0.0f);
}

TEST_CASE("MusicalTypingKeyboard: a command-button click plays no note",
          "[view][musical-typing][controls]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    kb.on_mouse_down({171, 229}); kb.on_mouse_up({171, 229});  // octave_up
    kb.on_mouse_down({54, 156});  kb.on_mouse_up({54, 156});   // sustain pad
    kb.on_mouse_down({126, 82});  kb.on_mouse_up({126, 82});   // pitch-bend pad
    REQUIRE(ons.empty());
}

// ── Overview-strip octave control (#80): drag to set the octave, snap to C ──
// bounds == panel → scale 1, origin (0,0), so panel coords map 1:1 to clicks.
// Typing strip rest_center≈348.24, px/oct≈37.0 (travel/4). Each octave step is
// a C boundary, so the snap == "always lands on a C range".

TEST_CASE("MusicalTypingKeyboard: dragging the overview strip sets the octave (snap-C)",
          "[view][musical-typing][overview]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };

    // Press on the centered full-range ruler ~2 octaves right of the octave-0
    // window center (≈344 + 2*~37 ≈ 418), y in the strip band. Snaps to octave +2.
    kb.on_mouse_down({418.0f, 30.0f});
    REQUIRE(kb.controller().octave_shift() == 2);
    REQUIRE(ons.empty());
    // 'a' now sounds C4 (48 + 24).
    KeyEvent a{}; a.key = KeyCode::a; a.is_down = true; kb.on_key_event(a);
    REQUIRE(ons.back() == 72);

    // Drag far left → clamps to −4 (C-2 = 48 − 48 = MIDI 0).
    kb.on_mouse_drag({120.0f, 33.0f});
    REQUIRE(kb.controller().octave_shift() == -4);
    // Drag far right → clamps to +5 on the typing tab (its play-window top then
    // reaches the strip's high end near G8; the −4..+5 range is asymmetric so the
    // C-aligned window climbs as high as it can without overshooting MIDI 127).
    kb.on_mouse_drag({700.0f, 33.0f});
    REQUIRE(kb.controller().octave_shift() == 5);
    kb.on_mouse_up({700.0f, 33.0f});
}

TEST_CASE("MusicalTypingKeyboard: strip drag reaches C-2 even when base > C2",
          "[view][musical-typing][overview]") {
    // A sampler sets the keyboard base to the sample root (often C3). The strip's
    // far-left drag must still bottom the play-window low note on C-2 (MIDI 0); a
    // fixed −4 floor would stop at C-1 (the overview-highlight-can't-reach-C-2 bug).
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.controller().set_base_note(60);              // C3 root, like the tempo sampler
    refit(kb);
    kb.on_mouse_down({400.0f, 30.0f});              // grab the strip
    kb.on_mouse_drag({120.0f, 33.0f});              // drag hard left
    REQUIRE(kb.controller().octave_shift() == -5);  // 60 − 60 = MIDI 0 = C-2
    REQUIRE(kb.controller().base_note() + kb.controller().octave_shift() * 12 == 0);
    kb.on_mouse_up({120.0f, 33.0f});
}

TEST_CASE("MusicalTypingKeyboard: the overview highlight is coupled to z/x + the arrows",
          "[view][musical-typing][overview]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    // z/x move the same octave the strip shows.
    KeyEvent x{}; x.key = KeyCode::x; x.is_down = true; kb.on_key_event(x);
    REQUIRE(kb.controller().octave_shift() == 1);
    // The < > arrows (on_action octave_up/down) too — after #82 centered the
    // strip the > arrow is at (571,17,22,24); click its chevron at ~582.
    kb.on_mouse_down({582.0f, 29.0f}); kb.on_mouse_up({582.0f, 29.0f});
    REQUIRE(kb.controller().octave_shift() == 2);
}

TEST_CASE("MusicalTypingKeyboard: the overview strip shows a grab cursor",
          "[view][musical-typing][overview]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    kb.on_hover_move({400.0f, 33.0f});          // over the strip band
    REQUIRE(kb.cursor() == View::CursorStyle::grab);
    kb.on_hover_move({400.0f, 300.0f});         // over the keys, not the strip
    REQUIRE(kb.cursor() == View::CursorStyle::default_);
    kb.on_mouse_down({400.0f, 33.0f});          // grabbing while dragging
    REQUIRE(kb.cursor() == View::CursorStyle::grabbing);
    kb.on_mouse_up({400.0f, 33.0f});
}

TEST_CASE("MusicalTypingKeyboard: a strip click does not play or light a key",
          "[view][musical-typing][overview]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> ons;
    kb.on_note_on = [&](int n, float) { ons.push_back(n); };
    kb.on_mouse_down({344.0f, 30.0f});          // octave-0 window center (≈344)
    kb.on_mouse_up({344.0f, 30.0f});
    REQUIRE(ons.empty());
    REQUIRE(kb.controller().octave_shift() == 0);   // center ⇒ octave 0
}

TEST_CASE("MusicalTypingKeyboard: a mode toggle reports the new intrinsic size",
          "[view][musical-typing][toggle]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<std::pair<float, float>> sizes;
    kb.on_intrinsic_size_changed = [&](float w, float h) { sizes.push_back({w, h}); };

    kb.set_mode(Mode::piano);     // typing (732×266) → piano (732×176)
    REQUIRE(sizes.size() == 1);
    REQUIRE(sizes.back() == std::pair<float, float>{732.0f, 176.0f});
    kb.set_mode(Mode::typing);    // back → grow to 732×266
    REQUIRE(sizes.size() == 2);
    REQUIRE(sizes.back() == std::pair<float, float>{732.0f, 266.0f});

    // No spurious fire when set_mode targets the already-active frame.
    kb.set_mode(Mode::typing);
    REQUIRE(sizes.size() == 2);
}

TEST_CASE("MusicalTypingKeyboard: number-row keys 1–8 + tab are consumed",
          "[view][musical-typing][controls]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    for (KeyCode k : {KeyCode::num1, KeyCode::num2, KeyCode::num3, KeyCode::num4,
                      KeyCode::num5, KeyCode::num6, KeyCode::num7, KeyCode::num8,
                      KeyCode::tab}) {
        KeyEvent e{}; e.key = k; e.is_down = true;
        REQUIRE(kb.on_key_event(e));             // consumed (was ignored before)
        e.is_down = false; kb.on_key_event(e);
    }
}

TEST_CASE("MusicalTypingKeyboard: pitch-bend/modulation pad highlight covers the full baked button",
          "[view][musical-typing][controls]") {
    // Regression: the baked pitch-bend/modulation pads span panel y[53,90]
    // (pixel-measured from the rendered SVG). The momentary element rect doubles
    // as BOTH the hit target and the lit-teal press highlight, so it must reach
    // the pad bottom. An earlier h=32 (rect ended at y=85) left the bottom ~5px
    // of the pad uncovered — the lit highlight showed a dark band under it.
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    for (const char* tag : {"pb_down", "pb_up", "mod_0", "mod_1", "mod_2",
                            "mod_3", "mod_4", "mod_5"}) {
        const int idx = tag_idx(kb, tag);
        REQUIRE(idx >= 0);
        const auto r = kb.element_rect(idx);
        INFO("control pad tag=" << tag);
        REQUIRE(r.height == Catch::Approx(37.0f));   // full baked-button height
        REQUIRE(r.bottom() == Catch::Approx(90.0f)); // reaches the pad bottom
        // a point near the bottom (below the old h=32 edge at y=85) is covered
        REQUIRE(r.contains({r.center().x, 88.0f}));
    }
}
