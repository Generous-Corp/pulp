// The X11 translation tables of pulp::native: keysyms, state masks and button
// numbers to Pulp's input types. Pure, so they run on every platform.

#include <pulp/native/input.hpp>
#include <pulp/native/surface.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp::native;

TEST_CASE("X11 keysyms map to physical keys", "[native][input][x11]") {
    REQUIRE(key_for_x11_keysym(0x61) == Key::a);
    REQUIRE(key_for_x11_keysym(0x5a) == Key::z); // an upper-case letter is its key
    REQUIRE(key_for_x11_keysym(0x35) == Key::digit_5);
    REQUIRE(key_for_x11_keysym(0x20) == Key::space);
    REQUIRE(key_for_x11_keysym(0xff0d) == Key::enter);
    REQUIRE(key_for_x11_keysym(0xff1b) == Key::escape);
    REQUIRE(key_for_x11_keysym(0xfe20) == Key::tab); // ISO_Left_Tab, Shift+Tab
    REQUIRE(key_for_x11_keysym(0xff51) == Key::left);
    REQUIRE(key_for_x11_keysym(0xffbe) == Key::f1);
    REQUIRE(key_for_x11_keysym(0xffd5) == Key::f24);
    // The keypad with Num Lock on and off is the same key.
    REQUIRE(key_for_x11_keysym(0xffb7) == Key::keypad_7);
    REQUIRE(key_for_x11_keysym(0xff95) == Key::keypad_7);
    REQUIRE(key_for_x11_keysym(0xffb0) == Key::keypad_0);
    REQUIRE(key_for_x11_keysym(0xff9e) == Key::keypad_0);
    REQUIRE(key_for_x11_keysym(0xff8d) == Key::keypad_enter);
    REQUIRE(key_for_x11_keysym(0xffe1) == Key::shift_left);
    REQUIRE(key_for_x11_keysym(0xffe4) == Key::control_right);
    REQUIRE(key_for_x11_keysym(0xffeb) == Key::command_left);
    REQUIRE(key_for_x11_keysym(0xffe9) == Key::option_left);
    REQUIRE(key_for_x11_keysym(0x0) == Key::unknown);
    REQUIRE(key_for_x11_keysym(0x10000000) == Key::unknown);
}

TEST_CASE("X11 state masks map to modifiers and held buttons", "[native][input][x11]") {
    REQUIRE(modifiers_from_x11_state(0) == 0u);
    REQUIRE(modifiers_from_x11_state(1u << 0) == modifier_shift);
    REQUIRE(modifiers_from_x11_state(1u << 1) == modifier_caps_lock);
    REQUIRE(modifiers_from_x11_state(1u << 2) == modifier_control);
    REQUIRE(modifiers_from_x11_state(1u << 3) == modifier_option);
    REQUIRE(modifiers_from_x11_state(1u << 6) == modifier_command);
    REQUIRE(modifiers_from_x11_state((1u << 0) | (1u << 2)) == (modifier_shift | modifier_control));
    // Button bits are not modifiers.
    REQUIRE(modifiers_from_x11_state(1u << 8) == 0u);

    // Button1 is bit 0, Button3 (right) is bit 1, Button2 (middle) is bit 2.
    REQUIRE(buttons_held_from_x11_state(1u << 8) == 1u);
    REQUIRE(buttons_held_from_x11_state(1u << 10) == 2u);
    REQUIRE(buttons_held_from_x11_state(1u << 9) == 4u);
    REQUIRE(buttons_held_from_x11_state((1u << 8) | (1u << 10)) == 3u);
    // The wheel buttons are never held.
    REQUIRE(buttons_held_from_x11_state((1u << 11) | (1u << 12)) == 0u);
}

TEST_CASE("X11 button numbers map to pointer buttons and wheel motion", "[native][input][x11]") {
    int number = 99;
    REQUIRE(pointer_button_for_x11(1, number) == PointerButton::left);
    REQUIRE(number == 0);
    REQUIRE(pointer_button_for_x11(3, number) == PointerButton::right);
    REQUIRE(number == 1);
    REQUIRE(pointer_button_for_x11(2, number) == PointerButton::middle);
    REQUIRE(number == 2);
    REQUIRE(pointer_button_for_x11(8, number) == PointerButton::other);
    REQUIRE(number == 3);
    REQUIRE(pointer_button_for_x11(9, number) == PointerButton::other);
    REQUIRE(number == 4);
    REQUIRE(pointer_button_for_x11(4, number) == PointerButton::none);
    REQUIRE(number == -1);

    double dx = 9;
    double dy = 9;
    REQUIRE(wheel_delta_for_x11_button(4, dx, dy));
    REQUIRE((dx == 0.0 && dy == 1.0)); // wheel up moves content down
    REQUIRE(wheel_delta_for_x11_button(5, dx, dy));
    REQUIRE((dx == 0.0 && dy == -1.0));
    REQUIRE(wheel_delta_for_x11_button(6, dx, dy));
    REQUIRE((dx == 1.0 && dy == 0.0));
    REQUIRE(wheel_delta_for_x11_button(7, dx, dy));
    REQUIRE((dx == -1.0 && dy == 0.0));
    REQUIRE_FALSE(wheel_delta_for_x11_button(1, dx, dy));
    REQUIRE((dx == 0.0 && dy == 0.0));
    REQUIRE_FALSE(wheel_delta_for_x11_button(8, dx, dy));
}

TEST_CASE("the display scale comes from Xft.dpi over 96 text", "[native][surface][x11]") {
    REQUIRE(scale_from_xft_dpi("Xft.dpi:\t192\n") == 2.0);
    REQUIRE(scale_from_xft_dpi("Xft.antialias:\t1\nXft.dpi:\t144\nXft.hinting:\t1\n") == 1.5);
    REQUIRE(scale_from_xft_dpi("Xft.dpi: 96") == 1.0);
    REQUIRE(scale_from_xft_dpi("Xft.dpi:\t96\nXft.dpi:\t192\n") == 2.0); // the last wins
    REQUIRE(scale_from_xft_dpi("") == 0.0);
    REQUIRE(scale_from_xft_dpi("Xft.antialias:\t1\n") == 0.0);
    REQUIRE(scale_from_xft_dpi("Xft.dpi:\tabc\n") == 0.0);
    REQUIRE(scale_from_xft_dpi("Xft.dpi:\t-96\n") == 0.0);
    REQUIRE(scale_from_xft_dpi("Other.Xft.dpi:\t192\n") == 0.0);
}
