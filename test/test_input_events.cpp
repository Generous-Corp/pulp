#include <catch2/catch_test_macros.hpp>
#include <pulp/view/input_events.hpp>

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

    e.pointer_id = 1;
    REQUIRE(e.isTouch());
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
