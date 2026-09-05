// widget_bridge/metadata_api.cpp - metadata registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include "api_registry.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

namespace {

View* unique_anchor(View& root, std::string_view anchor, int& matches) {
    View* result = nullptr;
    if (root.anchor_id() == anchor) {
        ++matches;
        result = &root;
    }
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = unique_anchor(*root.child_at(i), anchor, matches))
            result = found;
    }
    return result;
}

CanvasWidget* canvas_at(View& root, std::size_t wanted, std::size_t& seen) {
    if (auto* canvas = dynamic_cast<CanvasWidget*>(&root)) {
        if (seen++ == wanted) return canvas;
    }
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = canvas_at(*root.child_at(i), wanted, seen))
            return found;
    return nullptr;
}

// Return whether `ancestor` is on `view`'s live parent chain. The retained
// canvas and authored behavior owner are normally siblings, but the bridge
// API is public and callers can legally bind a canvas below its owner. In that
// topology native DOM dispatch already reaches the owner; a second relay
// would deliver every event twice.
bool is_ancestor_or_self(const View* ancestor, const View* view) noexcept {
    for (auto* current = view; current; current = current->parent()) {
        if (current == ancestor) return true;
    }
    return false;
}

// Relays call the owner's native callback directly. That is normally a
// sibling callback, but a public bridge caller can accidentally make two
// retained targets owners of one another. Keep a per-thread target stack so a
// malformed/re-entrant graph fails closed instead of recursing indefinitely.
class RelayDispatchScope {
public:
    explicit RelayDispatchScope(const View* target) : target_(target) {
        auto& active = active_targets();
        if (!target_ || std::find(active.begin(), active.end(), target_) !=
                            active.end())
            return;
        active.push_back(target_);
        entered_ = true;
    }

    ~RelayDispatchScope() {
        if (!entered_) return;
        auto& active = active_targets();
        // The stack is strictly nested for synchronous callbacks. Be
        // defensive if a hostile callback re-enters/throws while unwinding.
        if (!active.empty() && active.back() == target_)
            active.pop_back();
        else if (auto it = std::find(active.begin(), active.end(), target_);
                 it != active.end())
            active.erase(it);
    }

    bool entered() const noexcept { return entered_; }

private:
    static std::vector<const View*>& active_targets() {
        thread_local std::vector<const View*> active;
        return active;
    }

    const View* target_ = nullptr;
    bool entered_ = false;
};

// A materialized canvas's authored behavior owner and its retained DesignIR
// canvas are sibling subtrees. Native hit testing can therefore resolve the
// retained canvas while the bridge callback still lives on the authored
// wrapper. Keep that ownership explicit with a liveness-checked relay rather
// than copying a callback once (which would strand the retained target after a
// React commit replaces the wrapper).
//
// These callable structs deliberately live in the std::function type-erasure
// slot instead of a side table. That gives rebinding an idempotent way to
// recover the pre-bind callback: a later bind unwraps the previous relay and
// composes from the original callback instead of stacking another relay. It
// also means an existing native callback on the retained target is preserved.
struct DomPointerRelay {
    View* root = nullptr;
    std::weak_ptr<const std::uint64_t> root_lifetime;
    ViewCapture target_capture;
    ViewCapture source_capture;
    ViewCapture owner_capture;
    bool moving = false;
    View::PointerEvents source_previous_pointer_events =
        View::PointerEvents::auto_;
    float source_previous_opacity = 1.0f;
    bool source_state_changed = false;
    View::PointerEvents owner_previous_pointer_events =
        View::PointerEvents::auto_;
    bool owner_state_changed = false;
    std::function<void(const MouseEvent&, bool)> previous;

    void operator()(const MouseEvent& event, bool is_dom_origin) const {
        if (!root || root_lifetime.expired()) return;
        auto* live_target = target_capture.live_in(*root);
        if (!live_target) return;
        RelayDispatchScope dispatch_scope(live_target);
        if (!dispatch_scope.entered()) return;
        if (previous) previous(event, is_dom_origin);
        // A preserved callback may synchronously tear down the imported root.
        // Never re-enter the tree (or dereference root) after that callback
        // unless the lifetime token still proves the root is alive.
        if (!root || root_lifetime.expired()) return;
        // The preserved target callback may synchronously unmount this view.
        // Re-resolve the identity before using it for topology checks; the
        // captured instance id also rejects allocator-address reuse.
        live_target = target_capture.live_in(*root);
        if (!live_target) return;
        auto* live_owner = owner_capture.live_in(*root);
        if (!live_owner) return;
        // If the retained target has been reparented below its owner, the
        // normal native DOM walk will reach that owner later in this dispatch.
        // Do not deliver a second copy through the relay. Rebinding is still
        // required when a replacement object is allocated, but this check also
        // keeps a transient reparent from duplicating an event.
        if (is_ancestor_or_self(live_owner, live_target)) return;
        auto callback = moving ? live_owner->on_dom_pointer_move_event
                               : live_owner->on_dom_pointer_event;
        if (!callback) return;
        // Pointer dispatch localizes `event.position` to the retained target.
        // The owner can be in a differently transformed/scrolling subtree, so
        // re-localize from the stable window coordinate before forwarding.
        auto owner_event = event;
        owner_event.position =
            point_to_local(event.window_position, live_owner, root);
        callback(owner_event, is_dom_origin);
    }
};

struct DomWheelRelay {
    View* root = nullptr;
    std::weak_ptr<const std::uint64_t> root_lifetime;
    ViewCapture target_capture;
    ViewCapture owner_capture;
    std::function<void(const MouseEvent&, bool)> previous;

    void operator()(const MouseEvent& event, bool is_dom_origin) const {
        if (!root || root_lifetime.expired()) return;
        auto* live_target = target_capture.live_in(*root);
        if (!live_target) return;
        RelayDispatchScope dispatch_scope(live_target);
        if (!dispatch_scope.entered()) return;
        if (previous) previous(event, is_dom_origin);
        if (!root || root_lifetime.expired()) return;
        live_target = target_capture.live_in(*root);
        if (!live_target) return;
        auto* live_owner = owner_capture.live_in(*root);
        if (!live_owner) return;
        if (is_ancestor_or_self(live_owner, live_target)) return;
        if (!live_owner->on_dom_wheel_event) return;
        auto owner_event = event;
        owner_event.position =
            point_to_local(event.window_position, live_owner, root);
        live_owner->on_dom_wheel_event(owner_event, is_dom_origin);
    }
};

struct ClickRelay {
    View* root = nullptr;
    std::weak_ptr<const std::uint64_t> root_lifetime;
    ViewCapture target_capture;
    ViewCapture owner_capture;
    std::function<void()> previous;

    void operator()() const {
        if (!root || root_lifetime.expired()) return;
        auto* live_target = target_capture.live_in(*root);
        if (!live_target) return;
        RelayDispatchScope dispatch_scope(live_target);
        if (!dispatch_scope.entered()) return;
        if (previous) previous();
        if (!root || root_lifetime.expired()) return;
        live_target = target_capture.live_in(*root);
        if (!live_target) return;
        auto* live_owner = owner_capture.live_in(*root);
        if (!live_owner) return;
        // Unlike pointer/wheel dispatch, click resolution stops at the first
        // on_click slot it finds while walking up from the hit target. If this
        // relay remains on the target, native dispatch will not call an
        // ancestor's on_click separately, so forwarding here is required even
        // when the owner is already on that ancestor path.
        if (live_owner->on_click) live_owner->on_click();
    }
};

std::function<void(const MouseEvent&, bool)> unwrap_dom_pointer_relay(
    std::function<void(const MouseEvent&, bool)> callback) {
    // Unwrap repeatedly so a callback produced by an older/re-entrant bind
    // cannot accumulate a chain of relays after several React commits.
    while (auto* relay = callback.target<DomPointerRelay>())
        callback = relay->previous;
    return callback;
}

std::function<void(const MouseEvent&, bool)> unwrap_dom_wheel_relay(
    std::function<void(const MouseEvent&, bool)> callback) {
    while (auto* relay = callback.target<DomWheelRelay>())
        callback = relay->previous;
    return callback;
}

std::function<void()> unwrap_click_relay(std::function<void()> callback) {
    while (auto* relay = callback.target<ClickRelay>())
        callback = relay->previous;
    return callback;
}

// A successful bind always installs a DomPointerRelay in the target's press
// slot. Recovering that callable before replacing it lets a later bind restore
// the exact source/owner hit-test state that the previous bind temporarily
// changed. The fallback move slot handles a caller that replaced only the
// press callback between commits.
DomPointerRelay* existing_canvas_binding(View& target) noexcept {
    if (auto* relay = target.on_dom_pointer_event.target<DomPointerRelay>())
        return relay;
    return target.on_dom_pointer_move_event.target<DomPointerRelay>();
}

void restore_previous_canvas_state(View& root, DomPointerRelay* previous,
                                   View* source, View* owner) {
    if (!previous) return;
    if (auto* old_source = previous->source_capture.live_in(root);
        old_source && old_source != source && previous->source_state_changed) {
        // Do not overwrite a state changed deliberately by the caller after
        // the bind. Restore only the values this binding actually installed.
        if (old_source->pointer_events() == View::PointerEvents::none &&
            old_source->opacity() == 0.0f) {
            old_source->set_pointer_events(
                previous->source_previous_pointer_events);
            old_source->set_opacity(previous->source_previous_opacity);
        }
    }
    if (auto* old_owner = previous->owner_capture.live_in(root);
        old_owner && old_owner != owner && previous->owner_state_changed) {
        if (old_owner->pointer_events() == View::PointerEvents::auto_)
            old_owner->set_pointer_events(
                previous->owner_previous_pointer_events);
    }
}

DomPointerRelay make_dom_pointer_relay(
    View& root, View* target, View* source, View* owner, bool moving,
    std::function<void(const MouseEvent&, bool)> previous,
    View::PointerEvents source_previous_pointer_events,
    float source_previous_opacity, bool source_state_changed,
    View::PointerEvents owner_previous_pointer_events,
    bool owner_state_changed) {
    DomPointerRelay relay;
    relay.root = &root;
    relay.root_lifetime = root.import_binding_lifetime_token();
    relay.target_capture.set(target);
    relay.source_capture.set(source);
    relay.owner_capture.set(owner);
    relay.moving = moving;
    relay.source_previous_pointer_events = source_previous_pointer_events;
    relay.source_previous_opacity = source_previous_opacity;
    relay.source_state_changed = source_state_changed;
    relay.owner_previous_pointer_events = owner_previous_pointer_events;
    relay.owner_state_changed = owner_state_changed;
    relay.previous = std::move(previous);
    return relay;
}

DomWheelRelay make_dom_wheel_relay(
    View& root, View* target, View* owner,
    std::function<void(const MouseEvent&, bool)> previous) {
    DomWheelRelay relay;
    relay.root = &root;
    relay.root_lifetime = root.import_binding_lifetime_token();
    relay.target_capture.set(target);
    relay.owner_capture.set(owner);
    relay.previous = std::move(previous);
    return relay;
}

ClickRelay make_click_relay(View& root, View* target, View* owner,
                            std::function<void()> previous) {
    ClickRelay relay;
    relay.root = &root;
    relay.root_lifetime = root.import_binding_lifetime_token();
    relay.target_capture.set(target);
    relay.owner_capture.set(owner);
    relay.previous = std::move(previous);
    return relay;
}

// Return every owner reached by a relay installed on `view`. Pointer/move,
// wheel, and click slots are kept separate because a caller may have rebound
// only one channel. A shared behavior wrapper can legitimately own several
// retained canvases, so cycle rejection must follow the actual owner graph
// rather than reject every view that happens to carry one relay.
std::vector<View*> relay_owners(View& root, View& view) {
    std::vector<View*> owners;
    const auto add = [&](View* owner) {
        if (owner && std::find(owners.begin(), owners.end(), owner) ==
                         owners.end())
            owners.push_back(owner);
    };
    if (auto* relay = view.on_dom_pointer_event.target<DomPointerRelay>())
        add(relay->owner_capture.live_in(root));
    if (auto* relay = view.on_dom_pointer_move_event.target<DomPointerRelay>())
        add(relay->owner_capture.live_in(root));
    if (auto* relay = view.on_dom_wheel_event.target<DomWheelRelay>())
        add(relay->owner_capture.live_in(root));
    if (auto* relay = view.on_click.target<ClickRelay>())
        add(relay->owner_capture.live_in(root));
    return owners;
}

bool relay_reaches(View& root, View* start, const View* destination,
                   std::vector<View*>& visited) {
    if (!start) return false;
    if (start == destination) return true;
    if (std::find(visited.begin(), visited.end(), start) != visited.end())
        return false;
    visited.push_back(start);
    for (auto* owner : relay_owners(root, *start))
        if (relay_reaches(root, owner, destination, visited)) return true;
    return false;
}

bool bind_canvas_program(View& root, std::string anchor, CanvasWidget* source,
                         View*, View* behavior_owner) {
    int matches = 0;
    auto* target_view = unique_anchor(root, anchor, matches);
    // A relay owner must be a separate subtree. Self/ancestor bindings either
    // recurse or duplicate the native DOM walk, so fail closed before mutating
    // either canvas.
    if (matches != 1 || !target_view || !source || target_view == source ||
        (behavior_owner &&
          (is_ancestor_or_self(behavior_owner, target_view) ||
          is_ancestor_or_self(target_view, behavior_owner))) ||
        is_ancestor_or_self(source, target_view) ||
        is_ancestor_or_self(target_view, source))
        return false;
    if (behavior_owner) {
        // Shared owners are valid (several retained canvases commonly relay
        // into one authored wrapper), but a new edge that reaches the target
        // would close a relay cycle. Reject that exact graph before mutating
        // any callbacks; RelayDispatchScope remains a runtime defense for a
        // callback graph changed after this bind.
        std::vector<View*> visited;
        if (relay_reaches(root, behavior_owner, target_view, visited))
            return false;
    }
    auto* target = dynamic_cast<CanvasWidget*>(target_view);
    if (!target) return false;
    auto* previous_binding = existing_canvas_binding(*target_view);
    auto* previous_source = previous_binding
        ? previous_binding->source_capture.live_in(root) : nullptr;
    auto* previous_owner = previous_binding
        ? previous_binding->owner_capture.live_in(root) : nullptr;
    restore_previous_canvas_state(root, previous_binding, source,
                                  behavior_owner);

    // Capture the states before this bind makes its temporary hit-test edits.
    // When rebinding the same objects, carry the original pre-bind values
    // forward so a later owner/source swap can restore them instead of
    // treating the already-hidden/auto state as the new baseline.
    auto source_previous_pointer_events = source->pointer_events();
    auto source_previous_opacity = source->opacity();
    auto source_state_changed =
        source_previous_pointer_events != View::PointerEvents::none ||
        source_previous_opacity != 0.0f;
    if (previous_binding && previous_source == source) {
        source_previous_pointer_events =
            previous_binding->source_previous_pointer_events;
        source_previous_opacity = previous_binding->source_previous_opacity;
        source_state_changed = previous_binding->source_state_changed;
    }

    auto owner_previous_pointer_events = behavior_owner
        ? behavior_owner->pointer_events() : View::PointerEvents::auto_;
    auto owner_state_changed = behavior_owner && behavior_owner != source &&
                               owner_previous_pointer_events !=
                                   View::PointerEvents::auto_;
    const bool same_owner = previous_binding &&
                            previous_owner == behavior_owner;
    if (same_owner) {
        owner_previous_pointer_events =
            previous_binding->owner_previous_pointer_events;
        owner_state_changed = previous_binding->owner_state_changed;
    }
    // Share the retained Canvas2D program for reference/hybrid consumers, but
    // keep the live behavior canvas as the sole native painter. Its command
    // coordinates, client metrics, and pointer mapping all live in the same
    // authored coordinate space. Replaying that program through a captured
    // final-space DesignIR sibling applies the ancestor transform incorrectly
    // and splits paint ownership from event ownership.
    target->share_recorded_commands_from(*source);
    // The anchored CanvasWidget is the live behavior surface. Keep it visible
    // and hit-testable so pointer samples reach the authored wrapper through
    // the normal target-to-root DOM walk. The retained source canvas supplies
    // commands only; remove it from hit testing as well as paint. An opacity-0
    // view is still a valid native hit target, so leaving the source's default
    // PointerEvents::auto_ here would make the hidden behavior canvas win the
    // z-order walk and strand the callbacks on an invisible surface.
    source->set_opacity(0.0f);
    source->set_pointer_events(View::PointerEvents::none);
    target->set_opacity(1.0f);
    target->set_pointer_events(View::PointerEvents::auto_);
    // The source is intentionally hidden from native hit testing even when a
    // React handler is attached directly to the <canvas> itself. Do not undo
    // that exclusion while making an ordinary wrapper owner interactive.
    if (behavior_owner && behavior_owner != source && !same_owner)
        behavior_owner->set_pointer_events(View::PointerEvents::auto_);
    // Input ownership follows paint ownership. The authored behavior canvas
    // remains in the native hit-test tree; the final-space DesignIR sibling is
    // only retained as a hidden diagnostic/reference surface. The two are
    // sibling subtrees, though, so a hit on the retained target would otherwise
    // have no native callback at all. Relay the bridge-safe event slots to the
    // supplied behavior owner. ViewCapture makes the relay follow a replaced
    // React wrapper and become inert when that wrapper is retired.
    //
    // Preserve any callback already installed on the retained target. A target
    // may have been registered by another bridge consumer before this bind;
    // replacing that slot would silently discard its behavior. The relay
    // callable is self-identifying, so repeated binds unwrap its original
    // callback first instead of stacking duplicate relays.
    auto previous_pointer = unwrap_dom_pointer_relay(
        target_view->on_dom_pointer_event);
    auto previous_pointer_move = unwrap_dom_pointer_relay(
        target_view->on_dom_pointer_move_event);
    auto previous_wheel = unwrap_dom_wheel_relay(target_view->on_dom_wheel_event);
    auto previous_click = unwrap_click_relay(target_view->on_click);
    if (behavior_owner) {
        target_view->on_dom_pointer_event = make_dom_pointer_relay(
            root, target_view, source, behavior_owner, false,
            std::move(previous_pointer), source_previous_pointer_events,
            source_previous_opacity, source_state_changed,
            owner_previous_pointer_events, owner_state_changed);
        target_view->on_dom_pointer_move_event = make_dom_pointer_relay(
            root, target_view, source, behavior_owner, true,
            std::move(previous_pointer_move), source_previous_pointer_events,
            source_previous_opacity, source_state_changed,
            owner_previous_pointer_events, owner_state_changed);
        target_view->on_dom_wheel_event = make_dom_wheel_relay(
            root, target_view, behavior_owner, std::move(previous_wheel));
        target_view->on_click = make_click_relay(
            root, target_view, behavior_owner, std::move(previous_click));
    } else {
        // No owner means this call only establishes the paint/input surface;
        // restore the target's pre-bind callbacks rather than clearing them.
        target_view->on_dom_pointer_event = std::move(previous_pointer);
        target_view->on_dom_pointer_move_event = std::move(previous_pointer_move);
        target_view->on_dom_wheel_event = std::move(previous_wheel);
        target_view->on_click = std::move(previous_click);
    }
    return true;
}

} // namespace

void BridgeRegistrars::register_metadata_removal_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // removeWidget(id)
    register_bridge_function(api, "removeWidget", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* w = self.widget(id)) {
            View* parent = w->parent();
            if (parent) {
                auto removed = parent->remove_child(w);
                self.forget_widget_subtree(removed.get());
                self.retire_removed_widget(std::move(removed));
            }
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_metadata_source_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setAnchor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto anchor = args.get<std::string>(1, "");
        if (auto* v = self.widget(id)) v->set_anchor_id(std::move(anchor));
        return choc::value::Value();
    });

    // setVisibleAtAnchor(anchor, visible) -> bool
    //
    // Materialized imports load their executable behavior realm before the
    // DesignIR sibling is attached.  Addressing the later tree by stable
    // anchor lets a native-authority runtime retire Chromium's diagnostic
    // paint plane without knowing generated widget ids.  Ambiguous or missing
    // anchors fail closed so an import cannot hide an arbitrary subtree.
    register_bridge_function(api, "setVisibleAtAnchor", [&self](
        choc::javascript::ArgumentList args) {
        const auto anchor = args.get<std::string>(0, "");
        if (anchor.empty()) return choc::value::createBool(false);
        int matches = 0;
        auto* target = unique_anchor(self.root_, anchor, matches);
        if (matches != 1 || target == nullptr)
            return choc::value::createBool(false);
        target->set_visible(args.get<bool>(1, true));
        return choc::value::createBool(true);
    });

    // bindCanvasBehaviorAt(anchor, sourceRootId, canvasIndex) -> bool
    //
    // Native authority keeps the live authored-space CanvasWidget as the sole
    // painter and input owner. The corresponding final-space DesignIR canvas
    // is hidden but shares the retained program for diagnostics and reference
    // validation; it never participates in hit testing or pointer delivery.
    register_bridge_function(api, "bindCanvasBehaviorAt", [&self](
        choc::javascript::ArgumentList args) {
        const auto anchor = args.get<std::string>(0, "");
        const auto source_root_id = args.get<std::string>(1, "");
        const auto behavior_owner_id = args.get<std::string>(3, "");
        const auto raw_index = args.get<double>(2, -1.0);
        if (anchor.empty() || source_root_id.empty() ||
            !std::isfinite(raw_index) || raw_index < 0.0 ||
            std::floor(raw_index) != raw_index ||
            // Browser capture bounds materialized canvases to 256. Keeping the
            // same public bound makes the double-to-size_t conversion total on
            // every supported architecture.
            raw_index > 255.0)
            return choc::value::createBool(false);
        auto* source_root = self.widget(source_root_id);
        auto* behavior_owner = behavior_owner_id.empty()
            ? nullptr : self.widget(behavior_owner_id);
        const auto contained_by_source_root = [source_root](View* view) {
            for (auto* current = view; current; current = current->parent()) {
                if (current == source_root) return true;
            }
            return false;
        };
        if (behavior_owner && (!source_root ||
            !contained_by_source_root(behavior_owner)))
            return choc::value::createBool(false);
        std::size_t seen = 0;
        auto* source = source_root
            ? canvas_at(*source_root, static_cast<std::size_t>(raw_index), seen)
            : nullptr;
        return choc::value::createBool(
            source && bind_canvas_program(self.root_, anchor, source, source_root,
                                          behavior_owner));
    });

    register_bridge_function(api, "setSource", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto file = args.get<std::string>(1, "");
        auto line = static_cast<int>(args.get<double>(2, 0.0));
        auto col = static_cast<int>(args.get<double>(3, 0.0));
        if (file.empty()) return choc::value::Value();
        if (auto* v = self.widget(id))
            v->set_source_loc({std::move(file), line, col});
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_metadata_computed_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // getComputedValue(id, prop) -> string
    register_bridge_function(api, "getComputedValue", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (!v) return choc::value::createString("");
        if (prop == "width") return choc::value::createString(std::to_string(v->bounds().width) + "px");
        if (prop == "height") return choc::value::createString(std::to_string(v->bounds().height) + "px");
        if (prop == "opacity") return choc::value::createString(std::to_string(v->opacity()));
        if (prop == "display") return choc::value::createString(v->visible() ? "flex" : "none");
        if (prop == "visibility") return choc::value::createString(v->visible() ? "visible" : "hidden");
        return choc::value::createString("");
    });
}

} // namespace pulp::view
