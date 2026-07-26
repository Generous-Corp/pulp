#pragma once

// Windows plug-in editor host: pointer-input and surface-lifecycle decisions.
//
// HWND-free logic factored out of
// core/view/platform/win/plugin_view_host_win.cpp. The platform TU keeps the
// Win32 calls (GetKeyState, SetCapture, SetParent, Dawn surface creation);
// this header keeps the decisions those calls feed, so they are testable on
// every platform. That matters here specifically: the required Pulp CI gate is
// macOS, so a `_WIN32`-gated test of this logic would never actually run.
//
// Same split as pulp/view/platform/ns_role_mapping.hpp (Apple) and
// pulp/view/platform/uia_mapping.hpp (Windows accessibility).

#include <pulp/view/geometry.hpp>      // Point
#include <pulp/view/input_events.hpp>  // Modifier flags

#include <cstdint>

namespace pulp::view::win_input {

/// Host-neutral model of the single Win32 capture bracket owned by an editor.
/// Win32 can synchronously re-enter the wndproc from capture calls and view
/// callbacks, so transitions must be visible before those calls happen.
class PointerSession {
public:
    enum class Phase { idle, raw, gesture_candidate, gesture_claimed, terminal };
    struct Terminal {
        uint64_t generation = 0;
        MouseButton button = MouseButton::none;
        bool cancel_gesture = false;
        bool was_claimed = false;
        bool owns_terminal = false;
    };

    bool begin(MouseButton button) noexcept {
        // Terminal is still owned by the outer callback frame. It becomes
        // reusable only after finish_terminal(), so nested button-down cannot
        // overwrite that frame's shared drag target.
        if (phase_ != Phase::idle) return false;
        ++generation_;
        button_ = button;
        terminal_cancel_gesture_ = false;
        terminal_was_claimed_ = false;
        phase_ = button == MouseButton::left ? Phase::gesture_candidate
                                             : Phase::raw;
        return true;
    }
    bool active() const noexcept {
        return phase_ != Phase::idle && phase_ != Phase::terminal;
    }
    bool accepts(MouseButton button) const noexcept {
        return active() && button_ == button;
    }
    bool accepts(uint64_t generation, MouseButton button) const noexcept {
        return generation_ == generation && accepts(button);
    }
    bool gesture_in_flight() const noexcept {
        return phase_ == Phase::gesture_candidate ||
               phase_ == Phase::gesture_claimed;
    }
    bool claimed() const noexcept { return phase_ == Phase::gesture_claimed; }
    MouseButton button() const noexcept { return button_; }
    Phase phase() const noexcept { return phase_; }
    uint64_t generation() const noexcept { return generation_; }
    void mark_claimed() noexcept {
        if (phase_ == Phase::gesture_candidate)
            phase_ = Phase::gesture_claimed;
    }
    Terminal terminalize() noexcept {
        if (phase_ == Phase::terminal) {
            // Cancellation dispatch is edge-triggered. A re-entrant
            // WM_CAPTURECHANGED/CANCELMODE must not feed a second cancellation
            // into the GestureArbiter while the first dispatch still holds its
            // session reference.
            return {generation_, button_, false, terminal_was_claimed_, false};
        }
        if (active()) {
            terminal_cancel_gesture_ = gesture_in_flight();
            terminal_was_claimed_ = claimed();
            phase_ = Phase::terminal;
        }
        Terminal result{generation_, button_, terminal_cancel_gesture_,
                        terminal_was_claimed_, phase_ == Phase::terminal};
        return result;
    }
    void finish_terminal(const Terminal& terminal) noexcept {
        if (!terminal.owns_terminal) return;
        if (phase_ == Phase::terminal &&
            generation_ == terminal.generation) {
            phase_ = Phase::idle;
            button_ = MouseButton::none;
            terminal_cancel_gesture_ = false;
            terminal_was_claimed_ = false;
        }
    }

private:
    Phase phase_ = Phase::idle;
    MouseButton button_ = MouseButton::none;
    uint64_t generation_ = 0;
    bool terminal_cancel_gesture_ = false;
    bool terminal_was_claimed_ = false;
};

// ── Mouse-message WPARAM key-state flags ─────────────────────────────────
// Mirrors the MK_* constants from <winuser.h>. Duplicated as plain constants
// so this header stays parseable off Windows; static_assert'd against the
// real MK_* values in the platform TU.
inline constexpr uint32_t kMkLButton = 0x0001;  // MK_LBUTTON
inline constexpr uint32_t kMkRButton = 0x0002;  // MK_RBUTTON
inline constexpr uint32_t kMkShift   = 0x0004;  // MK_SHIFT
inline constexpr uint32_t kMkControl = 0x0008;  // MK_CONTROL
inline constexpr uint32_t kMkMButton = 0x0010;  // MK_MBUTTON

// Win32 message / virtual-key values used by the pure decoders below. Kept
// here (and static_assert'd against the SDK in the platform TU) so the Windows
// input contract is tested on non-Windows CI too.
inline constexpr uint32_t kWmLButtonDown = 0x0201;
inline constexpr uint32_t kWmLButtonUp   = 0x0202;
inline constexpr uint32_t kWmRButtonDown = 0x0204;
inline constexpr uint32_t kWmRButtonUp   = 0x0205;
inline constexpr uint32_t kWmMButtonDown = 0x0207;
inline constexpr uint32_t kWmMButtonUp   = 0x0208;

inline constexpr uint32_t kVkBack   = 0x08;
inline constexpr uint32_t kVkTab    = 0x09;
inline constexpr uint32_t kVkReturn = 0x0D;
inline constexpr uint32_t kVkEscape = 0x1B;
inline constexpr uint32_t kVkSpace  = 0x20;
inline constexpr uint32_t kVkPrior  = 0x21;
inline constexpr uint32_t kVkNext   = 0x22;
inline constexpr uint32_t kVkEnd    = 0x23;
inline constexpr uint32_t kVkHome   = 0x24;
inline constexpr uint32_t kVkLeft   = 0x25;
inline constexpr uint32_t kVkUp     = 0x26;
inline constexpr uint32_t kVkRight  = 0x27;
inline constexpr uint32_t kVkDown   = 0x28;
inline constexpr uint32_t kVkDelete = 0x2E;
inline constexpr uint32_t kVkF1     = 0x70;
inline constexpr uint32_t kVkF12    = 0x7B;
inline constexpr uint32_t kVkOem1   = 0xBA;
inline constexpr uint32_t kVkOem7   = 0xDE;

// ── Coordinates ──────────────────────────────────────────────────────────

// Windows packs the client-area x in the low word of LPARAM and y in the high
// word, each a SIGNED 16-bit value. Reading them as unsigned loses the
// negative coordinates a CAPTURED drag reports once the pointer leaves the
// window past its left or top edge: a knob dragged up past the editor top
// would see y jump from 0 to ~65535 and slam to the opposite end of its
// range. LOWORD/HIWORD alone are unsigned, hence the explicit sign cast.
constexpr int16_t lparam_x(uint32_t lparam) noexcept {
    return static_cast<int16_t>(static_cast<uint16_t>(lparam & 0xFFFFu));
}

constexpr int16_t lparam_y(uint32_t lparam) noexcept {
    return static_cast<int16_t>(static_cast<uint16_t>((lparam >> 16) & 0xFFFFu));
}

// Window messages report client coordinates in PHYSICAL device pixels, while
// the view tree lives in LOGICAL units — the same physical/logical split
// handle_wm_size() applies to WM_SIZE. A non-positive scale falls back to 1
// rather than dividing by zero.
//
// This is only the window-space mapping; the host still runs the result
// through window_to_root_point() for the design-viewport transform.
inline Point lparam_to_logical_point(uint32_t lparam, float scale) noexcept {
    const float s = scale > 0.0f ? scale : 1.0f;
    return {static_cast<float>(lparam_x(lparam)) / s,
            static_cast<float>(lparam_y(lparam)) / s};
}

// ── Modifiers ────────────────────────────────────────────────────────────

// Windows packs only Shift and Control into the mouse-message WPARAM. Alt and
// the Windows key are keyboard state, not message state, so the caller samples
// them (GetKeyState) and passes them in — keeping this function pure.
constexpr uint16_t mouse_modifiers(uint32_t wparam, bool alt_down,
                                   bool meta_down) noexcept {
    uint16_t mods = kModNone;
    if (wparam & kMkShift) mods |= kModShift;
    if (wparam & kMkControl) mods |= kModCtrl;
    if (alt_down) mods |= kModAlt;
    if (meta_down) mods |= kModMeta;
    return mods;
}

// WM_MOUSEMOVE fires for plain hover too. A drag tick is only a drag while the
// left button is still physically held: with mouse capture active we keep
// receiving moves after the button is released outside the window, and
// treating those as drags would let a knob follow the pointer forever.
constexpr bool drag_continues(uint32_t wparam) noexcept {
    return (wparam & kMkLButton) != 0;
}

constexpr uint32_t button_mask(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::left: return kMkLButton;
        case MouseButton::right: return kMkRButton;
        case MouseButton::middle: return kMkMButton;
        default: return 0;
    }
}

constexpr bool drag_continues(uint32_t wparam, MouseButton button) noexcept {
    const uint32_t mask = button_mask(button);
    return mask != 0 && (wparam & mask) != 0;
}

constexpr MouseButton mouse_button_from_message(uint32_t message) noexcept {
    switch (message) {
        case kWmLButtonDown:
        case kWmLButtonUp: return MouseButton::left;
        case kWmRButtonDown:
        case kWmRButtonUp: return MouseButton::right;
        case kWmMButtonDown:
        case kWmMButtonUp: return MouseButton::middle;
        default: return MouseButton::none;
    }
}

// GET_WHEEL_DELTA_WPARAM, expressed without windowsx.h. Vertical wheel input
// is inverted into Pulp's positive-down convention; horizontal input keeps
// Win32's positive-right convention.
constexpr float wheel_steps(uint32_t wparam, bool horizontal) noexcept {
    const auto raw = static_cast<int16_t>(static_cast<uint16_t>(wparam >> 16));
    const float steps = static_cast<float>(raw) / 120.0f;
    return horizontal ? steps : -steps;
}

constexpr uint16_t key_modifiers(bool shift_down, bool control_down,
                                 bool alt_down, bool meta_down) noexcept {
    uint16_t mods = kModNone;
    if (shift_down) mods |= kModShift;
    if (control_down) mods |= kModCtrl;
    if (alt_down) mods |= kModAlt;
    if (meta_down) mods |= kModMeta;
    return mods;
}

constexpr KeyCode key_code_from_virtual_key(uint32_t vk) noexcept {
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<KeyCode>('a' + static_cast<int>(vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<KeyCode>(vk);
    if (vk >= kVkF1 && vk <= kVkF12)
        return static_cast<KeyCode>(static_cast<int>(KeyCode::f1) +
                                    static_cast<int>(vk - kVkF1));
    switch (vk) {
        case kVkBack: return KeyCode::backspace;
        case kVkTab: return KeyCode::tab;
        case kVkReturn: return KeyCode::enter;
        case kVkEscape: return KeyCode::escape;
        case kVkSpace: return KeyCode::space;
        case kVkPrior: return KeyCode::page_up;
        case kVkNext: return KeyCode::page_down;
        case kVkEnd: return KeyCode::end_;
        case kVkHome: return KeyCode::home;
        case kVkLeft: return KeyCode::left;
        case kVkUp: return KeyCode::up;
        case kVkRight: return KeyCode::right;
        case kVkDown: return KeyCode::down;
        case kVkDelete: return KeyCode::delete_;
        case kVkOem1: return KeyCode::semicolon;
        case kVkOem7: return KeyCode::apostrophe;
        default: return KeyCode::unknown;
    }
}

// ── GPU surface lifecycle ────────────────────────────────────────────────

// Dawn configures its presentation surface for the HWND's native-window shape
// at creation time. The editor HWND is created as a hidden top-level WS_POPUP
// and only becomes the DAW's WS_CHILD inside attach_to_parent(), so a surface
// created in the constructor presents against a window shape that no longer
// exists. The symptom is a black editor that only refreshes when something
// else forces the host to repaint (dragging the DAW's own scrollbar).
//
// This tracks the "create after reparent, destroy on detach" contract so an
// attach/detach/attach cycle is asserted without needing an HWND. Encoding it
// as state rather than an inline `if (!gpu_ || !skia_)` also keeps a partially
// constructed surface pair (GPU created, Skia failed) from being mistaken for
// "already initialized" on the next attach.
class SurfaceLifecycle {
public:
    // True once note_attached() has asked for creation and no detach has
    // reclaimed it. Must be false on a freshly constructed host.
    constexpr bool surfaces_created() const noexcept { return created_; }

    // Call after the HWND's final parent, style, and size are in place.
    // Returns true when the caller must create the GPU/Skia surfaces now.
    constexpr bool note_attached() noexcept {
        if (created_) return false;
        created_ = true;
        return true;
    }

    // Call on detach, before the HWND leaves its parent. Returns true when the
    // caller must tear the surfaces down so the next attach rebuilds them for
    // the new native-window shape.
    constexpr bool note_detached() noexcept {
        if (!created_) return false;
        created_ = false;
        return true;
    }

    // Surface creation is allowed to fail (no Dawn adapter, Skia surface
    // creation returned null). Recording that keeps the next attach from
    // assuming a live surface pair.
    constexpr void note_creation_failed() noexcept { created_ = false; }

private:
    bool created_ = false;
};

}  // namespace pulp::view::win_input
