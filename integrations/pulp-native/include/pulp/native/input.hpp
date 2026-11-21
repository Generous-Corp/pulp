#pragma once

/// @file input.hpp
/// Neutral input events for a native-surface editor, the macOS translation
/// from `NSEvent`, and the X11 translation tables.
///
/// The events name no AppKit type. Positions are in logical points with the
/// origin at the surface's top-left, so a client converts to pixels with the
/// surface's backing scale. Positions outside the surface (a drag that left
/// it) are reported as they are, negative or past its size.
///
/// The translation functions take `NSEvent*` and `NSView*` as `void*` so C++
/// translation units can include this header. They run on the main thread,
/// where AppKit delivers events.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::native {

/// Modifier bits, set on every event.
enum Modifier : std::uint32_t {
    modifier_shift = 1u << 0,
    modifier_control = 1u << 1,
    modifier_option = 1u << 2,
    modifier_command = 1u << 3,
    modifier_caps_lock = 1u << 4,
    modifier_function = 1u << 5,
    modifier_numeric_pad = 1u << 6,
};

enum class PointerButton : std::uint8_t {
    none = 0,
    left = 1,
    right = 2,
    middle = 3,
    /// A further button; its number is in `PointerEvent::button_number`.
    other = 4,
};

enum class PointerKind : std::uint8_t {
    down = 0,
    up = 1,
    /// Movement with no button held.
    move = 2,
    /// Movement with a button held, delivered to the surface the press began
    /// in even after the pointer has left it.
    drag = 3,
    enter = 4,
    leave = 5,
};

struct PointerEvent {
    PointerKind kind = PointerKind::move;
    double x = 0.0;
    double y = 0.0;
    PointerButton button = PointerButton::none;
    /// The platform's button number: 0 left, 1 right, 2 middle, 3 and up for
    /// further buttons.
    int button_number = -1;
    /// Bit n set while button number n is held.
    std::uint32_t buttons_held = 0;
    std::uint32_t modifiers = 0;
    int click_count = 0;
    /// Seconds since system start-up, as the platform stamps events.
    double timestamp = 0.0;
};

/// The gesture or momentum phase of a scroll, as the trackpad reports it.
enum class ScrollPhase : std::uint8_t {
    none = 0,
    began = 1,
    changed = 2,
    ended = 3,
    cancelled = 4,
    may_begin = 5,
};

struct WheelEvent {
    double x = 0.0;
    double y = 0.0;
    /// Content motion: positive `dy` moves content down. The user's natural
    /// scrolling preference is already applied, exactly once.
    double dx = 0.0;
    double dy = 0.0;
    /// True for a trackpad or Magic Mouse (deltas in points); false for a
    /// wheel with detents (deltas in lines).
    bool precise = false;
    /// The platform's unscaled per-event deltas, in lines.
    double raw_dx = 0.0;
    double raw_dy = 0.0;
    /// True when the device's direction is inverted (natural scrolling on).
    bool direction_inverted = false;
    ScrollPhase phase = ScrollPhase::none;
    ScrollPhase momentum_phase = ScrollPhase::none;
    std::uint32_t modifiers = 0;
    double timestamp = 0.0;
};

/// A physical key, independent of the keyboard layout. Values are stable;
/// new keys are only ever appended.
enum class Key : std::uint16_t {
    unknown = 0,
    a,
    b,
    c,
    d,
    e,
    f,
    g,
    h,
    i,
    j,
    k,
    l,
    m,
    n,
    o,
    p,
    q,
    r,
    s,
    t,
    u,
    v,
    w,
    x,
    y,
    z,
    digit_0,
    digit_1,
    digit_2,
    digit_3,
    digit_4,
    digit_5,
    digit_6,
    digit_7,
    digit_8,
    digit_9,
    space,
    enter,
    tab,
    backspace,
    escape,
    delete_forward,
    left,
    right,
    up,
    down,
    home,
    end,
    page_up,
    page_down,
    help,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    f13,
    f14,
    f15,
    f16,
    f17,
    f18,
    f19,
    f20,
    f21,
    f22,
    f23,
    f24,
    keypad_0,
    keypad_1,
    keypad_2,
    keypad_3,
    keypad_4,
    keypad_5,
    keypad_6,
    keypad_7,
    keypad_8,
    keypad_9,
    keypad_decimal,
    keypad_plus,
    keypad_minus,
    keypad_multiply,
    keypad_divide,
    keypad_equals,
    keypad_enter,
    keypad_clear,
    keypad_comma,
    minus,
    equal,
    left_bracket,
    right_bracket,
    backslash,
    semicolon,
    quote,
    grave,
    comma,
    period,
    slash,
    section,
    shift_left,
    shift_right,
    control_left,
    control_right,
    option_left,
    option_right,
    command_left,
    command_right,
    caps_lock,
    function,
    context_menu,
    volume_up,
    volume_down,
    mute,
    jis_yen,
    jis_underscore,
    jis_eisu,
    jis_kana,
};

enum class KeyKind : std::uint8_t {
    down = 0,
    up = 1,
};

struct KeyEvent {
    KeyKind kind = KeyKind::down;
    Key key = Key::unknown;
    /// The platform's key code (a macOS virtual key code).
    std::uint16_t native_code = 0;
    /// An auto-repeat of a held key.
    bool repeat = false;
    std::uint32_t modifiers = 0;
    /// The characters the key produced with its modifiers, UTF-8; empty for a
    /// key that produces none. Composed text (dead keys, input methods)
    /// arrives as text input, not here.
    std::string characters;
    double timestamp = 0.0;
};

/// Named cursors a client can set over the surface.
enum class Cursor : std::uint8_t {
    arrow = 0,
    pointing_hand,
    open_hand,
    closed_hand,
    ibeam,
    crosshair,
    busy,
    resize_horizontal,
    resize_vertical,
    resize_diagonal_nesw,
    resize_diagonal_nwse,
    not_allowed,
};

/// Where a key goes first.
enum class KeyRoute : std::uint8_t {
    /// The host keeps it (Space for transport, its shortcuts, Musical Typing).
    host = 0,
    /// Offer it to the client first; the host gets it if the client declines.
    client_first = 1,
};

/// What the client holds the keyboard for.
enum class KeyboardFocus : std::uint8_t {
    /// The host keeps the keyboard.
    none = 0,
    /// Keys without text entry (a focused control, an editor shortcut): the
    /// client is offered every key first, and a key it declines goes on to
    /// the host, so Space still reaches the DAW's transport.
    keys = 1,
    /// Text entry: the client is offered every key first, and a key it
    /// declines becomes text input instead of reaching the host.
    text = 2,
};

/// The routing rule. While the client holds the keyboard, for keys or for
/// text, every key, Space and Command chords included, is offered to the
/// client first. Otherwise the host keeps every key, so an editor that only
/// shows controls never takes Space or a shortcut from the DAW.
KeyRoute route_key(const KeyEvent& event, KeyboardFocus focus) noexcept;
/// The same rule with text-entry focus as a flag (`true` is
/// `KeyboardFocus::text`).
KeyRoute route_key(const KeyEvent& event, bool client_holds_text_focus) noexcept;

/// The key for a macOS virtual key code, or `Key::unknown`.
Key key_for_native_code(std::uint16_t code) noexcept;
/// A stable lower-case name for `key` ("f13", "keypad_enter"), for logs and
/// tests.
std::string_view key_name(Key key) noexcept;

// ── macOS translation ────────────────────────────────────────────────────────

/// Modifier bits from `NSEventModifierFlags`.
std::uint32_t modifiers_from_ns_flags(std::uint64_t flags) noexcept;

/// A mouse event (`NSEvent*`) in `view`'s (`NSView*`) logical coordinates,
/// origin at its top-left. Nullopt for an event that is not a mouse event.
std::optional<PointerEvent> translate_pointer(void* ns_event, void* ns_view);
/// A scroll-wheel event, or nullopt.
std::optional<WheelEvent> translate_wheel(void* ns_event, void* ns_view);
/// A key-down or key-up event, or nullopt.
std::optional<KeyEvent> translate_key(void* ns_event);
/// A flags-changed event: the modifier key that moved, as a down or up of
/// that key with its side (left or right shift). Caps Lock reports down when
/// it turns on and up when it turns off.
std::optional<KeyEvent> translate_flags_changed(void* ns_event);

// ── X11 translation ──────────────────────────────────────────────────────────
//
// Pure tables over X11 protocol values (keysyms, state masks, button numbers).
// They need no X headers, so they are built and testable on every platform;
// the X11 surface backend feeds them what the server sends.

/// The key for the X keysym a key produces at its first level (the unshifted
/// symbol: lower-case letters, digits, keypad keys with and without Num Lock),
/// or `Key::unknown`. An upper-case letter keysym maps to its letter.
Key key_for_x11_keysym(std::uint32_t keysym) noexcept;

/// Modifier bits from an X event `state`: Shift, Lock (Caps Lock), Control,
/// Mod1 (Alt, as `modifier_option`) and Mod4 (Super, as `modifier_command`).
std::uint32_t modifiers_from_x11_state(unsigned state) noexcept;

/// `PointerEvent::buttons_held` from the button bits of an X event `state`
/// (Button1Mask to Button5Mask): bit 0 left, bit 1 right, bit 2 middle, and
/// bit n for button number n of a further button.
std::uint32_t buttons_held_from_x11_state(unsigned state) noexcept;

/// The pointer button for X button number 1 (left), 2 (middle), 3 (right) or
/// 8 and up (further buttons, numbered from 3). Buttons 4 to 7 are wheel
/// motion and map to `PointerButton::none`; `number` receives the Pulp button
/// number (0 left, 1 right, 2 middle), or -1 for a wheel button.
PointerButton pointer_button_for_x11(unsigned button, int& number) noexcept;

/// Wheel motion for X button 4 (up), 5 (down), 6 (left) or 7 (right), in
/// lines, with the sign convention of `WheelEvent` (positive `dy` moves
/// content down, positive `dx` moves content right). False for any other
/// button.
bool wheel_delta_for_x11_button(unsigned button, double& dx, double& dy) noexcept;

/// The X11 core cursor-font shape (`XC_*`) for `cursor`: `XC_left_ptr` for the
/// arrow, `XC_hand2` for the pointing hand, `XC_xterm` for the I-beam, and so
/// on. Pure; the X11 backend passes it to `XCreateFontCursor`.
unsigned x11_cursor_shape(Cursor cursor) noexcept;

// ── Win32 translation ────────────────────────────────────────────────────────
//
// Pure tables over Win32 message values, with no Windows headers, so they are
// built and tested on every platform; the Win32 surface backend feeds them what
// its window procedure receives.

/// The key for a virtual-key code (`VK_*`); `extended` is the extended-key bit
/// of `lParam` (bit 24), which tells the Enter on the keypad from the main one.
/// `Key::unknown` for a code with no key.
///
/// Win32 reports Shift, Ctrl and Alt as the generic `VK_SHIFT`, `VK_CONTROL`
/// and `VK_MENU` for both sides, so the side comes from `lParam`: the scan code
/// (bits 16-23, 0x36 for right Shift) for Shift, and the extended bit for Ctrl
/// and Alt. Pass `scan_code` for those; the side-specific `VK_L*` / `VK_R*`
/// codes ignore it.
Key key_for_win32_vk(std::uint32_t vk, bool extended, std::uint32_t scan_code = 0) noexcept;

/// Which key-downs the client took, so the `WM_CHAR` they generate can be
/// dropped and nothing else is. A character is tied to its key by the scan code
/// and extended bit of `lParam`, so a taken key that makes no character (an
/// arrow, a shortcut) never swallows a later, unrelated one.
class Win32TakenKeys {
public:
    /// The scan-code identity of a key message's `lParam`: the scan code with
    /// the extended bit above it.
    static std::uint32_t id_from_lparam(std::uint64_t lparam) noexcept {
        return static_cast<std::uint32_t>((lparam >> 16) & 0xff) |
               (static_cast<std::uint32_t>((lparam >> 24) & 1) << 8);
    }
    /// The client took the key-down with this id (its repeats too).
    void taken(std::uint32_t id);
    /// The key was released; its characters are over.
    void released(std::uint32_t id);
    /// Whether a `WM_CHAR` with this id comes from a taken key.
    bool suppresses_char(std::uint32_t id) const noexcept;
    /// Forget everything: focus changed or the window went away, so releases
    /// may never arrive.
    void clear() noexcept {
        ids_.clear();
    }

private:
    std::vector<std::uint32_t> ids_;
};

/// Modifier bits from the states of the modifier keys.
std::uint32_t modifiers_from_win32_keys(bool shift, bool control, bool alt, bool windows,
                                        bool caps_lock) noexcept;

/// `PointerEvent::buttons_held` from the `MK_*` bits of a mouse message's
/// `wParam`: bit 0 left, bit 1 right, bit 2 middle, and bits 3 and 4 for the
/// two extra buttons.
std::uint32_t buttons_held_from_win32_mk(unsigned mk) noexcept;

/// Wheel lines for a `WM_MOUSEWHEEL` / `WM_MOUSEHWHEEL` delta (`WHEEL_DELTA`
/// is 120 per detent), with the sign convention of `WheelEvent`: a vertical
/// wheel turned away from the user (positive delta) moves content down. A
/// horizontal delta is positive when the wheel tilts right, which moves content
/// left, so pass `horizontal` to flip it.
double win32_wheel_lines(int delta, bool horizontal) noexcept;

/// The system cursor resource id (`IDC_*`) for `cursor`; the Win32 backend
/// passes it to `LoadCursor`. Windows has no open-hand or grab cursor: the
/// pointing hand stands for the open hand and the four-way arrow for the closed.
unsigned win32_cursor_resource(Cursor cursor) noexcept;

/// The `NSCursor*` for `cursor`. A cursor the running system has no public
/// equivalent for falls back to the arrow; `cursor_is_native` says which.
void* ns_cursor_for(Cursor cursor);
bool cursor_is_native(Cursor cursor) noexcept;

} // namespace pulp::native
