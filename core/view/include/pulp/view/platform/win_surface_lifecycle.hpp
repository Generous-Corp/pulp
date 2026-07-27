#pragma once

// win_surface_lifecycle.hpp — the Windows editor host's "create after reparent,
// destroy on detach" GPU-surface contract.
//
// Split out of win_pointer_input.hpp (WAH-9), which had accumulated three
// unrelated things: the pointer session, the Win32 message/key-code mappings,
// and this. A reader looking for how surfaces are created had no reason to open
// a header called "pointer input", and a reader changing pointer dispatch had
// no reason to expect surface lifetime to live under their hands.
//
// Kept HWND-free for the same reason the pointer decisions are: Pulp's required
// CI gate is macOS, so anything behind `_WIN32` is never executed by a gate.
// The state machine here is the part with a real invariant, so it is the part
// that most needs to be testable off Windows.

namespace pulp::view::win_input {

/// Tracks whether the Windows editor host currently owns a GPU/Skia surface
/// pair.
///
/// Dawn configures its presentation surface for the HWND's native-window shape
/// at creation time. The editor HWND is created as a hidden top-level WS_POPUP
/// and only becomes the DAW's WS_CHILD inside attach_to_parent(), so a surface
/// created in the constructor presents against a window shape that no longer
/// exists. The symptom is a black editor that only refreshes when something
/// else forces the host to repaint (dragging the DAW's own scrollbar).
///
/// Encoding this as state rather than an inline `if (!gpu_ || !skia_)` is what
/// keeps a PARTIALLY constructed pair — GPU surface created, Skia surface
/// failed — from being mistaken for "already initialized" on the next attach.
class SurfaceLifecycle {
public:
    /// True once note_attached() has asked for creation and no detach has
    /// reclaimed it. Must be false on a freshly constructed host.
    constexpr bool surfaces_created() const noexcept { return created_; }

    /// Call after the HWND's final parent, style, and size are in place.
    /// Returns true when the caller must create the GPU/Skia surfaces now.
    constexpr bool note_attached() noexcept {
        if (created_) return false;
        created_ = true;
        return true;
    }

    /// Call on detach, before the HWND leaves its parent. Returns true when the
    /// caller must tear the surfaces down so the next attach rebuilds them for
    /// the new native-window shape.
    constexpr bool note_detached() noexcept {
        if (!created_) return false;
        created_ = false;
        return true;
    }

    /// Surface creation is allowed to fail (no Dawn adapter, Skia surface
    /// creation returned null). Recording that keeps the next attach from
    /// assuming a live surface pair.
    constexpr void note_creation_failed() noexcept { created_ = false; }

private:
    bool created_ = false;
};

}  // namespace pulp::view::win_input
