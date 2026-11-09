#pragma once

/// @file metal_surface.hpp
/// A native Metal surface for an editor that brings its own renderer.
///
/// `MetalSurface` owns a child `NSView` backed by a `CAMetalLayer` and the
/// `MTLDevice` that layer uses. It hands out raw drawables and leaves command
/// queues, encoding and presentation to the client: a renderer that takes only
/// a device can draw into it as it is. It names no Pulp view, canvas, render
/// or GPU-runtime type, and it never uses the shared GPU runtime. See
/// `pulp/format/native_surface_host.hpp` for that rule.
///
/// Handles are `void*` so C++ translation units can include this header. They
/// are `NSView*`, `id<MTLDevice>`, `id<MTLCommandQueue>`, `id<CAMetalDrawable>`
/// and `id<MTLTexture>`; Objective-C++ callers cast them with `(__bridge T)`.
///
/// Threading:
/// - Construction, `attach`, `detach`, `invalidate`, `set_drawable_size`,
///   `set_listener`, `set_release_handler`, `on_screen` and destruction run on
///   the main thread.
/// - `geometry()`, `acquire_drawable()` and everything on `MetalDrawable` may
///   run on any thread, typically a render thread the client owns.
///
/// Teardown is two-phase. Before the surface removes its view or lets go of
/// its layer (in `detach`, `invalidate` or destruction), it calls the release
/// handler on the main thread. The handler returns only once the client has
/// stopped using drawables: it signals the render thread and joins it. After
/// that, `acquire_drawable()` returns null until the next `attach`. A pending
/// acquisition is bounded (`drawable_wait_bound`, Core Animation's
/// `allowsNextDrawableTimeout`), so a render thread blocked in one cannot hold
/// teardown longer than that.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <pulp/native/input.hpp>

namespace pulp::native {

/// Who decides the layer's drawable size.
enum class DrawableSizing : std::uint8_t {
    /// The surface keeps the drawable at the view's size times its backing
    /// scale, updated on the main thread before the next drawable is handed
    /// out at the old size.
    automatic = 0,
    /// The client sets it with `set_drawable_size`, between its own frames.
    /// The surface never changes it.
    client = 1,
};

struct MetalSurfaceOptions {
    /// An `MTLPixelFormat` value. 80 is `MTLPixelFormatBGRA8Unorm`.
    std::uint32_t pixel_format = 80;
    /// False lets a client read or blit from a drawable's texture.
    bool framebuffer_only = false;
    bool opaque = true;
    DrawableSizing sizing = DrawableSizing::automatic;
    /// 2 or 3; Core Animation's own limits.
    std::uint32_t maximum_drawable_count = 3;
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

/// A drawable handed out by `MetalSurface::acquire_drawable()`. It keeps the
/// drawable, and so its layer, alive until it is destroyed. Move-only.
class MetalDrawable {
  public:
    MetalDrawable() = default;
    MetalDrawable(MetalDrawable&& other) noexcept;
    MetalDrawable& operator=(MetalDrawable&& other) noexcept;
    MetalDrawable(const MetalDrawable&) = delete;
    MetalDrawable& operator=(const MetalDrawable&) = delete;
    ~MetalDrawable();

    explicit operator bool() const noexcept {
        return drawable_ != nullptr;
    }

    /// `id<CAMetalDrawable>`, for a client that presents through its own
    /// command buffer or by any other means.
    void* drawable() const noexcept {
        return drawable_;
    }
    /// `id<MTLTexture>` of the drawable.
    void* texture() const noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;

    /// Schedule presentation on `command_buffer` (`id<MTLCommandBuffer>`), or
    /// present at once when it is null. False, and nothing happens, if this
    /// drawable was already presented through this call.
    bool present(void* command_buffer = nullptr);
    bool presented() const noexcept {
        return presented_;
    }

  private:
    friend class MetalSurface;
    explicit MetalDrawable(void* retained_drawable) noexcept : drawable_(retained_drawable) {}
    void reset() noexcept;

    void* drawable_ = nullptr;
    bool presented_ = false;
};

class MetalSurface {
  public:
    /// The longest a single `acquire_drawable()` waits for a free drawable
    /// before it returns null. It is Core Animation's own timeout.
    static constexpr std::chrono::milliseconds drawable_wait_bound{1000};

    /// A surface on the system default Metal device, or null when the system
    /// has no Metal device. Main thread.
    static std::unique_ptr<MetalSurface> create(const MetalSurfaceOptions& options = {});

    ~MetalSurface();
    MetalSurface(const MetalSurface&) = delete;
    MetalSurface& operator=(const MetalSurface&) = delete;

    /// Add the surface's view to `parent` (`NSView*`) and track its window.
    /// The parent may not be in a window yet: the surface reports
    /// `window_changed` when it is. False when already attached, after
    /// `invalidate()`, or when `parent` is null.
    bool attach(void* parent);
    /// Two-phase teardown, then remove the view from its parent. The parent is
    /// still valid during the call. A no-op when not attached.
    void detach();
    /// The parent is already gone, or is being destroyed by the host. Run the
    /// release handler and drop every reference to the view without touching
    /// the parent. Idempotent.
    void invalidate();
    bool attached() const noexcept;

    /// `NSView*` the surface draws into, for a host that positions it.
    void* view() const noexcept;
    /// `id<MTLDevice>` the layer uses. A client may create its own command
    /// queue on it.
    void* device() const noexcept;
    /// `id<MTLCommandQueue>` the surface created, for a client that has none.
    void* command_queue() const noexcept;
    /// The layer's `MTLPixelFormat`, equal to every drawable texture's format.
    std::uint32_t pixel_format() const noexcept;
    DrawableSizing sizing() const noexcept;

    /// The latest consistent size snapshot. Any thread.
    SurfaceGeometry geometry() const;
    /// On-screen state now. Main thread.
    OnScreenState on_screen() const;
    /// Report `visibility_changed` if the on-screen state moved since the last
    /// report. AppKit's occlusion, miniaturize and screen notifications call
    /// this, but ordering a window in or out posts none of them in an app
    /// that is not active (a plug-in host's helper process, a test), so a
    /// frame tick calls it as well. Main thread.
    void poll_visibility();

    /// The next drawable, or an empty one when the surface is not attached, is
    /// in no window, is being torn down, or none freed up within
    /// `drawable_wait_bound`. Any thread.
    MetalDrawable acquire_drawable();

    /// Set the drawable size, in pixels, for `DrawableSizing::client`. Main
    /// thread. False in automatic mode, or for a zero size.
    bool set_drawable_size(std::uint32_t width, std::uint32_t height);

    /// Called on the main thread for each `SurfaceEvent`.
    void set_listener(std::function<void(const SurfaceEvent&)> listener);
    /// Called on the main thread at the start of teardown. It must return only
    /// after the client has stopped acquiring and using drawables.
    void set_release_handler(std::function<void()> handler);

    /// Where pointer, wheel, key, text, focus and drop events go. Main thread.
    void set_input_handler(InputHandler handler);
    /// The cursor shown while the pointer is over the surface; the arrow is
    /// restored when it leaves. Main thread.
    void set_cursor(Cursor cursor);
    /// Hold keyboard focus for text entry (a click into a text field), or give
    /// it back. While held the view is first responder and keys are offered to
    /// the client first; otherwise the host keeps the keyboard. A host that
    /// takes first responder ends the hold. False when the view is in no
    /// window or AppKit refused the change. Main thread.
    bool set_text_focus(bool held);
    bool text_focus() const noexcept;

    /// The Objective-C class of the surface's view. Its name is unique to the
    /// binary that contains this library.
    static std::string view_class_name();

    struct Impl;

  private:
    explicit MetalSurface(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::native
