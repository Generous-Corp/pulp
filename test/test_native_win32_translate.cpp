// The Win32 translation tables of pulp::native: virtual keys, modifier and
// button state, wheel deltas and system cursors. Pure, so they run on every
// platform.

#include <pulp/native/input.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace pulp::native;

TEST_CASE("Win32 virtual keys map to physical keys", "[native][input][win32]") {
    REQUIRE(key_for_win32_vk(0x41, false) == Key::a);
    REQUIRE(key_for_win32_vk(0x5a, false) == Key::z);
    REQUIRE(key_for_win32_vk(0x35, false) == Key::digit_5);
    REQUIRE(key_for_win32_vk(0x20, false) == Key::space);
    REQUIRE(key_for_win32_vk(0x1b, false) == Key::escape);
    REQUIRE(key_for_win32_vk(0x25, false) == Key::left);
    REQUIRE(key_for_win32_vk(0x70, false) == Key::f1);
    REQUIRE(key_for_win32_vk(0x87, false) == Key::f24);
    REQUIRE(key_for_win32_vk(0x67, false) == Key::keypad_7);
    // The extended bit tells the keypad Enter from the main one.
    REQUIRE(key_for_win32_vk(0x0d, false) == Key::enter);
    REQUIRE(key_for_win32_vk(0x0d, true) == Key::keypad_enter);
    REQUIRE(key_for_win32_vk(0xa0, false) == Key::shift_left);
    REQUIRE(key_for_win32_vk(0xa3, false) == Key::control_right);
    REQUIRE(key_for_win32_vk(0xa4, false) == Key::option_left);
    REQUIRE(key_for_win32_vk(0x5b, false) == Key::command_left);
    REQUIRE(key_for_win32_vk(0xbd, false) == Key::minus);
    REQUIRE(key_for_win32_vk(0xdb, false) == Key::left_bracket);
    REQUIRE(key_for_win32_vk(0x0, false) == Key::unknown);
    REQUIRE(key_for_win32_vk(0xff, false) == Key::unknown);
}

TEST_CASE("Win32 key and button state map to Pulp's", "[native][input][win32]") {
    REQUIRE(modifiers_from_win32_keys(false, false, false, false, false) == 0u);
    REQUIRE(modifiers_from_win32_keys(true, false, false, false, false) == modifier_shift);
    REQUIRE(modifiers_from_win32_keys(false, true, true, false, false) ==
            (modifier_control | modifier_option));
    REQUIRE(modifiers_from_win32_keys(false, false, false, true, true) ==
            (modifier_command | modifier_caps_lock));

    REQUIRE(buttons_held_from_win32_mk(0) == 0u);
    REQUIRE(buttons_held_from_win32_mk(0x0001) == 1u); // MK_LBUTTON
    REQUIRE(buttons_held_from_win32_mk(0x0002) == 2u); // MK_RBUTTON
    REQUIRE(buttons_held_from_win32_mk(0x0010) == 4u); // MK_MBUTTON
    REQUIRE(buttons_held_from_win32_mk(0x0001 | 0x0002) == 3u);
    REQUIRE(buttons_held_from_win32_mk(0x0004 | 0x0008) == 0u); // shift and control
}

TEST_CASE("Win32 wheel deltas become lines with WheelEvent's sign", "[native][input][win32]") {
    REQUIRE(win32_wheel_lines(120, false) == 1.0); // wheel away: content moves down
    REQUIRE(win32_wheel_lines(-240, false) == -2.0);
    REQUIRE(win32_wheel_lines(60, false) == 0.5);  // a finer wheel
    REQUIRE(win32_wheel_lines(120, true) == -1.0); // tilted right: content moves left
    REQUIRE(win32_wheel_lines(-120, true) == 1.0);
}

TEST_CASE("every cursor maps to a Win32 system cursor", "[native][input][win32]") {
    REQUIRE(win32_cursor_resource(Cursor::arrow) == 32512);         // IDC_ARROW
    REQUIRE(win32_cursor_resource(Cursor::ibeam) == 32513);         // IDC_IBEAM
    REQUIRE(win32_cursor_resource(Cursor::busy) == 32514);          // IDC_WAIT
    REQUIRE(win32_cursor_resource(Cursor::pointing_hand) == 32649); // IDC_HAND
    std::vector<unsigned> ids;
    for (auto c :
         {Cursor::arrow, Cursor::pointing_hand, Cursor::closed_hand, Cursor::ibeam,
          Cursor::crosshair, Cursor::busy, Cursor::resize_horizontal, Cursor::resize_vertical,
          Cursor::resize_diagonal_nesw, Cursor::resize_diagonal_nwse, Cursor::not_allowed})
        ids.push_back(win32_cursor_resource(c));
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    // Windows has no open hand: it shares the pointing hand.
    REQUIRE(win32_cursor_resource(Cursor::open_hand) ==
            win32_cursor_resource(Cursor::pointing_hand));
}
