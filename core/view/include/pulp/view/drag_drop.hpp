#pragma once

#include <pulp/view/geometry.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace pulp::view {

class View;
class FileDropZone;

// ── Drop data types ──────────────────────────────────────────────────────────

struct DropData {
    enum class Type { files, text, custom };

    Type type = Type::files;
    std::vector<std::string> file_paths;  // For file drops
    std::string text;                      // For text drops
    std::string custom_type;               // For custom data type
    std::vector<uint8_t> custom_data;      // For custom binary data
};

// ── Drop target interface ────────────────────────────────────────────────────

// Views implement this to accept drops
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

// ── Drag-drop registration ──────────────────────────────────────────────────

// Register/unregister a native view for file drops (macOS NSView)
bool register_drop_target(void* native_view, DropTarget& target);
void unregister_drop_target(void* native_view);

// ── Native → view-tree drop dispatch ─────────────────────────────────────────
//
// The bridge a platform drag-drop backend (SDL3 standalone drop events, Windows
// IDropTarget, Linux XDND, macOS NSDraggingDestination) calls to route a native
// drop into a Pulp view tree. Each function hit-tests `root` at the given
// root-space point, finds the deepest View with a drop handler — bubbling up the
// parent chain bounded by `root`, exactly like View::simulate_click resolves a
// click target — and invokes it.
//
// Two handler surfaces are driven (a view may use either):
//   • View::on_drop(type, data, x, y) — the generic callback the JS bridge wires.
//     `type` is "file" / "text" / "custom"; for a multi-file drop it fires once
//     per path. x/y are in the handler view's local coordinates.
//   • FileDropZone — receives the typed paths via drag_enter/drag_leave/drop so
//     its extension validation + hover visuals work.
//
// Coordinate space: `root_pos` is in `root`'s local (window/root) coordinates;
// the platform backend is responsible for any window→root viewport transform
// (e.g. PluginViewHost::window_to_root_point) before calling in.
//
// Threading: UI thread only (mirrors the rest of the view input path).
//
// Hover state is held in a DragSession the *caller* owns — one per platform
// backend / window, NOT a process global — so concurrent drags on separate
// windows can't corrupt each other and the tracked pointer's lifetime is scoped
// to the backend that brackets the drag (enter … (move)* … exit|drop).

// Per-drag hover state owned by the platform backend (a member of the window
// host / drop target). Opaque to callers beyond construction; reset between
// drags happens automatically via exit/drop.
struct DragSession {
    FileDropZone* hover_zone = nullptr;  // currently-highlighted zone, or null
};

// Hover lifecycle for visual feedback (FileDropZone highlight). Returns true if a
// drop zone / handler is under the point and would accept the drag.
bool dispatch_drag_enter(View& root, DragSession& session, const DropData& data,
                         Point root_pos);
void dispatch_drag_move(View& root, DragSession& session, const DropData& data,
                        Point root_pos);
void dispatch_drag_exit(View& root, DragSession& session);

// Commit a drop. Returns true if a view handled it. Also clears any hover state.
bool dispatch_drop(View& root, DragSession& session, const DropData& data,
                   Point root_pos);

} // namespace pulp::view
