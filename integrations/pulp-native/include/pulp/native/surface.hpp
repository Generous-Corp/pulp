#pragma once

/// @file surface.hpp
/// Types shared by every pulp-native surface (`MetalSurface` on macOS,
/// `GlSurface` on Windows and X11): size and scale snapshots, on-screen state,
/// surface events and the input handler.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <pulp/native/input.hpp>

namespace pulp::native {

/// Who decides the drawable size.
enum class DrawableSizing : std::uint8_t {
    /// The surface keeps the drawable at the view's size times its scale,
    /// updated on the UI thread before the next frame at the old size.
    automatic = 0,
    /// The client sets it with `set_drawable_size`, between its own frames.
    /// The surface never changes it.
    client = 1,
};

/// One consistent reading of the surface's size. Written only on the main
/// thread and read whole from any thread.
struct SurfaceGeometry {
    /// Increases on every change.
    std::uint64_t generation = 0;
    /// The view's size in logical points, origin at its top-left.
    double width = 0.0;
    double height = 0.0;
    /// The window's backing scale, or 0 while the view is in no window.
    double scale = 0.0;
    /// The layer's drawable size in pixels.
    std::uint32_t drawable_width = 0;
    std::uint32_t drawable_height = 0;
    bool in_window = false;
};

/// Whether the surface is on screen, as far as AppKit reports it.
struct OnScreenState {
    bool in_window = false;
    bool window_visible = false;
    bool minimized = false;
    bool occluded = false;

    bool on_screen() const noexcept {
        return in_window && window_visible && !minimized && !occluded;
    }
    bool operator==(const OnScreenState&) const = default;
};

enum class SurfaceEventKind : std::uint8_t {
    /// The view entered a window, left one, or moved to another.
    window_changed = 0,
    /// Size or backing scale changed; read `geometry()` for the new values.
    geometry_changed = 1,
    /// `on_screen()` changed.
    visibility_changed = 2,
};

struct SurfaceEvent {
    SurfaceEventKind kind = SurfaceEventKind::geometry_changed;
    SurfaceGeometry geometry;
    OnScreenState visibility;
};

/// Why focus moved.
enum class FocusReason : std::uint8_t {
    /// The surface's view became or stopped being first responder.
    responder = 0,
    /// Its window became or stopped being the key window.
    window = 1,
    /// The application became or stopped being active.
    application = 2,
};

struct FocusEvent {
    bool focused = false;
    FocusReason reason = FocusReason::responder;
};

enum class DropPhase : std::uint8_t {
    enter = 0,
    move = 1,
    exit = 2,
    drop = 3,
};

/// A file drag over the surface, in logical points from its top-left.
struct DropEvent {
    DropPhase phase = DropPhase::enter;
    double x = 0.0;
    double y = 0.0;
    /// The dragged files, as POSIX paths.
    std::vector<std::string> paths;
};

/// What the surface delivers to its client, on the main thread. Any member
/// may be empty.
struct InputHandler {
    std::function<void(const PointerEvent&)> pointer;
    /// Unset: wheel events go on to the host, so a host scroll view keeps
    /// working.
    std::function<void(const WheelEvent&)> wheel;
    /// Offered only when `route_key` says the client goes first. Return true
    /// when consumed; an unconsumed key goes on to the host (or, while text
    /// focus is held, to text input).
    std::function<bool(const KeyEvent&)> key;
    /// Committed text, separate from key identity, UTF-8.
    std::function<void(const std::string&)> text;
    /// Text an input method is still composing, and the caret within it in
    /// UTF-16 units. An empty string ends the composition.
    std::function<void(const std::string&, std::size_t)> marked_text;
    std::function<void(const FocusEvent&)> focus;
    /// Return true to accept the drag (on enter and every move); the drop
    /// itself arrives only for an accepted drag.
    std::function<bool(const DropEvent&)> drop;
};

} // namespace pulp::native
