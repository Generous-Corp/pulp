// X11 protocol values to pulp::native input values. Pure tables with no X
// headers, so they build on every platform. The numbers are the protocol's
// (X11/keysymdef.h, X.h): a static_assert in gl_surface_x11.cpp checks the
// ones it relies on against the real headers.

#include <pulp/native/input.hpp>
#include <pulp/native/surface.hpp>

#include <cstdlib>
#include <string>

namespace pulp::native {

namespace {

constexpr unsigned kShiftMask = 1u << 0;
constexpr unsigned kLockMask = 1u << 1;
constexpr unsigned kControlMask = 1u << 2;
constexpr unsigned kMod1Mask = 1u << 3;
constexpr unsigned kMod4Mask = 1u << 6;
constexpr unsigned kButton1Mask = 1u << 8;
constexpr unsigned kButton2Mask = 1u << 9;
constexpr unsigned kButton3Mask = 1u << 10;
constexpr unsigned kButton4Mask = 1u << 11;
constexpr unsigned kButton5Mask = 1u << 12;

Key letter_key(std::uint32_t lower) {
    return static_cast<Key>(static_cast<std::uint16_t>(Key::a) + (lower - 'a'));
}

} // namespace

Key key_for_x11_keysym(std::uint32_t k) noexcept {
    if (k >= 'a' && k <= 'z')
        return letter_key(k);
    if (k >= 'A' && k <= 'Z')
        return letter_key(k - 'A' + 'a');
    if (k >= '0' && k <= '9')
        return static_cast<Key>(static_cast<std::uint16_t>(Key::digit_0) + (k - '0'));
    if (k >= 0xffbe && k <= 0xffd5) // XK_F1 .. XK_F24
        return static_cast<Key>(static_cast<std::uint16_t>(Key::f1) + (k - 0xffbe));
    if (k >= 0xffb0 && k <= 0xffb9) // XK_KP_0 .. XK_KP_9
        return static_cast<Key>(static_cast<std::uint16_t>(Key::keypad_0) + (k - 0xffb0));
    switch (k) {
    case 0x0020:
        return Key::space;
    case 0xff0d:
        return Key::enter;
    case 0xff09:
    case 0xfe20: // XK_ISO_Left_Tab, Shift+Tab
        return Key::tab;
    case 0xff08:
        return Key::backspace;
    case 0xff1b:
        return Key::escape;
    case 0xffff:
        return Key::delete_forward;
    case 0xff51:
        return Key::left;
    case 0xff53:
        return Key::right;
    case 0xff52:
        return Key::up;
    case 0xff54:
        return Key::down;
    case 0xff50:
        return Key::home;
    case 0xff57:
        return Key::end;
    case 0xff55:
        return Key::page_up;
    case 0xff56:
        return Key::page_down;
    case 0xff6a:
        return Key::help;
    // Keypad keys as the server reports them with Num Lock off.
    case 0xff9e: // KP_Insert
        return Key::keypad_0;
    case 0xff9c: // KP_End
        return Key::keypad_1;
    case 0xff99: // KP_Down
        return Key::keypad_2;
    case 0xff9b: // KP_Page_Down
        return Key::keypad_3;
    case 0xff96: // KP_Left
        return Key::keypad_4;
    case 0xff9d: // KP_Begin
        return Key::keypad_5;
    case 0xff98: // KP_Right
        return Key::keypad_6;
    case 0xff95: // KP_Home
        return Key::keypad_7;
    case 0xff97: // KP_Up
        return Key::keypad_8;
    case 0xff9a: // KP_Page_Up
        return Key::keypad_9;
    case 0xff9f: // KP_Delete
    case 0xffae: // KP_Decimal
        return Key::keypad_decimal;
    case 0xffab:
        return Key::keypad_plus;
    case 0xffad:
        return Key::keypad_minus;
    case 0xffaa:
        return Key::keypad_multiply;
    case 0xffaf:
        return Key::keypad_divide;
    case 0xffbd:
        return Key::keypad_equals;
    case 0xff8d:
        return Key::keypad_enter;
    case 0xff0b: // XK_Clear
        return Key::keypad_clear;
    case 0xffac: // KP_Separator
        return Key::keypad_comma;
    case 0x002d:
        return Key::minus;
    case 0x003d:
        return Key::equal;
    case 0x005b:
        return Key::left_bracket;
    case 0x005d:
        return Key::right_bracket;
    case 0x005c:
        return Key::backslash;
    case 0x003b:
        return Key::semicolon;
    case 0x0027:
        return Key::quote;
    case 0x0060:
        return Key::grave;
    case 0x002c:
        return Key::comma;
    case 0x002e:
        return Key::period;
    case 0x002f:
        return Key::slash;
    case 0x00a7:
        return Key::section;
    case 0x00a5:
        return Key::jis_yen;
    case 0xffe1:
        return Key::shift_left;
    case 0xffe2:
        return Key::shift_right;
    case 0xffe3:
        return Key::control_left;
    case 0xffe4:
        return Key::control_right;
    case 0xffe9:
        return Key::option_left;
    case 0xffea:
        return Key::option_right;
    case 0xffeb:
        return Key::command_left;
    case 0xffec:
        return Key::command_right;
    case 0xffe5:
        return Key::caps_lock;
    case 0xff67:
        return Key::context_menu;
    case 0x1008ff13:
        return Key::volume_up;
    case 0x1008ff11:
        return Key::volume_down;
    case 0x1008ff12:
        return Key::mute;
    default:
        return Key::unknown;
    }
}

std::uint32_t modifiers_from_x11_state(unsigned state) noexcept {
    std::uint32_t m = 0;
    if (state & kShiftMask)
        m |= modifier_shift;
    if (state & kLockMask)
        m |= modifier_caps_lock;
    if (state & kControlMask)
        m |= modifier_control;
    if (state & kMod1Mask)
        m |= modifier_option;
    if (state & kMod4Mask)
        m |= modifier_command;
    return m;
}

std::uint32_t buttons_held_from_x11_state(unsigned state) noexcept {
    std::uint32_t held = 0;
    if (state & kButton1Mask)
        held |= 1u << 0;
    if (state & kButton3Mask)
        held |= 1u << 1;
    if (state & kButton2Mask)
        held |= 1u << 2;
    // Buttons 4 and 5 are wheel motion, never held.
    (void)kButton4Mask;
    (void)kButton5Mask;
    return held;
}

PointerButton pointer_button_for_x11(unsigned button, int& number) noexcept {
    switch (button) {
    case 1:
        number = 0;
        return PointerButton::left;
    case 3:
        number = 1;
        return PointerButton::right;
    case 2:
        number = 2;
        return PointerButton::middle;
    default:
        break;
    }
    if (button >= 8) {
        number = static_cast<int>(button) - 5; // 8 -> 3, 9 -> 4
        return PointerButton::other;
    }
    number = -1;
    return PointerButton::none;
}

bool wheel_delta_for_x11_button(unsigned button, double& dx, double& dy) noexcept {
    dx = 0.0;
    dy = 0.0;
    switch (button) {
    case 4:
        dy = 1.0;
        return true;
    case 5:
        dy = -1.0;
        return true;
    case 6:
        dx = 1.0;
        return true;
    case 7:
        dx = -1.0;
        return true;
    default:
        return false;
    }
}

double scale_from_xft_dpi(const std::string& resources) {
    // Lines of "Name:<whitespace>value". The last Xft.dpi line wins, as in the
    // X resource database.
    double dpi = 0.0;
    std::size_t pos = 0;
    while (pos < resources.size()) {
        std::size_t end = resources.find('\n', pos);
        if (end == std::string::npos)
            end = resources.size();
        const std::string line = resources.substr(pos, end - pos);
        pos = end + 1;
        static const std::string kName = "Xft.dpi:";
        if (line.compare(0, kName.size(), kName) != 0)
            continue;
        const char* value = line.c_str() + kName.size();
        char* parsed_end = nullptr;
        const double parsed = std::strtod(value, &parsed_end);
        if (parsed_end != value && parsed > 0.0)
            dpi = parsed;
    }
    return dpi > 0.0 ? dpi / 96.0 : 0.0;
}

} // namespace pulp::native
