#pragma once

/// @file input.hpp
/// Neutral input events for a native-surface editor, and the macOS translation
/// from `NSEvent`.
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

/// The `NSCursor*` for `cursor`. A cursor the running system has no public
/// equivalent for falls back to the arrow; `cursor_is_native` says which.
void* ns_cursor_for(Cursor cursor);
bool cursor_is_native(Cursor cursor) noexcept;

} // namespace pulp::native
