// DesignFrameView `Kind::momentary` press/release primitive. Covers the
// locked edges — note semantics, smallest-area hit tiebreak, per-view scoping,
// release-on-mode-switch, glissando — plus set_element_value lighting.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_frame_view.hpp>

#include <memory>
#include <vector>

using namespace pulp::view;
using K = DesignFrameElement::Kind;

namespace {
// A 200x100 frame with two typing keys (group 0) and one piano key (group 1).
// White note0 spans x[0,40]; black note1 spans x[25,45] y[0,60] (overlaps white
// in the band x[25,40] y[0,60]); piano note60 spans x[0,40] (group 1).
std::unique_ptr<DesignFrameView> make_frame() {
    DesignFrameElement white; white.kind = K::momentary; white.note = 0; white.view_group = 0;
    white.x = 0;  white.y = 0; white.w = 40; white.h = 100;
    DesignFrameElement black; black.kind = K::momentary; black.note = 1; black.view_group = 0;
    black.x = 25; black.y = 0; black.w = 20; black.h = 60;
    DesignFrameElement piano; piano.kind = K::momentary; piano.note = 60; piano.view_group = 1;
    piano.x = 0;  piano.y = 0; piano.w = 40; piano.h = 100;
    auto v = std::make_unique<DesignFrameView>(
        "<svg width='200' height='100' viewBox='0 0 200 100'></svg>",
        std::vector<DesignFrameElement>{white, black, piano}, 0, 0, 200, 100);
    v->set_bounds({0, 0, 200, 100});
    return v;
}
}  // namespace

TEST_CASE("DesignFrameView momentary: note + kind accessors", "[design-frame][momentary]") {
    auto vp = make_frame(); auto& v = *vp;
    REQUIRE(v.element_kind(0) == K::momentary);
    REQUIRE(v.element_note(0) == 0);
    REQUIRE(v.element_note(1) == 1);
    REQUIRE(v.element_note(2) == 60);
    REQUIRE(v.element_note(99) == -1);
}

TEST_CASE("DesignFrameView momentary: smallest-area hit tiebreak", "[design-frame][momentary]") {
    auto vp = make_frame(); auto& v = *vp;
    v.set_active_view_group(0);  // typing only
    std::vector<int> begins, ends;
    v.on_gesture_begin = [&](int i) { begins.push_back(i); };
    v.on_gesture_end = [&](int i) { ends.push_back(i); };

    // Point (30,30) is inside BOTH white and black; the smaller black key wins.
    v.on_mouse_down({30.0f, 30.0f});
    REQUIRE(begins == std::vector<int>{1});
    REQUIRE(v.element_value(1) == 1.0f);   // lit while held
    v.on_mouse_up({30.0f, 30.0f});
    REQUIRE(ends == std::vector<int>{1});
    REQUIRE(v.element_value(1) == 0.0f);

    // White-only band (below the black key, y>60): hits the white key.
    begins.clear();
    v.on_mouse_down({10.0f, 80.0f});
    REQUIRE(begins == std::vector<int>{0});
    v.on_mouse_up({10.0f, 80.0f});
}

TEST_CASE("DesignFrameView momentary: glissando retriggers", "[design-frame][momentary]") {
    auto vp = make_frame(); auto& v = *vp;
    v.set_active_view_group(0);
    std::vector<int> begins, ends;
    v.on_gesture_begin = [&](int i) { begins.push_back(i); };
    v.on_gesture_end = [&](int i) { ends.push_back(i); };

    v.on_mouse_down({10.0f, 80.0f});      // press white (note0)
    v.on_mouse_drag({30.0f, 30.0f});      // drag onto black (note1)
    REQUIRE(begins == std::vector<int>{0, 1});  // note-on white, then black
    REQUIRE(ends == std::vector<int>{0});       // white released first
    v.on_mouse_up({30.0f, 30.0f});
    REQUIRE(ends == std::vector<int>{0, 1});
}

TEST_CASE("DesignFrameView momentary: per-view scoping", "[design-frame][momentary]") {
    auto vp = make_frame(); auto& v = *vp;
    std::vector<int> begins;
    v.on_gesture_begin = [&](int i) { begins.push_back(i); };

    v.set_active_view_group(1);            // piano view
    v.on_mouse_down({10.0f, 50.0f});       // typing keys are skipped → piano (idx 2)
    REQUIRE(begins == std::vector<int>{2});
    v.on_mouse_up({10.0f, 50.0f});
}

TEST_CASE("DesignFrameView momentary: release on mode switch (no stuck notes)",
          "[design-frame][momentary]") {
    auto vp = make_frame(); auto& v = *vp;
    v.set_active_view_group(0);
    std::vector<int> ends;
    v.on_gesture_end = [&](int i) { ends.push_back(i); };

    v.on_mouse_down({10.0f, 80.0f});       // hold a typing key
    REQUIRE(ends.empty());
    v.set_active_view_group(1);            // switch view while held → release
    REQUIRE(ends == std::vector<int>{0});
}

TEST_CASE("DesignFrameView momentary: set_element_value lights without callback",
          "[design-frame][momentary]") {
    auto vp = make_frame(); auto& v = *vp;
    bool changed = false;
    v.on_element_changed = [&](int, float) { changed = true; };
    v.set_element_value(0, 1.0f);
    REQUIRE(v.element_value(0) == 1.0f);   // lit
    v.set_element_value(0, 0.0f);
    REQUIRE(v.element_value(0) == 0.0f);
    REQUIRE_FALSE(changed);                // host->view push must not echo
}
