// MusicalTypingKeyboard — Ink & Signal catalog component, faithful Figma SVG
// rendered via DesignFrameView (the figma-plugin faithful-vector lane). These
// pin: the embedded SVG loads (non-empty panel), the component renders
// headlessly, and it is discoverable in the pulp::design catalog.

#include <catch2/catch_test_macros.hpp>

#include <pulp/design/design_system.hpp>
#include <pulp/view/musical_typing_keyboard.hpp>
#include <pulp/view/screenshot.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using namespace pulp::view;

namespace {
using K = DesignFrameElement::Kind;
// DesignFrameView (a View) is non-copyable/non-movable, so hand back a pointer.
std::unique_ptr<MusicalTypingKeyboard> make_playable_kb() {
    auto kb = std::make_unique<MusicalTypingKeyboard>();
    // Bounds == panel size → identity transform (scale 1, no letterbox), so SVG
    // coords map straight to view coords for spatial clicks.
    kb->set_bounds({0, 0, kb->panel_width(), kb->panel_height()});
    return kb;
}
}  // namespace

TEST_CASE("MusicalTypingKeyboard loads its embedded faithful SVG", "[view][musical-typing]") {
    MusicalTypingKeyboard kbd;
    // DesignFrameView auto-detects the panel from the SVG's largest rect; a
    // non-empty panel proves the embedded base64 SVG decoded and parsed.
    REQUIRE(kbd.panel_width() > 0.0f);
    REQUIRE(kbd.panel_height() > 0.0f);
}

TEST_CASE("MusicalTypingKeyboard renders headlessly", "[view][musical-typing]") {
    MusicalTypingKeyboard kbd;
    kbd.set_bounds({0.0f, 0.0f, 900.0f, 300.0f});
    auto png = render_to_png(kbd, 900, 300, 1.0f, ScreenshotBackend::skia);
    if (png.empty()) SKIP("Skia raster screenshot backend unavailable");  // no Skia (e.g. Windows CI)
    REQUIRE(png.size() > 1000);  // a real PNG, not an empty/error buffer
}

TEST_CASE("MusicalTyping is registered in the pulp::design catalog", "[design][catalog]") {
    const auto* info = pulp::design::find("MusicalTyping");
    REQUIRE(info != nullptr);
    REQUIRE(info->native_class == "pulp::view::MusicalTypingKeyboard");
    REQUIRE(info->category == pulp::design::Category::audio);
}

TEST_CASE("MusicalTypingKeyboard: typing + piano playable momentary keys",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    // 18 typing keys (relative semitone 0..17) + 36 piano keys (chromatic
    // C2..B4 = MIDI 48..83). Both keyboards render in the faithful frame and
    // are playable at once.
    REQUIRE(kb.element_count() == 18 + 36);
    REQUIRE(kb.active_view_group() == 0);
    for (int i = 0; i < kb.element_count(); ++i)
        REQUIRE(kb.element_kind(i) == K::momentary);
    // Typing row: array order == relative semitone.
    for (int i = 0; i < 18; ++i) REQUIRE(kb.element_note(i) == i);
    // Piano row: the full chromatic span 48..83 is present, each exactly once.
    std::vector<int> piano;
    for (int i = 18; i < kb.element_count(); ++i) piano.push_back(kb.element_note(i));
    std::sort(piano.begin(), piano.end());
    REQUIRE(piano.front() == 48);   // C2
    REQUIRE(piano.back() == 83);    // B4
    REQUIRE(piano.size() == 36);
    for (int n = 48; n <= 83; ++n)
        REQUIRE(std::count(piano.begin(), piano.end(), n) == 1);
}

TEST_CASE("MusicalTypingKeyboard: piano white-key click plays its MIDI note",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> begins;
    kb.on_gesture_begin = [&](int i) { begins.push_back(kb.element_note(i)); };
    // Leftmost piano white key C2 (MIDI 48): rect x[90,120] y[456,535]; click
    // low-centre (below the black keys) so only the white key is under the point.
    kb.on_mouse_down({100.0f, 525.0f});
    REQUIRE(begins == std::vector<int>{48});
    kb.on_mouse_up({100.0f, 525.0f});
}

TEST_CASE("MusicalTypingKeyboard: white-key click plays its note",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> begins, ends;
    kb.on_gesture_begin = [&](int i) { begins.push_back(kb.element_note(i)); };
    kb.on_gesture_end = [&](int i) { ends.push_back(kb.element_note(i)); };

    // First white key 'a' (note 0): rect x[166,216] y[233,311]; click low-centre
    // (below the black keys) so only the white key is under the point.
    kb.on_mouse_down({191.0f, 300.0f});
    REQUIRE(begins == std::vector<int>{0});
    REQUIRE(kb.element_value(0) == 1.0f);   // lit while held
    kb.on_mouse_up({191.0f, 300.0f});
    REQUIRE(ends == std::vector<int>{0});
    REQUIRE(kb.element_value(0) == 0.0f);
}

TEST_CASE("MusicalTypingKeyboard: black key wins the overlap (smallest area)",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    std::vector<int> begins;
    kb.on_gesture_begin = [&](int i) { begins.push_back(kb.element_note(i)); };

    // 'w' (note 1) rect x[203,233] y[233,287] overlaps white 'a' (note 0) in the
    // band x[203,216]. A click there must pick the narrower black key.
    kb.on_mouse_down({210.0f, 250.0f});
    REQUIRE(begins == std::vector<int>{1});
    kb.on_mouse_up({210.0f, 250.0f});
}

TEST_CASE("MusicalTypingKeyboard: set_element_value lights without firing change",
          "[view][musical-typing][momentary]") {
    auto kbp = make_playable_kb(); auto& kb = *kbp;
    bool changed = false;
    kb.on_element_changed = [&](int, float) { changed = true; };
    kb.set_element_value(5, 1.0f);          // light 'f' (note 5)
    REQUIRE(kb.element_value(5) == 1.0f);
    kb.set_element_value(5, 0.0f);
    REQUIRE(kb.element_value(5) == 0.0f);
    REQUIRE_FALSE(changed);                 // host->view push must not echo
}
