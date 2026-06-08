#pragma once

#include <pulp/view/geometry.hpp>
#include <string>
#include <vector>
#include <functional>

namespace pulp::view {

class View;

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
// Threading: UI thread only (mirrors the rest of the view input path). A single
// hover target is tracked across a drag (one pointer at a time); the backend must
// bracket a drag with enter … (move)* … exit|drop so the tracked pointer never
// outlives the view.

// Hover lifecycle for visual feedback (FileDropZone highlight). Returns true if a
// drop zone / handler is under the point and would accept the drag.
bool dispatch_drag_enter(View& root, const DropData& data, Point root_pos);
void dispatch_drag_move(View& root, const DropData& data, Point root_pos);
void dispatch_drag_exit(View& root);

// Commit a drop. Returns true if a view handled it. Also clears any hover state.
bool dispatch_drop(View& root, const DropData& data, Point root_pos);

} // namespace pulp::view
