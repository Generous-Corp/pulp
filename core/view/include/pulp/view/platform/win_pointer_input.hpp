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

// ── Mouse-message WPARAM key-state flags ─────────────────────────────────
// Mirrors the MK_* constants from <winuser.h>. Duplicated as plain constants
// so this header stays parseable off Windows; static_assert'd against the
// real MK_* values in the platform TU.
inline constexpr uint32_t kMkLButton = 0x0001;  // MK_LBUTTON
inline constexpr uint32_t kMkRButton = 0x0002;  // MK_RBUTTON
inline constexpr uint32_t kMkShift   = 0x0004;  // MK_SHIFT
inline constexpr uint32_t kMkControl = 0x0008;  // MK_CONTROL

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
