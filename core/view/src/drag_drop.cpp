// Cross-platform native → view-tree drop dispatch (declared in drag_drop.hpp).
//
// Platform backends (SDL3 standalone drop events, Windows IDropTarget, Linux
// XDND, macOS NSDraggingDestination) extract the dropped payload into a DropData
// and a root-space point, then call these to route it into the view tree. The
// target-resolution + local-coordinate walk + handler-bubble mirror
// View::simulate_click (core/view/src/view.cpp) so drops land on the same view a
// click at that point would.
//
// Platform-specific *capture* lives in the per-OS backends; this file is the one
// shared place that understands the Pulp view tree, so it compiles everywhere.

#include <pulp/view/drag_drop.hpp>

#include <pulp/view/file_drop_zone.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

namespace {

// The single drop zone currently highlighted by an in-flight drag. One pointer
// drags at a time, so a single raw pointer suffices; it is cleared on exit/drop,
// and the backend always brackets a drag (enter … exit|drop), so it never
// outlives the view. UI thread only — no synchronization.
FileDropZone* g_hover_zone = nullptr;

// Walk a root-space point down into `target`'s local coordinates by subtracting
// each ancestor's origin up to (but not including) `root`. Mirrors the local
// conversion in View::simulate_click.
Point to_local(View& root, View* target, Point root_pos) {
    Point local = root_pos;
    View* v = target;
    while (v && v != &root) {
        local.x -= v->bounds().x;
        local.y -= v->bounds().y;
        v = v->parent();
    }
    return local;
}

// Nearest ancestor (inclusive of `target`, bounded by `root`) with a generic
// View::on_drop handler. nullptr if none.
View* find_drop_handler(View& root, View* target) {
    View* v = target;
    while (v) {
        if (v->on_drop) return v;
        if (v == &root) break;
        v = v->parent();
    }
    return nullptr;
}

// Nearest FileDropZone ancestor (inclusive of `target`, bounded by `root`).
FileDropZone* find_drop_zone(View& root, View* target) {
    View* v = target;
    while (v) {
        if (auto* zone = dynamic_cast<FileDropZone*>(v)) return zone;
        if (v == &root) break;
        v = v->parent();
    }
    return nullptr;
}

// Fire View::on_drop with the (type, data, x, y) string contract. A multi-file
// drop fires once per path (matches the JS-bridge expectation of one callback
// invocation per dropped item).
void fire_view_on_drop(View& root, View* handler, const DropData& data,
                       Point root_pos) {
    const Point local = to_local(root, handler, root_pos);
    switch (data.type) {
        case DropData::Type::files:
            for (const auto& path : data.file_paths)
                handler->on_drop("file", path, local.x, local.y);
            break;
        case DropData::Type::text:
            handler->on_drop("text", data.text, local.x, local.y);
            break;
        case DropData::Type::custom:
            handler->on_drop("custom", data.custom_type, local.x, local.y);
            break;
    }
}

void clear_hover() {
    if (g_hover_zone) {
        g_hover_zone->drag_leave();
        g_hover_zone = nullptr;
    }
}

}  // namespace

bool dispatch_drag_enter(View& root, const DropData& data, Point root_pos) {
    View* target = root.hit_test(root_pos);
    if (!target) {  // outside the window entirely
        clear_hover();
        return false;
    }

    // Hover visuals only apply to file drags over a FileDropZone.
    FileDropZone* zone = (data.type == DropData::Type::files)
                             ? find_drop_zone(root, target)
                             : nullptr;
    if (zone != g_hover_zone) {
        clear_hover();
        g_hover_zone = zone;
        if (g_hover_zone) g_hover_zone->drag_enter(data.file_paths);
    }

    return zone != nullptr || find_drop_handler(root, target) != nullptr;
}

void dispatch_drag_move(View& root, const DropData& data, Point root_pos) {
    // A move is an enter against the (possibly new) target; enter already handles
    // the leave-old / enter-new transition.
    dispatch_drag_enter(root, data, root_pos);
}

void dispatch_drag_exit(View& /*root*/) { clear_hover(); }

bool dispatch_drop(View& root, const DropData& data, Point root_pos) {
    clear_hover();

    View* target = root.hit_test(root_pos);
    if (!target) return false;  // dropped outside the window

    bool handled = false;

    // FileDropZone gets the typed paths (extension validation + visual reset live
    // in FileDropZone::drop) for file drops.
    if (data.type == DropData::Type::files) {
        if (auto* zone = find_drop_zone(root, target)) {
            zone->drop(data.file_paths);
            handled = true;
        }
    }

    // Generic View::on_drop — the surface the JS bridge wires. A view can carry
    // both (a FileDropZone subclass with its own on_drop), so fire both.
    if (auto* handler = find_drop_handler(root, target)) {
        fire_view_on_drop(root, handler, data, root_pos);
        handled = true;
    }

    return handled;
}

}  // namespace pulp::view
