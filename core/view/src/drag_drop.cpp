// Cross-platform native → view-tree drop dispatch (declared in drag_drop.hpp).
//
// Platform backends (SDL3 standalone drop events, Windows IDropTarget, Linux
// XDND, macOS NSDraggingDestination) extract the dropped payload into a DropData
// and a root-space point, then call these to route it into the view tree. The
// target-resolution + local-coordinate walk mirror View::simulate_click
// (core/view/src/view.cpp) so drops land on the same view a click would.
//
// Resolution is generic: it knows about the DropReceiver interface and the
// View::on_drop std::function, never a concrete widget — so new drop-aware
// widgets just implement DropReceiver. Platform-specific *capture* lives in the
// per-OS backends; this file is the one shared place that understands the view
// tree, so it compiles everywhere.

#include <pulp/view/drag_drop.hpp>

#include <pulp/view/view.hpp>

#include <mutex>

namespace pulp::view {

namespace {

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

// Fire View::on_drop with the (type, data, x, y) string contract. A multi-file
// drop fires once per path (matches the JS-bridge expectation of one callback
// invocation per dropped item).
void fire_view_on_drop(Point local, View* handler, const DropData& data) {
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

void clear_hover(DragSession& session) {
    if (session.hover) {
        session.hover->leave_drag();
        session.hover = nullptr;
    }
}

// Walk from the hit target up to `root` and return the first DropReceiver that
// claims the drag via accept_drag (idempotent; sets its own highlight). nullptr
// if none. `target` must be non-null.
DropReceiver* find_drag_receiver(View& root, View* target, const DropData& data,
                                 Point root_pos) {
    for (View* v = target; v; v = (v == &root ? nullptr : v->parent())) {
        if (auto* r = dynamic_cast<DropReceiver*>(v)) {
            if (r->accept_drag(data, to_local(root, v, root_pos))) return r;
        }
    }
    return nullptr;
}

// True if some View::on_drop handler sits at or above `target` (bounded by root).
bool has_on_drop_handler(View& root, View* target) {
    for (View* v = target; v; v = (v == &root ? nullptr : v->parent())) {
        if (v->on_drop) return true;
    }
    return false;
}

}  // namespace

bool dispatch_drag_enter(View& root, DragSession& session, const DropData& data,
                         Point root_pos) {
    View* target = root.hit_test(root_pos);
    if (!target) {  // outside the window entirely
        clear_hover(session);
        return false;
    }

    DropReceiver* found = find_drag_receiver(root, target, data, root_pos);
    if (found != session.hover) {
        // leave the previously-highlighted receiver; `found` (if any) already
        // highlighted itself via accept_drag during the search.
        if (session.hover) session.hover->leave_drag();
        session.hover = found;
    }

    return found != nullptr || has_on_drop_handler(root, target);
}

void dispatch_drag_move(View& root, DragSession& session, const DropData& data,
                        Point root_pos) {
    // A move is an enter against the (possibly new) target; enter already handles
    // the leave-old / enter-new transition.
    dispatch_drag_enter(root, session, data, root_pos);
}

void dispatch_drag_exit(View& /*root*/, DragSession& session) {
    clear_hover(session);
}

bool dispatch_drop(View& root, DragSession& session, const DropData& data,
                   Point root_pos) {
    clear_hover(session);

    View* target = root.hit_test(root_pos);
    if (!target) return false;  // dropped outside the window

    // First-handler-wins: walk deepest→root; at each view try the DropReceiver
    // surface first, then the View::on_drop convenience surface. The first that
    // consumes the drop ends the walk (no double-dispatch).
    for (View* v = target; v; v = (v == &root ? nullptr : v->parent())) {
        const Point local = to_local(root, v, root_pos);
        if (auto* r = dynamic_cast<DropReceiver*>(v)) {
            if (r->accept_drop(data, local)) return true;
        }
        if (v->on_drop) {
            fire_view_on_drop(local, v, data);
            return true;
        }
    }
    return false;
}

// ── Process-global outbound drag backend (hostless platforms; Android) ───────

namespace {
// Guards g_file_drag_backend: it is set on the platform's surface/render thread
// (Android registers it from nativeOnSurfaceCreated) but invoked from the UI
// thread during a drag, so an unsynchronised std::function read could tear.
std::mutex g_file_drag_backend_mutex;
FileDragBackend g_file_drag_backend;
}  // namespace

FileDragBackend set_file_drag_backend(FileDragBackend backend) {
    std::lock_guard<std::mutex> lock(g_file_drag_backend_mutex);
    FileDragBackend prev = std::move(g_file_drag_backend);
    g_file_drag_backend = std::move(backend);
    return prev;
}

bool invoke_file_drag_backend(const FileDragRequest& request) {
    // Copy under the lock, then invoke unlocked — the backend may up-call into
    // platform code (Android: a JNI call into Kotlin) and must not run holding
    // the lock.
    FileDragBackend backend;
    {
        std::lock_guard<std::mutex> lock(g_file_drag_backend_mutex);
        backend = g_file_drag_backend;
    }
    return backend ? backend(request) : false;
}

#if !defined(__APPLE__)
// Outbound file drag is macOS-only today — the NSDraggingSession backend lives
// in platform/mac/drag_drop_mac.mm, which is compiled only on macOS. On every
// other platform View::start_file_drag (cross-platform, in view.cpp) still
// references begin_file_drag, so provide a no-op here so it links and degrades
// gracefully. Windows (IDataObject/DoDragDrop) and Linux (XDND) backends can
// supply a real implementation in their own platform TU when they land; this
// definition then drops out via the same __APPLE__-style guard.
bool begin_file_drag(void* /*native_view*/, const FileDragRequest& /*request*/) {
    return false;
}
#endif

}  // namespace pulp::view
