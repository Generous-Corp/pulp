// Win32 message values to pulp::native input values. Pure tables with no
// Windows headers, so they build on every platform. The numbers are the
// documented VK_*, MK_* and IDC_* values.

#include <pulp/native/input.hpp>

namespace pulp::native {

Key key_for_win32_vk(std::uint32_t vk, bool extended) noexcept {
    if (vk >= 0x41 && vk <= 0x5a) // 'A'..'Z'
        return static_cast<Key>(static_cast<std::uint16_t>(Key::a) + (vk - 0x41));
    if (vk >= 0x30 && vk <= 0x39) // '0'..'9'
        return static_cast<Key>(static_cast<std::uint16_t>(Key::digit_0) + (vk - 0x30));
    if (vk >= 0x70 && vk <= 0x87) // VK_F1..VK_F24
        return static_cast<Key>(static_cast<std::uint16_t>(Key::f1) + (vk - 0x70));
    if (vk >= 0x60 && vk <= 0x69) // VK_NUMPAD0..9
        return static_cast<Key>(static_cast<std::uint16_t>(Key::keypad_0) + (vk - 0x60));
    switch (vk) {
    case 0x20:
        return Key::space;
    case 0x0d:
        return extended ? Key::keypad_enter : Key::enter;
    case 0x09:
        return Key::tab;
    case 0x08:
        return Key::backspace;
    case 0x1b:
        return Key::escape;
    case 0x2e:
        return Key::delete_forward;
    case 0x25:
        return Key::left;
    case 0x27:
        return Key::right;
    case 0x26:
        return Key::up;
    case 0x28:
        return Key::down;
    case 0x24:
        return Key::home;
    case 0x23:
        return Key::end;
    case 0x21:
        return Key::page_up;
    case 0x22:
        return Key::page_down;
    case 0x2f: // VK_HELP
        return Key::help;
    case 0x6a:
        return Key::keypad_multiply;
    case 0x6b:
        return Key::keypad_plus;
    case 0x6d:
        return Key::keypad_minus;
    case 0x6e:
        return Key::keypad_decimal;
    case 0x6f:
        return Key::keypad_divide;
    case 0xba: // VK_OEM_1
        return Key::semicolon;
    case 0xbb: // VK_OEM_PLUS
        return Key::equal;
    case 0xbc:
        return Key::comma;
    case 0xbd: // VK_OEM_MINUS
        return Key::minus;
    case 0xbe:
        return Key::period;
    case 0xbf:
        return Key::slash;
    case 0xc0:
        return Key::grave;
    case 0xdb:
        return Key::left_bracket;
    case 0xdc:
        return Key::backslash;
    case 0xdd:
        return Key::right_bracket;
    case 0xde:
        return Key::quote;
    case 0x10: // VK_SHIFT, before the side is known
    case 0xa0:
        return Key::shift_left;
    case 0xa1:
        return Key::shift_right;
    case 0x11:
    case 0xa2:
        return Key::control_left;
    case 0xa3:
        return Key::control_right;
    case 0x12:
    case 0xa4:
        return Key::option_left;
    case 0xa5:
        return Key::option_right;
    case 0x5b:
        return Key::command_left;
    case 0x5c:
        return Key::command_right;
    case 0x14:
        return Key::caps_lock;
    case 0x5d:
        return Key::context_menu;
    case 0xaf:
        return Key::volume_up;
    case 0xae:
        return Key::volume_down;
    case 0xad:
        return Key::mute;
    default:
        return Key::unknown;
    }
}

std::uint32_t modifiers_from_win32_keys(bool shift, bool control, bool alt, bool windows,
                                        bool caps_lock) noexcept {
    std::uint32_t m = 0;
    if (shift)
        m |= modifier_shift;
    if (control)
        m |= modifier_control;
    if (alt)
        m |= modifier_option;
    if (windows)
        m |= modifier_command;
    if (caps_lock)
        m |= modifier_caps_lock;
    return m;
}

std::uint32_t buttons_held_from_win32_mk(unsigned mk) noexcept {
    std::uint32_t held = 0;
    if (mk & 0x0001) // MK_LBUTTON
        held |= 1u << 0;
    if (mk & 0x0002) // MK_RBUTTON
        held |= 1u << 1;
    if (mk & 0x0010) // MK_MBUTTON
        held |= 1u << 2;
    if (mk & 0x0020) // MK_XBUTTON1
        held |= 1u << 3;
    if (mk & 0x0040) // MK_XBUTTON2
        held |= 1u << 4;
    return held;
}

double win32_wheel_lines(int delta, bool horizontal) noexcept {
    const double lines = delta / 120.0;
    return horizontal ? -lines : lines;
}

unsigned win32_cursor_resource(Cursor cursor) noexcept {
    switch (cursor) {
    case Cursor::arrow:
        return 32512; // IDC_ARROW
    case Cursor::pointing_hand:
    case Cursor::open_hand:
        return 32649; // IDC_HAND
    case Cursor::closed_hand:
        return 32646; // IDC_SIZEALL
    case Cursor::ibeam:
        return 32513; // IDC_IBEAM
    case Cursor::crosshair:
        return 32515; // IDC_CROSS
    case Cursor::busy:
        return 32514; // IDC_WAIT
    case Cursor::resize_horizontal:
        return 32644; // IDC_SIZEWE
    case Cursor::resize_vertical:
        return 32645; // IDC_SIZENS
    case Cursor::resize_diagonal_nesw:
        return 32643; // IDC_SIZENESW
    case Cursor::resize_diagonal_nwse:
        return 32642; // IDC_SIZENWSE
    case Cursor::not_allowed:
        return 32648; // IDC_NO
    }
    return 32512;
}

} // namespace pulp::native
