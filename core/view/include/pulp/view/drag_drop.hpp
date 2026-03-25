#pragma once

#include <pulp/view/geometry.hpp>
#include <string>
#include <vector>
#include <functional>

namespace pulp::view {

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

} // namespace pulp::view
