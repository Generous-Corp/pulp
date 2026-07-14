#pragma once

#include <pulp/view/geometry.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulp::view {

class View;

// ── Drop data types ──────────────────────────────────────────────────────────

struct DropData {
    enum class Type { files, text, custom };

    Type type = Type::files;
    std::vector<std::string> file_paths;  // For file drops
    std::string text;                      // For text drops
    std::string custom_type;               // For custom data type
    std::vector<std::uint8_t> custom_data; // For custom binary data
};

// ── Drop receiver (view-tree drop handler) ───────────────────────────────────
//
// The extensible interface a View subclass mixes in to accept native drops
// routed by the dispatch_* functions below:
//
//     class MyZone : public View, public DropReceiver { ... };
//
// The dispatch core finds receivers via dynamic_cast as it walks the view tree,
// so a new drop-aware widget needs NO change to the dispatch core — that is the
// extension point. This is the rich, typed surface (FileDropZone implements it).
// For lightweight cases a view can instead set the View::on_drop std::function
// (the JS-bridge convenience surface). See the dispatch contract below for how
// the two interact.
class DropReceiver {
public:
    virtual ~DropReceiver() = default;

    // A drag carrying `data` is hovering at `local` (this receiver's local
    // coordinates). Return true to claim the hover (drives highlight state).
    // Called on enter and on each move while over this receiver, so it must be
    // idempotent. Default: decline.
    virtual bool accept_drag(const DropData& data, Point local) {
        (void)data;
        (void)local;
        return false;
    }

    // The drag left this receiver (moved away, exited the window, or dropped).
    // Clear any highlight set by accept_drag.
    virtual void leave_drag() {}

    // Commit a drop at `local`. Return true if handled — a true return CONSUMES
    // the drop (dispatch stops bubbling); false lets it fall through to an outer
    // receiver / View::on_drop.
    virtual bool accept_drop(const DropData& data, Point local) = 0;
};

// ── Legacy platform-registration drop target (macOS NSView) ──────────────────
//
// SEPARATE CONCERN from DropReceiver above. This is the platform-side
// registration layer: register_drop_target() tells a native OS view (today only
// the macOS NSView path in drag_drop_mac.mm) to advertise dragged types. It does
// NOT route into the Pulp view tree. The dispatch_* functions below are the
// convergence point for native delivery and DropReceiver. New code should
// implement DropReceiver, not DropTarget.
class DropTarget {
public:
    virtual ~DropTarget() = default;

    // Called when a drag enters the view. Return true to accept.
    virtual bool on_drag_enter(const DropData& data, Point position) { (void)data; (void)position; return false; }

    // Called as the drag moves over the view
    virtual void on_drag_move(Point position) { (void)position; }

    // Called when the drag leaves the view
    virtual void on_drag_exit() {}

    // Called when the user drops. Return true if handled.
    virtual bool on_drop(const DropData& data, Point position) { (void)data; (void)position; return false; }
};

// ── Drag source ──────────────────────────────────────────────────────────────

// Initiate a drag operation from a view
struct DragSource {
    DropData data;
    // Platform-specific drag initiation (macOS: NSDraggingSession)
    // Called by the view system when the user starts dragging
};

// ── Outbound file drag (view-tree → host) ────────────────────────────────────
//
// A request to drag one or more EXISTING on-disk files out of a Pulp view and
// into another application (e.g. drop a rendered/bounced .wav onto a DAW
// timeline). The files must already exist at `file_paths` when the drag begins
// — this is a plain file-URL drag, not a lazy file-promise, so the caller
// writes the file (typically to a temp dir) first.
//
// Driven from a pointer interaction: `View::start_file_drag()` reaches the
// view's attached host, hands the request to the platform backend
// (`begin_file_drag` below), and the OS takes over the drag. On macOS the
// backend begins an `NSDraggingSession` on the host NSView using the current
// mouse event, so this must be called synchronously from within a
// mouse-down/drag handler (where `NSApp.currentEvent` is the initiating event).
struct FileDragRequest {
    std::vector<std::string> file_paths;  // existing files to drag (>= 1 required)
    Point root_position{};                 // drag origin in root/view coords
    std::string display_name;              // optional label for the drag image
};

// Begin a native outbound file drag from `native_view` (macOS: an NSView*).
// Returns true if the platform started a drag session. Returns false when the
// platform is unsupported, the handle is null, there are no files, or there is
// no active mouse event to anchor the drag. Implemented per-platform alongside
// register_drop_target (macOS, Windows, and Linux/X11 today; a no-op stub
// elsewhere).
bool begin_file_drag(void* native_view, const FileDragRequest& request);

// Process-global outbound file-drag backend for HOSTLESS platforms — Android,
// where the view tree is a bare root View with no WindowHost / PluginViewHost,
// so neither the host virtual nor the native-handle free function above can be
// reached. The platform layer registers a backend (Android: a JNI up-call into
// Kotlin's View.startDragAndDrop); View::start_file_drag() invokes it only as a
// last resort, after the host paths decline. Returns the previous backend so a
// caller can restore it. Pass nullptr to clear.
using FileDragBackend = std::function<bool(const FileDragRequest&)>;
FileDragBackend set_file_drag_backend(FileDragBackend backend);

// Invoke the registered global backend; returns false if none is set. Used by
// View::start_file_drag() — application code should call View::start_file_drag.
bool invoke_file_drag_backend(const FileDragRequest& request);

// ── Drag-drop registration ──────────────────────────────────────────────────

// Register/unregister a native view for file drops (macOS NSView). See the
// DropTarget note above — this is the platform-registration layer, not the
// view-tree dispatch.
bool register_drop_target(void* native_view, DropTarget& target);
void unregister_drop_target(void* native_view);

// ── Native → view-tree drop dispatch ─────────────────────────────────────────
//
// The bridge a platform drag-drop backend (SDL3 standalone drop events, Windows
// IDropTarget, Linux XDND, macOS NSDraggingDestination) calls to route a native
// drop into a Pulp view tree. Each function hit-tests `root` at the given
// root-space point, then walks from the hit target up to `root` — exactly like
// View::simulate_click resolves a click target.
//
// Dispatch contract (first-handler-wins, no double-dispatch): at each view from
// the deepest hit up to the root, in order:
//   1. if the view is a DropReceiver and accept_drop() returns true → consumed.
//   2. else if the view has a View::on_drop std::function set → fire it,
//      consumed. on_drop is the JS-bridge convenience surface; `type` is
//      "file"/"text"/"custom" and a multi-file drop fires it once per path,
//      with x/y in that view's local coordinates.
// The first view that consumes the drop ends the walk; a view never receives the
// drop through both surfaces.
//
// Coordinate space: `root_pos` is in `root`'s local (window/root) coordinates;
// the platform backend owns any window→root viewport transform (e.g. HiDPI /
// design-viewport scale) before calling in.
//
// Threading: UI thread only (mirrors the rest of the view input path).
//
// Hover state lives in a DragSession the *caller* owns — one per platform
// backend / window, NOT a process global — so concurrent drags on separate
// windows can't corrupt each other and the tracked pointer's lifetime is scoped
// to the backend that brackets the drag (enter … (move)* … exit|drop).

// Per-drag hover state owned by the platform backend (a member of the window
// host / drop target). Reset between drags happens automatically via exit/drop.
struct DragSession {
    DropReceiver* hover = nullptr;  // receiver currently claiming hover, or null
};

// Hover lifecycle for visual feedback (DropReceiver highlight). Returns true if a
// receiver claims the drag, or a View::on_drop handler is under the point.
bool dispatch_drag_enter(View& root, DragSession& session, const DropData& data,
                         Point root_pos);
void dispatch_drag_move(View& root, DragSession& session, const DropData& data,
                        Point root_pos);
void dispatch_drag_exit(View& root, DragSession& session);

// Commit a drop. Returns true if a view consumed it. Also clears any hover state.
bool dispatch_drop(View& root, DragSession& session, const DropData& data,
                   Point root_pos);

// ── In-tree drag source ──────────────────────────────────────────────────────
//
// Everything above routes a drag that ORIGINATED OUTSIDE the view tree (the OS
// handed it to a platform backend). An ActiveDrag is the other direction: a drag
// that starts INSIDE the tree and stays there.
//
// The payload never touches the OS pasteboard, so it can be anything a DropData
// can carry — including a `custom_data` blob — and it arrives at the receiver
// byte-for-byte. (An OS-mediated drag can only carry serialized types, which is
// why code that needs to hand a live object across a drag usually ends up
// parking it in a side channel and dragging a token. An in-tree drag does not
// need that.)
//
// The SOURCE view drives the gesture, because the source view is the one holding
// the pointer capture: it calls start_drag() on press-and-move, drag_to() on each
// motion tick, and drop_here() on release. The tree does the rest — hit-testing
// receivers under the pointer, firing accept_drag()/leave_drag() for highlight,
// and committing through the same dispatch core a native drop uses.
struct ActiveDrag {
    DropData data;
    View* source = nullptr;
    DragSession session;
    bool active = false;
};

} // namespace pulp::view
