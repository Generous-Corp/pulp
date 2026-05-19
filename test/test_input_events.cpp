#include <catch2/catch_test_macros.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/view.hpp>

using namespace pulp::view;

TEST_CASE("Modifier flags are distinct bits", "[view][input]") {
    REQUIRE(kModShift == 1);
    REQUIRE(kModCtrl == 2);
    REQUIRE(kModAlt == 4);
    REQUIRE(kModMeta == 8);
    REQUIRE(kModCmd == 16);
}

TEST_CASE("MouseEvent modifier queries", "[view][input]") {
    MouseEvent e;
    e.modifiers = kModShift | kModCmd;

    REQUIRE(e.isShiftDown());
    REQUIRE(e.isCmdDown());
    REQUIRE_FALSE(e.isCtrlDown());
    REQUIRE_FALSE(e.isAltDown());
}

TEST_CASE("MouseEvent isMainModifier is platform-aware", "[view][input]") {
    MouseEvent e;
#ifdef __APPLE__
    e.modifiers = kModCmd;
    REQUIRE(e.isMainModifier());
    e.modifiers = kModCtrl;
    REQUIRE_FALSE(e.isMainModifier());
#else
    e.modifiers = kModCtrl;
    REQUIRE(e.isMainModifier());
#endif
}

TEST_CASE("MouseEvent pointer_id defaults to primary", "[view][input]") {
    MouseEvent e;
    REQUIRE(e.pointer_id == 0);
    REQUIRE_FALSE(e.isTouch());

    // pointer_id alone doesn't make it a touch — need pointer_type or touch flag
    e.pointer_type = PointerType::touch;
    REQUIRE(e.isTouch());

    // Legacy touch flag (0x8000) also works
    MouseEvent e2;
    e2.modifiers = 0x8000;
    REQUIRE(e2.isTouch());
}

TEST_CASE("MouseEvent click_count defaults to 1", "[view][input]") {
    MouseEvent e;
    REQUIRE(e.click_count == 1);
}

TEST_CASE("KeyEvent modifier queries", "[view][input]") {
    KeyEvent e;
    e.modifiers = kModAlt | kModShift;

    REQUIRE(e.isAltDown());
    REQUIRE(e.isShiftDown());
    REQUIRE_FALSE(e.isCmdDown());
}

TEST_CASE("KeyEvent combined modifier helpers stay independent",
          "[view][input][coverage][phase3]") {
    KeyEvent e;
    e.modifiers = kModShift | kModCtrl | kModAlt | kModCmd;

    REQUIRE(e.isShiftDown());
    REQUIRE(e.isCtrlDown());
    REQUIRE(e.isAltDown());
    REQUIRE(e.isCmdDown());
}

TEST_CASE("KeyEvent defaults", "[view][input]") {
    KeyEvent e;
    REQUIRE(e.key == KeyCode::unknown);
    REQUIRE(e.is_down == true);
    REQUIRE(e.is_repeat == false);
}

TEST_CASE("TextInputEvent holds UTF-8 text", "[view][input]") {
    TextInputEvent e;
    e.text = "Hello";
    REQUIRE(e.text == "Hello");
}

TEST_CASE("KeyCode letter values match ASCII", "[view][input]") {
    REQUIRE(static_cast<int>(KeyCode::a) == 'a');
    REQUIRE(static_cast<int>(KeyCode::z) == 'z');
}

TEST_CASE("KeyCode navigation keys are sequential", "[view][input]") {
    REQUIRE(static_cast<int>(KeyCode::right) == static_cast<int>(KeyCode::left) + 1);
    REQUIRE(static_cast<int>(KeyCode::up) == static_cast<int>(KeyCode::right) + 1);
    REQUIRE(static_cast<int>(KeyCode::down) == static_cast<int>(KeyCode::up) + 1);
}

// ── Pointer type tests (P2/P3) ─────────────────────────────────────────

TEST_CASE("MouseEvent default pointer type is mouse", "[view][input][pointer]") {
    MouseEvent e;
    REQUIRE(e.pointer_type == PointerType::mouse);
    REQUIRE(std::string(e.pointerTypeString()) == "mouse");
    REQUIRE(e.isPrimary());
    REQUIRE(e.pressure == 0.5f); // mouse default
}

TEST_CASE("MouseEvent touch pointer type", "[view][input][pointer]") {
    MouseEvent e;
    e.pointer_type = PointerType::touch;
    e.pointer_id = 1;
    REQUIRE(e.isTouch());
    REQUIRE_FALSE(e.isPrimary());
    REQUIRE(std::string(e.pointerTypeString()) == "touch");
}

TEST_CASE("MouseEvent pen pointer type with stylus data", "[view][input][pointer]") {
    MouseEvent e;
    e.pointer_type = PointerType::pen;
    e.pressure = 0.8f;
    e.altitude_angle = 1.2f;
    e.azimuth_angle = 3.14f;

    REQUIRE(e.isPen());
    REQUIRE(std::string(e.pointerTypeString()) == "pen");
    REQUIRE(e.pressure == 0.8f);
    REQUIRE(e.altitude_angle == 1.2f);
    REQUIRE(e.azimuth_angle == 3.14f);
}

TEST_CASE("MouseEvent unknown pointer type falls back to mouse string",
          "[view][input][pointer][coverage][phase3]") {
    MouseEvent e;
    e.pointer_type = static_cast<PointerType>(99);
    e.pointer_id = 3;

    REQUIRE(std::string(e.pointerTypeString()) == "mouse");
    REQUIRE_FALSE(e.isTouch());
    REQUIRE_FALSE(e.isPen());
    REQUIRE_FALSE(e.isPrimary());
}

TEST_CASE("MouseEvent is_cancelled flag", "[view][input][pointer]") {
    MouseEvent e;
    REQUIRE_FALSE(e.is_cancelled);
    e.is_cancelled = true;
    REQUIRE(e.is_cancelled);
}

TEST_CASE("MouseEvent wheel and meta helper edge paths",
          "[view][input][issue-493]") {
    MouseEvent e;
    REQUIRE(e.button == MouseButton::left);
    REQUIRE_FALSE(e.isWheel());
    REQUIRE_FALSE(e.isMetaDown());

    e.button = MouseButton::none;
    e.modifiers = kModMeta | kModCtrl;
    e.is_wheel = true;
    e.scroll_delta_x = -3.0f;
    e.scroll_delta_y = 7.0f;
    e.pointer_id = 2;

    REQUIRE(e.button == MouseButton::none);
    REQUIRE(e.isWheel());
    REQUIRE(e.isMetaDown());
    REQUIRE(e.isCtrlDown());
    REQUIRE_FALSE(e.isPrimary());
    REQUIRE(e.scroll_delta_x == -3.0f);
    REQUIRE(e.scroll_delta_y == 7.0f);
}

// ── Gesture event tests (P4) ───────────────────────────────────────────

TEST_CASE("GestureEvent defaults", "[view][input][gesture]") {
    GestureEvent ge;
    REQUIRE(ge.phase == GesturePhase::began);
    REQUIRE(ge.scale == 1.0f);
    REQUIRE(ge.rotation == 0.0f);
    REQUIRE(ge.delta_scale == 0.0f);
}

TEST_CASE("GestureEvent pinch", "[view][input][gesture]") {
    GestureEvent ge;
    ge.phase = GesturePhase::changed;
    ge.scale = 1.5f;
    ge.delta_scale = 0.05f;
    ge.position = {100, 200};

    REQUIRE(ge.scale == 1.5f);
    REQUIRE(ge.position.x == 100.0f);
}

TEST_CASE("GestureEvent ended and cancelled phases carry deltas",
          "[view][input][gesture][issue-493]") {
    GestureEvent ended;
    ended.phase = GesturePhase::ended;
    ended.rotation = 0.75f;
    ended.delta_rotation = 0.25f;
    ended.position = {12, 34};

    REQUIRE(ended.phase == GesturePhase::ended);
    REQUIRE(ended.rotation == 0.75f);
    REQUIRE(ended.delta_rotation == 0.25f);
    REQUIRE(ended.position.y == 34.0f);

    GestureEvent cancelled;
    cancelled.phase = GesturePhase::cancelled;
    cancelled.scale = 0.8f;
    cancelled.delta_scale = -0.2f;

    REQUIRE(cancelled.phase == GesturePhase::cancelled);
    REQUIRE(cancelled.scale == 0.8f);
    REQUIRE(cancelled.delta_scale == -0.2f);
}

TEST_CASE("KeyEvent main modifier release and repeat edge paths",
          "[view][input][issue-493]") {
    KeyEvent e;
    e.key = KeyCode::enter;
    e.is_down = false;
    e.is_repeat = true;
    REQUIRE(e.key == KeyCode::enter);
    REQUIRE_FALSE(e.is_down);
    REQUIRE(e.is_repeat);
    REQUIRE_FALSE(e.isCtrlDown());
    REQUIRE_FALSE(e.isCmdDown());
    REQUIRE_FALSE(e.isMainModifier());

#ifdef __APPLE__
    e.modifiers = kModCmd;
    REQUIRE(e.isCmdDown());
    REQUIRE(e.isMainModifier());
#else
    e.modifiers = kModCtrl;
    REQUIRE(e.isCtrlDown());
    REQUIRE(e.isMainModifier());
#endif
}

// ── Pointer capture tests (P2b) ────────────────────────────────────────

TEST_CASE("View pointer capture", "[view][input][capture]") {
    View v;
    REQUIRE_FALSE(v.has_pointer_capture(0));

    v.set_pointer_capture(0);
    REQUIRE(v.has_pointer_capture(0));
    REQUIRE_FALSE(v.has_pointer_capture(1));

    v.set_pointer_capture(1);
    REQUIRE(v.has_pointer_capture(1));

    v.release_pointer_capture(0);
    REQUIRE_FALSE(v.has_pointer_capture(0));
    REQUIRE(v.has_pointer_capture(1));
}

TEST_CASE("View pointer capture duplicate is no-op", "[view][input][capture]") {
    View v;
    v.set_pointer_capture(0);
    v.set_pointer_capture(0); // duplicate
    REQUIRE(v.has_pointer_capture(0));
    v.release_pointer_capture(0);
    REQUIRE_FALSE(v.has_pointer_capture(0)); // single release clears it
}

TEST_CASE("View pointer capture ignores missing release ids",
          "[view][input][capture][issue-493]") {
    View v;
    v.set_pointer_capture(1);
    v.set_pointer_capture(2);

    v.release_pointer_capture(99);
    REQUIRE(v.has_pointer_capture(1));
    REQUIRE(v.has_pointer_capture(2));

    v.release_pointer_capture(1);
    v.release_pointer_capture(1);
    REQUIRE_FALSE(v.has_pointer_capture(1));
    REQUIRE(v.has_pointer_capture(2));
}
