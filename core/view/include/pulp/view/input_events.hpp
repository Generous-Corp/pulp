#pragma once

/// @file input_events.hpp
/// Input event types with modifier keys and pointer IDs.
/// Replaces the bare on_mouse_down(Point) API with rich event objects.

#include <pulp/view/geometry.hpp>
#include <cstdint>
#include <string>

namespace pulp::view {

// ── Modifier flags ───────────────────────────────────────────────────────

enum Modifier : uint16_t {
    kModNone    = 0,
    kModShift   = 1 << 0,
    kModCtrl    = 1 << 1,   ///< Control key (Ctrl on Windows/Linux)
    kModAlt     = 1 << 2,   ///< Alt / Option
    kModMeta    = 1 << 3,   ///< Windows key / Command key
    kModCmd     = 1 << 4,   ///< Command (macOS) — alias for platform primary modifier
};

/// True if the primary action modifier is held (Cmd on macOS, Ctrl on Windows/Linux).
inline bool is_main_modifier(uint16_t mods) {
#ifdef __APPLE__
    return (mods & kModCmd) != 0;
#else
    return (mods & kModCtrl) != 0;
#endif
}

// ── Key codes ────────────────────────────────────────────────────────────

enum class KeyCode : int {
    unknown = 0,

    // Letters
    a = 'a', b = 'b', c = 'c', d = 'd', e = 'e', f = 'f', g = 'g', h = 'h',
    i = 'i', j = 'j', k = 'k', l = 'l', m = 'm', n = 'n', o = 'o', p = 'p',
    q = 'q', r = 'r', s = 's', t = 't', u = 'u', v = 'v', w = 'w', x = 'x',
    y = 'y', z = 'z',

    // Numbers
    num0 = '0', num1 = '1', num2 = '2', num3 = '3', num4 = '4',
    num5 = '5', num6 = '6', num7 = '7', num8 = '8', num9 = '9',

    // Navigation
    left = 256, right, up, down,
    home, end_, page_up, page_down,

    // Editing
    backspace = 270, delete_, tab, enter, escape,
    space = ' ',

    // Function keys
    f1 = 290, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};

// ── Mouse / pointer event ────────────────────────────────────────────────

/// Mouse button identifiers.
enum class MouseButton : uint8_t {
    none = 0,
    left = 1,
    right = 2,
    middle = 3,
};

/// Pointer device type (W3C PointerEvent.pointerType).
enum class PointerType : uint8_t {
    mouse = 0,   ///< Mouse cursor (default)
    touch = 1,   ///< Direct touch (finger)
    pen   = 2,   ///< Stylus / Apple Pencil
};

/// Rich mouse/pointer event with position, modifiers, button state, pointer ID,
/// and stylus properties. Unified for mouse, touch, and pen input.
struct MouseEvent {
    Point position;              ///< Position in local view coordinates
    Point window_position;       ///< Position in window coordinates
    MouseButton button = MouseButton::left;
    uint16_t modifiers = 0;      ///< Bitfield of Modifier flags
    int pointer_id = 0;          ///< 0 = primary, >0 = additional touches (multi-touch)
    int click_count = 1;         ///< 1=single, 2=double, 3=triple click
    bool is_down = false;        ///< True for mouse-down / touch-began events
    bool is_cancelled = false;   ///< True for touchesCancelled (→ pointercancel)
    bool is_wheel = false;       ///< True for scroll wheel events
    float scroll_delta_x = 0;   ///< Horizontal scroll delta (positive = right)
    float scroll_delta_y = 0;   ///< Vertical scroll delta (positive = down)

    // ── Pointer / stylus properties (W3C PointerEvent) ──────────────
    PointerType pointer_type = PointerType::mouse;
    float pressure = 0.5f;       ///< 0.0–1.0 (0.5 = default for mouse)
    float altitude_angle = 0;    ///< Stylus tilt in radians (0 = perpendicular)
    float azimuth_angle = 0;     ///< Stylus rotation in radians (0–2π)

    bool isShiftDown() const  { return (modifiers & kModShift) != 0; }
    bool isCtrlDown() const   { return (modifiers & kModCtrl) != 0; }
    bool isAltDown() const    { return (modifiers & kModAlt) != 0; }
    bool isCmdDown() const    { return (modifiers & kModCmd) != 0; }
    bool isMetaDown() const   { return (modifiers & kModMeta) != 0; }
    bool isMainModifier() const { return is_main_modifier(modifiers); }
    bool isTouch() const      { return pointer_type == PointerType::touch || (modifiers & 0x8000) != 0; }
    bool isPen() const        { return pointer_type == PointerType::pen; }
    bool isWheel() const      { return is_wheel; }
    bool isPrimary() const    { return pointer_id == 0; }

    /// W3C pointerType string
    const char* pointerTypeString() const {
        switch (pointer_type) {
            case PointerType::touch: return "touch";
            case PointerType::pen:   return "pen";
            default:                 return "mouse";
        }
    }
};

// ── Gesture event (pinch / rotate) ──────────────────────────────────────

enum class GesturePhase : uint8_t {
    began = 0,
    changed = 1,
    ended = 2,
    cancelled = 3,
};

/// High-level gesture event for pinch (scale) and rotate.
/// Fired by iOS multi-touch analysis or macOS trackpad magnify/rotate.
struct GestureEvent {
    GesturePhase phase = GesturePhase::began;
    float scale = 1.0f;          ///< Cumulative scale factor (1.0 = no change)
    float rotation = 0.0f;       ///< Cumulative rotation in radians
    float delta_scale = 0.0f;    ///< Scale delta since last event
    float delta_rotation = 0.0f; ///< Rotation delta since last event
    Point position;              ///< Center point of the gesture
};

// ── Key event ────────────────────────────────────────────────────────────

/// Rich key event with key code, modifiers, and repeat state.
struct KeyEvent {
    KeyCode key = KeyCode::unknown;
    uint16_t modifiers = 0;
    bool is_down = true;         ///< True for key-down, false for key-up
    bool is_repeat = false;      ///< True if this is a key repeat

    bool isShiftDown() const  { return (modifiers & kModShift) != 0; }
    bool isCtrlDown() const   { return (modifiers & kModCtrl) != 0; }
    bool isAltDown() const    { return (modifiers & kModAlt) != 0; }
    bool isCmdDown() const    { return (modifiers & kModCmd) != 0; }
    bool isMainModifier() const { return is_main_modifier(modifiers); }
};

// ── Text input event ─────────────────────────────────────────────────────

/// Text input from IME or keyboard composition (separate from KeyEvent).
/// Use this for actual character input, not raw key presses.
struct TextInputEvent {
    std::string text;            ///< UTF-8 encoded text
};

} // namespace pulp::view
