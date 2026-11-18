// The parts of pulp::native input that name no platform event: key names and
// the key routing rule. Built on every platform; the AppKit translation lives
// in input_mac.mm and the X11 one in input_x11.cpp.

#include <pulp/native/input.hpp>

#include <cstddef>
#include <iterator>

namespace pulp::native {

namespace {

constexpr const char* kKeyNames[] = {
    "unknown",
    "a",
    "b",
    "c",
    "d",
    "e",
    "f",
    "g",
    "h",
    "i",
    "j",
    "k",
    "l",
    "m",
    "n",
    "o",
    "p",
    "q",
    "r",
    "s",
    "t",
    "u",
    "v",
    "w",
    "x",
    "y",
    "z",
    "digit_0",
    "digit_1",
    "digit_2",
    "digit_3",
    "digit_4",
    "digit_5",
    "digit_6",
    "digit_7",
    "digit_8",
    "digit_9",
    "space",
    "enter",
    "tab",
    "backspace",
    "escape",
    "delete_forward",
    "left",
    "right",
    "up",
    "down",
    "home",
    "end",
    "page_up",
    "page_down",
    "help",
    "f1",
    "f2",
    "f3",
    "f4",
    "f5",
    "f6",
    "f7",
    "f8",
    "f9",
    "f10",
    "f11",
    "f12",
    "f13",
    "f14",
    "f15",
    "f16",
    "f17",
    "f18",
    "f19",
    "f20",
    "f21",
    "f22",
    "f23",
    "f24",
    "keypad_0",
    "keypad_1",
    "keypad_2",
    "keypad_3",
    "keypad_4",
    "keypad_5",
    "keypad_6",
    "keypad_7",
    "keypad_8",
    "keypad_9",
    "keypad_decimal",
    "keypad_plus",
    "keypad_minus",
    "keypad_multiply",
    "keypad_divide",
    "keypad_equals",
    "keypad_enter",
    "keypad_clear",
    "keypad_comma",
    "minus",
    "equal",
    "left_bracket",
    "right_bracket",
    "backslash",
    "semicolon",
    "quote",
    "grave",
    "comma",
    "period",
    "slash",
    "section",
    "shift_left",
    "shift_right",
    "control_left",
    "control_right",
    "option_left",
    "option_right",
    "command_left",
    "command_right",
    "caps_lock",
    "function",
    "context_menu",
    "volume_up",
    "volume_down",
    "mute",
    "jis_yen",
    "jis_underscore",
    "jis_eisu",
    "jis_kana",
};

} // namespace

KeyRoute route_key(const KeyEvent&, KeyboardFocus focus) noexcept {
    return focus == KeyboardFocus::none ? KeyRoute::host : KeyRoute::client_first;
}

KeyRoute route_key(const KeyEvent& event, bool client_holds_text_focus) noexcept {
    return route_key(event, client_holds_text_focus ? KeyboardFocus::text : KeyboardFocus::none);
}

std::string_view key_name(Key key) noexcept {
    const auto index = static_cast<std::size_t>(key);
    if (index >= std::size(kKeyNames))
        return "unknown";
    return kKeyNames[index];
}

#if !defined(__APPLE__)

// The AppKit translation exists only on macOS; elsewhere these report that
// they translated nothing, so the declarations always link.
Key key_for_native_code(std::uint16_t) noexcept {
    return Key::unknown;
}
std::uint32_t modifiers_from_ns_flags(std::uint64_t) noexcept {
    return 0;
}
std::optional<PointerEvent> translate_pointer(void*, void*) {
    return std::nullopt;
}
std::optional<WheelEvent> translate_wheel(void*, void*) {
    return std::nullopt;
}
std::optional<KeyEvent> translate_key(void*) {
    return std::nullopt;
}
std::optional<KeyEvent> translate_flags_changed(void*) {
    return std::nullopt;
}
void* ns_cursor_for(Cursor) {
    return nullptr;
}
bool cursor_is_native(Cursor) noexcept {
    return false;
}

#endif

} // namespace pulp::native
