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

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>

#include <mutex>

namespace pulp::view {

namespace detail {

struct DragSessionAccess {
    struct Revision {
        std::uint64_t generation = 0;
        DropReceiver* hover = nullptr;
    };

    static std::uint64_t generation(const DragSession& session) {
        return session.state_.generation_;
    }

    static std::uint64_t bump_generation(DragSession& session) {
        return ++session.state_.generation_;
    }

    static Revision revision(const DragSession& session) {
        return {session.state_.generation_, session.hover};
    }

    static ViewCapture capture_live_hover(View& root, DragSession& session) {
        auto& state = session.state_;
        const bool public_value_changed =
            session.hover != state.observed_public_hover_;
        if (public_value_changed) {
            state.observed_public_hover_ = session.hover;
            state.identity_.reset();
        }
        if (!session.hover) {
            state.identity_.reset();
            return {};
        }
        if (state.identity_.has_value()) {
            if (auto* cached = state.identity_.live_in(root)) {
                if (dynamic_cast<DropReceiver*>(cached) == session.hover)
                    return state.identity_;
            }
            // identity_ once described this exact public pointer value. Its
            // logical View is gone; do not turn same-address allocator reuse
            // into ownership of a different drag receiver.
            return {};
        }
        // Preserve aggregate/designated initialization and direct pointer
        // assignment by importing a newly observed public value without ever
        // dereferencing it.
        std::vector<View*> pending{&root};
        while (!pending.empty()) {
            View* view = pending.back();
            pending.pop_back();
            if (dynamic_cast<DropReceiver*>(view) == session.hover) {
                state.identity_.set(view);
                return state.identity_;
            }
            for (std::size_t i = 0; i < view->child_count(); ++i) {
                if (auto* child = view->child_at(i)) pending.push_back(child);
            }
        }
        return {};
    }

    static bool changed_since(View& root, DragSession& session,
                              Revision before) {
        auto& state = session.state_;
        // `hover` remains assignable for source compatibility. Fold a changed
        // pointer into the dispatch generation before an outer callback can
        // publish stale ownership.
        if (state.generation_ == before.generation &&
            session.hover != before.hover) {
            capture_live_hover(root, session);
            ++state.generation_;
        }
        return state.generation_ != before.generation ||
               session.hover != before.hover;
    }

    static void clear_owner(DragSession& session) {
        session.hover = nullptr;
        session.state_.observed_public_hover_ = nullptr;
        session.state_.identity_.reset();
    }

    static void set_owner(DragSession& session, DropReceiver* receiver,
                          ViewCapture identity) {
        session.hover = receiver;
        session.state_.observed_public_hover_ = receiver;
        session.state_.identity_ = std::move(identity);
    }
};

}  // namespace detail

namespace {

// Walk a root-space point down into `target`'s local coordinates by subtracting
// each ancestor's origin up to (but not including) `root`. Mirrors the local
// conversion in View::simulate_click.
Point to_local(View& root, View* target, Point root_pos) {
    return point_to_local(root_pos, target, &root);
}

// Fire View::on_drop with the (type, data, x, y) string contract. A multi-file
// drop fires once per path (matches the JS-bridge expectation of one callback
// invocation per dropped item).
void fire_view_on_drop(Point local,
                       const std::function<void(const std::string&,
                                                const std::string&, float,
                                                float)>& handler,
                       const DropData& data) {
    switch (data.type) {
        case DropData::Type::files:
            for (const auto& path : data.file_paths)
                handler("file", path, local.x, local.y);
            break;
        case DropData::Type::text:
            handler("text", data.text, local.x, local.y);
            break;
        case DropData::Type::custom:
            handler("custom", data.custom_type, local.x, local.y);
            break;
    }
}

ViewCapture capture_live_hover(View& root, DragSession& session) {
    return detail::DragSessionAccess::capture_live_hover(root, session);
}

View* live_hover_view(View& root, DragSession& session) {
    return capture_live_hover(root, session).live_in(root);
}

bool clear_hover(View& root, DragSession& session) {
    // The platform owns DragSession and can keep it across a runtime realm
    // replacement. Re-resolve the captured View identity before virtual
    // dispatch; the old receiver may already have been destroyed.
    ViewCapture previous = capture_live_hover(root, session);
    // Publish the cleared state before user code. A reentrant drag callback may
    // install a new hover, which the outer clear must not erase afterward.
    const bool had_hover = session.hover != nullptr;
    detail::DragSessionAccess::clear_owner(session);
    if (had_hover)
        detail::DragSessionAccess::bump_generation(session);
    const auto callback_revision =
        detail::DragSessionAccess::revision(session);
    if (View* live = previous.live_in(root)) {
        if (auto* receiver = dynamic_cast<DropReceiver*>(live))
            receiver->leave_drag();
    }
    return detail::DragSessionAccess::changed_since(
        root, session, callback_revision);
}

// Walk from the hit target up to `root` and return the first DropReceiver that
// claims the drag via accept_drag (idempotent; sets its own highlight). nullptr
// if none. `target` must be non-null.
struct FoundReceiver {
    ViewCapture view;
    bool claimed = false;
    bool reentered = false;
};

void balance_pending_receiver(View& root, const FoundReceiver& found,
                              DragSession& session) {
    if (!found.claimed) return;
    View* pending = found.view.live_in(root);
    if (!pending || pending == live_hover_view(root, session))
        return;
    if (auto* receiver = dynamic_cast<DropReceiver*>(pending))
        receiver->leave_drag();
}

struct CapturedPathEntry {
    ViewCapture view;
    Point local;
};

std::vector<CapturedPathEntry> capture_path(View& root, View* target,
                                            Point root_pos) {
    std::vector<CapturedPathEntry> path;
    for (View* v = target; v; v = (v == &root ? nullptr : v->parent())) {
        CapturedPathEntry entry;
        entry.view.set(v);
        entry.local = to_local(root, v, root_pos);
        path.push_back(std::move(entry));
    }
    return path;
}

FoundReceiver find_drag_receiver(View& root,
                                 const std::vector<CapturedPathEntry>& path,
                                 const DropData& data, DragSession& session) {
    for (const auto& entry : path) {
        View* v = entry.view.live_in(root);
        if (!v) continue;
        if (auto* r = dynamic_cast<DropReceiver*>(v)) {
            const auto callback_revision =
                detail::DragSessionAccess::revision(session);
            const bool claimed = r->accept_drag(data, entry.local);
            if (detail::DragSessionAccess::changed_since(
                    root, session, callback_revision))
                return {.view = entry.view,
                        .claimed = claimed,
                        .reentered = true};
            if (!claimed) continue;
            return {.view = entry.view, .claimed = true};
        }
    }
    return {};
}

// True if some still-live View::on_drop handler sits in the captured path.
bool has_on_drop_handler(View& root,
                         const std::vector<CapturedPathEntry>& path) {
    for (const auto& entry : path) {
        View* v = entry.view.live_in(root);
        if (!v) continue;
        if (v->on_drop) return true;
    }
    return false;
}

}  // namespace

bool dispatch_drag_enter(View& root, DragSession& session, const DropData& data,
                         Point root_pos) {
    // Revision the operation before any user callback. A nested exit/drop must
    // be visible even during the first accept, before a hover is published.
    detail::DragSessionAccess::bump_generation(session);
    View* target = root.hit_test(root_pos);
    if (!target) {  // outside the window entirely
        clear_hover(root, session);
        return false;
    }

    const auto path = capture_path(root, target, root_pos);
    const auto found = find_drag_receiver(root, path, data, session);
    // accept_drag() is user code. If it synchronously dispatched another drag
    // operation, that nested operation owns the session now. Balance the outer
    // candidate's accept notification without disturbing the nested owner.
    if (found.reentered) {
        balance_pending_receiver(root, found, session);
        return found.claimed || has_on_drop_handler(root, path);
    }
    View* found_view = found.view.live_in(root);
    auto* found_receiver = found_view
                               ? dynamic_cast<DropReceiver*>(found_view)
                               : nullptr;
    if (found_receiver != session.hover ||
        found_view != live_hover_view(root, session)) {
        // leave the previously-highlighted receiver; `found` (if any) already
        // highlighted itself via accept_drag during the search.
        const bool reentered = clear_hover(root, session);
        // leave_drag() is user code and may synchronously dispatch a new drag
        // enter. Preserve the ownership installed by that nested dispatch;
        // overwriting it here would strand its highlight without a matching
        // leave notification.
        if (reentered) {
            balance_pending_receiver(root, found, session);
            return found.claimed || has_on_drop_handler(root, path);
        }
        found_view = found.view.live_in(root);
        found_receiver = found_view
                             ? dynamic_cast<DropReceiver*>(found_view)
                             : nullptr;
        detail::DragSessionAccess::set_owner(
            session, found_receiver, found.view);
        if (found_receiver)
            detail::DragSessionAccess::bump_generation(session);
    }

    return found.claimed || has_on_drop_handler(root, path);
}

void dispatch_drag_move(View& root, DragSession& session, const DropData& data,
                        Point root_pos) {
    // A move is an enter against the (possibly new) target; enter already handles
    // the leave-old / enter-new transition.
    dispatch_drag_enter(root, session, data, root_pos);
}

void dispatch_drag_exit(View& root, DragSession& session) {
    detail::DragSessionAccess::bump_generation(session);
    clear_hover(root, session);
}

bool dispatch_drop(View& root, DragSession& session, const DropData& data,
                   Point root_pos) {
    detail::DragSessionAccess::bump_generation(session);
    if (clear_hover(root, session))
        return false;

    View* target = root.hit_test(root_pos);
    if (!target) return false;  // dropped outside the window
    const auto path = capture_path(root, target, root_pos);
    // First-handler-wins: walk deepest→root; at each view try the DropReceiver
    // surface first, then the View::on_drop convenience surface. The first that
    // consumes the drop ends the walk (no double-dispatch).
    for (const auto& entry : path) {
        View* v = entry.view.live_in(root);
        if (!v) continue;
        if (auto* r = dynamic_cast<DropReceiver*>(v)) {
            const auto callback_revision =
                detail::DragSessionAccess::revision(session);
            const bool accepted = r->accept_drop(data, entry.local);
            if (detail::DragSessionAccess::changed_since(
                    root, session, callback_revision))
                return accepted;
            if (accepted) return true;
        }
        v = entry.view.live_in(root);
        if (!v) continue;
        if (v->on_drop) {
            auto handler = v->on_drop;
            fire_view_on_drop(entry.local, handler, data);
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

#if !defined(__APPLE__) && !(defined(__linux__) && defined(PULP_HAS_X11)) && !defined(_WIN32)
// Outbound file drag free-function backend, used by View::start_file_drag when
// the tree is owned by a WindowHost (standalone app) rather than a plugin host.
// Real backends live per platform: macOS NSDraggingSession (drag_drop_mac.mm),
// Linux XDND (drag_drop_linux.cpp, compiled when Xlib links — PULP_HAS_X11), and
// Windows OLE DoDragDrop (drag_drop_win.cpp, compiled unconditionally on _WIN32).
// This no-op covers the platforms without one yet (headless Linux, etc.) so the
// cross-platform call site links and degrades gracefully; each new
// backend extends the guard above as it lands.
bool begin_file_drag(void* /*native_view*/, const FileDragRequest& /*request*/) {
    return false;
}
#endif

}  // namespace pulp::view
