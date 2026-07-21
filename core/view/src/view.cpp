#include <pulp/view/view.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/view/widget_metrics.hpp>
#include <pulp/view/tracing_badge.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/view/motion.hpp>
#include <pulp/view/gesture.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/drag_drop.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/value_source_binding.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/capability_fallback.hpp>
#include <memory>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pulp::view {

// Holds a view's value-source bindings behind one lazily allocated pointer, so
// the common view (no live value) pays a single null pointer rather than two
// bindings' worth of members. Defined before ~View so the unique_ptr member has
// a complete type to destroy.
struct ViewValueBindings {
    explicit ViewValueBindings(View& owner) : meter(owner), scalar(owner) {}
    MeterSourceBinding meter;
    ScalarSourceBinding scalar;
};

namespace {

std::uint64_t next_import_binding_instance_id() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

View* root_for_gesture_relationship_cleanup(View* view) {
    if (!view) return nullptr;
    while (view->parent())
        view = view->parent();
    return view;
}

// Backs View::structure_generation(). Bumped only by remove_child (the sole
// path that detaches a node). Starts at 1 so 0 is a reserved sentinel. Relaxed
// ordering: tree mutation and the cache lookups that read it run on the same
// (UI) thread; the atomic only guards against incidental cross-thread access.
std::atomic<std::uint64_t> g_view_structure_generation{1};

// Process-global count of views with subtree caching enabled (FU-3). The vast
// majority of trees never cache, and invalidate_subtree_caches_up() runs on
// EVERY request_repaint()/set_bounds()/child-mutation — a hot mutation path.
// This counter lets that walk early-out with a single relaxed load when no
// view anywhere has opted in, so the feature adds no measurable cost to trees
// that don't use it. Touched only by set_subtree_cached() and ~View(). Relaxed
// is sufficient: the flag only gates an optimization, and the mutations that
// matter are already ordered by the single-threaded UI paint/mutation model.
std::atomic<int>& subtree_cache_enabled_count() {
    static std::atomic<int> count{0};
    return count;
}

} // namespace

View::View()
    : import_binding_instance_id_(next_import_binding_instance_id()),
      import_binding_lifetime_token_(std::make_shared<const std::uint64_t>(
          import_binding_instance_id_)) {}

// View destructor stays out-of-line while remaining virtual + public so vtable
// layout + SDK contract are unchanged.
View::~View() {
    if (gesture_arbiter_)
        gesture_arbiter_->reset();
    // Clear the overlay + focus slots if this dying View holds them. Without
    // this, an unmounted React popover / focused widget leaves a dangling
    // pointer that the platform window host would dereference on the next click
    // or keypress (crash via dynamic_cast<TextEditor*> on freed memory in
    // -[PulpView focusedTextEditor]). Clear BOTH the root-owned slot (S11) and
    // the process-global shim mirror. Uses existing_interaction() so a tree that
    // never allocated a state block is not forced to allocate one during
    // teardown. (~ComboBox does the same for active_popup.)
    if (RootInteractionState* s = existing_interaction()) {
        if (s->focused_input == this) s->focused_input = nullptr;
        if (s->active_overlay == this) s->active_overlay = nullptr;
    }
    if (active_overlay_ == this) active_overlay_ = nullptr;
    if (focused_input_ == this) focused_input_ = nullptr;
    // Cancel any running animate() tweens so their FrameClock callbacks (which
    // capture `this`) can't fire after this View is gone.
    // Unsubscribe against the CACHED clock (not frame_clock()): a child detached
    // via remove_child has parent_==null, so frame_clock() would return null and
    // leave the root's still-live subscription firing on freed memory.
    for (const auto& a : animations_) {
        if (a.clock) a.clock->unsubscribe(a.clock_id);
    }
    animations_.clear();
    // Keep the process-global "any view caching?" counter balanced. Uses the
    // member directly (not set_subtree_cached, which would request a repaint on
    // a dying view). Parent-cache invalidation for a removed subtree is handled
    // in remove_child while parent_ is still live.
    if (subtree_cached_)
        subtree_cache_enabled_count().fetch_sub(1, std::memory_order_relaxed);
}

int View::animate(std::function<void(float)> apply, float from, float to,
                  float duration_s, std::function<float(float)> ease,
                  std::function<void()> on_done, const std::string& tag) {
    FrameClock* fc = frame_clock();
    if (!fc || !apply) return -1;
    if (!ease) ease = easing::linear;
    if (!tag.empty()) {
        // Self-cancelling: drop any prior animation sharing this tag.
        for (int i = static_cast<int>(animations_.size()) - 1; i >= 0; --i) {
            if (animations_[i].tag == tag) {
                fc->unsubscribe(animations_[i].clock_id);
                animations_.erase(animations_.begin() + i);
            }
        }
    }
    apply(from);  // seed the start value so there's no one-frame delay
    auto elapsed = std::make_shared<float>(0.0f);
    auto id_slot = std::make_shared<int>(-1);
    const int cid = fc->subscribe(
        [this, apply, from, to, duration_s, ease, on_done, elapsed, id_slot](float dt) -> bool {
            *elapsed += dt;
            const float t = duration_s > 0.0f
                                ? std::clamp(*elapsed / duration_s, 0.0f, 1.0f)
                                : 1.0f;
            apply(from + (to - from) * ease(t));
            request_repaint();
            if (t >= 1.0f) {
                // Reached the target: forget the record and fire on_done. Return
                // false so the FrameClock auto-unsubscribes this callback.
                for (int i = static_cast<int>(animations_.size()) - 1; i >= 0; --i) {
                    if (animations_[i].clock_id == *id_slot) {
                        animations_.erase(animations_.begin() + i);
                        break;
                    }
                }
                if (on_done) on_done();
                return false;
            }
            return true;
        });
    *id_slot = cid;
    animations_.push_back({cid, tag, fc});
    return cid;
}

void View::cancel_animation(int id) {
    if (id < 0) return;
    for (int i = static_cast<int>(animations_.size()) - 1; i >= 0; --i) {
        if (animations_[i].clock_id == id) {
            if (animations_[i].clock) animations_[i].clock->unsubscribe(id);
            animations_.erase(animations_.begin() + i);
            return;
        }
    }
}

// ── Tracing badge ────────────────────────────────────────────────────────
// Process-global visibility flag for the "◉ TRACING" reminder painted by the
// root View in a PULP_TRACING=ON build. Default visible; a golden-screenshot
// harness can suppress it. Stored in a function-local static so the flag has no
// static-init ordering dependency.
namespace {
std::atomic<bool>& tracing_badge_visible_flag() {
    static std::atomic<bool> visible{true};
    return visible;
}
}  // namespace
// warn_capability_fallback_once lives in <pulp/view/capability_fallback.hpp>
// (the one shared warn-once vocabulary, also used by ImageView).

bool tracing_badge_should_paint() {
    return pulp::runtime::kTracingEnabled
           && tracing_badge_visible_flag().load(std::memory_order_relaxed);
}

void set_tracing_badge_visible(bool visible) {
    tracing_badge_visible_flag().store(visible, std::memory_order_relaxed);
}

// ── Subtree scene cache (FU-3) ───────────────────────────────────────────
void View::set_subtree_cached(bool v) {
    if (subtree_cached_ == v) return;
    subtree_cached_ = v;
    if (v) {
        subtree_cache_enabled_count().fetch_add(1, std::memory_order_relaxed);
    } else {
        subtree_cache_enabled_count().fetch_sub(1, std::memory_order_relaxed);
        // Drop any recorded scene so re-enabling later starts fresh, and so the
        // shared_ptr does not pin a stale recording.
        scene_cache_.reset();
        scene_cache_valid_ = false;
    }
    // Turning caching on or off changes what the next frame must do; make sure
    // a repaint is on the way.
    request_repaint();
}

void View::invalidate_subtree_caches_up() {
    // Fast path: nobody caches anywhere → nothing to invalidate. One relaxed
    // load, no tree walk.
    if (subtree_cache_enabled_count().load(std::memory_order_relaxed) == 0)
        return;
    // A mutation at THIS view stales the recording of every cached ancestor
    // (their pictures include this view's content) and of this view itself if
    // it caches. Walk up parent_ links clearing validity; O(depth).
    for (View* v = this; v; v = v->parent_) {
        if (v->subtree_cached_)
            v->scene_cache_valid_ = false;
    }
}

void View::simulate_click(Point root_pos) {
    auto* target = hit_test(root_pos);
    // Record the synthetic input into the active motion fixture before
    // dispatch so replay sees the same target lookup the original recording
    // captured: target id is what we resolve, not "wherever this click would
    // land at replay time".
    if (pulp::view::motion::input_recording_enabled()) {
        const std::string id = target ? target->id() : std::string();
        std::vector<std::pair<std::string, double>> coords;
        coords.emplace_back("x", static_cast<double>(root_pos.x));
        coords.emplace_back("y", static_cast<double>(root_pos.y));
        pulp::view::motion::record_simulated_input("click", id, std::move(coords));
    }
    if (!target) return;

    MouseEvent down;
    down.position = root_pos;
    down.window_position = root_pos;
    down.button = MouseButton::left;
    down.is_down = true;
    down.phase = MousePhase::press;
    const bool gesture_down = dispatch_gesture_pointer_event(down);

    MouseEvent up = down;
    up.is_down = false;
    up.phase = MousePhase::release;
    const bool gesture_up = dispatch_gesture_pointer_event(up);
    if (gesture_down || gesture_up) return;

    // DOM-style click bubbling. `hit_test` returns the deepest
    // hit-testable view, which for `<button onClick=...>Label</button>` is
    // the inner Label child. Walk up the parent chain to find the nearest
    // ancestor with a registered handler.
    //
    // Bound the bubble walk to `this` (inclusive). Walking past the receiver
    // into ancestors outside its subtree leaks synthetic clicks across
    // component boundaries, a false-positive hazard for tests / tooling that
    // simulate interaction on isolated subtrees. `deliver_mouse_up` resolves
    // the same handler with an UNbounded walk (the hosts always dispatch from
    // the window root, where there is nothing above to leak into), so resolve
    // ours here and have the fire-click hook use it instead.
    View* click_target = target;
    while (click_target && !click_target->on_click) {
        if (click_target == this) break;  // stop at receiver, even if no handler
        click_target = click_target->parent();
    }
    auto bounded_click = click_target ? click_target->on_click : std::function<void()>{};

    if (!deliver_mouse_down(*this, target, root_pos, /*modifiers=*/0)) return;
    MouseUpHost up_host;
    up_host.fire_click = [&bounded_click](const std::function<void()>&,
                                          const std::string&, uint16_t) {
        if (bounded_click) bounded_click();
    };
    deliver_mouse_up(*this, target, root_pos, /*modifiers=*/0, /*click_count=*/1, up_host);
}

void View::simulate_drag(Point start, Point end, int steps) {
    auto* target = hit_test(start);
    if (pulp::view::motion::input_recording_enabled()) {
        const std::string id = target ? target->id() : std::string();
        std::vector<std::pair<std::string, double>> coords;
        // Recorded coordinates are keyed fields; insertion order is not semantic.
        coords.emplace_back("end_x",   static_cast<double>(end.x));
        coords.emplace_back("end_y",   static_cast<double>(end.y));
        coords.emplace_back("start_x", static_cast<double>(start.x));
        coords.emplace_back("start_y", static_cast<double>(start.y));
        coords.emplace_back("steps",   static_cast<double>(steps));
        pulp::view::motion::record_simulated_input("drag", id, std::move(coords));
    }
    if (!target) return;

    MouseEvent down;
    down.position = start;
    down.window_position = start;
    down.button = MouseButton::left;
    down.is_down = true;
    down.phase = MousePhase::press;
    bool gesture_consumed = dispatch_gesture_pointer_event(down);

    if (gesture_consumed) {
        for (int i = 1; i <= steps; ++i) {
            float t = static_cast<float>(i) / steps;
            Point p = {start.x + (end.x - start.x) * t,
                       start.y + (end.y - start.y) * t};
            MouseEvent move;
            move.position = p;
            move.window_position = p;
            move.button = MouseButton::left;
            move.is_down = true;
            move.phase = MousePhase::drag;
            dispatch_gesture_pointer_event(move);
        }
        MouseEvent up;
        up.position = end;
        up.window_position = end;
        up.button = MouseButton::left;
        up.is_down = false;
        up.phase = MousePhase::release;
        dispatch_gesture_pointer_event(up);
        return;
    }

    if (!deliver_mouse_down(*this, target, start, /*modifiers=*/0)) return;
    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / steps;
        Point p = {start.x + (end.x - start.x) * t,
                   start.y + (end.y - start.y) * t};
        MouseEvent move;
        move.position = p;
        move.window_position = p;
        move.button = MouseButton::left;
        move.is_down = true;
        move.phase = MousePhase::drag;
        gesture_consumed = dispatch_gesture_pointer_event(move) || gesture_consumed;
        deliver_mouse_drag(*this, target, p, /*modifiers=*/0);
    }
    MouseEvent up;
    up.position = end;
    up.window_position = end;
    up.button = MouseButton::left;
    up.is_down = false;
    up.phase = MousePhase::release;
    gesture_consumed = dispatch_gesture_pointer_event(up) || gesture_consumed;
    if (gesture_consumed) return;
    // Release delivery only, with no fire-click hook. The hosts DO fire a click
    // when a release lands on the press target, even after a drag; this
    // simulator keeps drag and click separable so a test can drive one without
    // the other. `simulate_click` is the verb that fires a click.
    deliver_mouse_up(*this, target, end, /*modifiers=*/0, /*click_count=*/1, MouseUpHost{});
}

static void collect_focusable(View& root, std::vector<View*>& out) {
    if (root.focusable()) out.push_back(&root);
    for (size_t i = 0; i < root.child_count(); ++i)
        collect_focusable(*root.child_at(i), out);
}

View* View::focus_next(View& root, View* current) {
    std::vector<View*> focusable;
    collect_focusable(root, focusable);
    if (focusable.empty()) return nullptr;

    if (!current) {
        focusable[0]->set_focus(true);
        return focusable[0];
    }

    current->set_focus(false);
    for (size_t i = 0; i < focusable.size(); ++i) {
        if (focusable[i] == current) {
            auto* next = focusable[(i + 1) % focusable.size()];
            next->set_focus(true);
            return next;
        }
    }
    focusable[0]->set_focus(true);
    return focusable[0];
}

View* View::focus_prev(View& root, View* current) {
    std::vector<View*> focusable;
    collect_focusable(root, focusable);
    if (focusable.empty()) return nullptr;

    if (!current) {
        focusable.back()->set_focus(true);
        return focusable.back();
    }

    current->set_focus(false);
    for (size_t i = 0; i < focusable.size(); ++i) {
        if (focusable[i] == current) {
            auto* prev = focusable[(i + focusable.size() - 1) % focusable.size()];
            prev->set_focus(true);
            return prev;
        }
    }
    focusable.back()->set_focus(true);
    return focusable.back();
}

void View::set_bounds(Rect r) {
    if (bounds_ == r) return;
    bounds_ = r;
    // Resize/move re-records: the recording was captured at the old size (and a
    // parent's recording placed this view at its old box). Stale this view and
    // its cached ancestors before on_resized() drives the repaint.
    invalidate_subtree_caches_up();
    on_resized();
}

void View::prepare_for_reuse() {
    // A pooled view is never attached to a live tree. Firing this on a parented
    // view would mean the pool is recycling something still wired into a paint /
    // hit-test path — a logic error, not a recoverable state.
    assert(parent_ == nullptr && "prepare_for_reuse() on a view that still has a parent");

    // Geometry / visibility / compositing — reset directly (not via setters) to
    // avoid on_resized()/request_repaint() side effects on a detached view.
    bounds_ = Rect{};
    visible_ = true;
    opacity_ = 1.0f;

    // Accessibility identity from the previous binding must not leak into the
    // next occupant of this slot.
    access_role_ = AccessRole::none;
    access_label_.clear();
    derived_access_label_.clear();
    access_value_.clear();
    access_pressed_.clear();
    access_checked_.clear();
    access_disabled_.clear();
    access_hidden_.clear();

    // Pointer / hover / focus interaction state.
    hovered_ = false;
    has_focus_ = false;
    captured_pointers_.clear();

    // A recycled view must not remain the process-global overlay owner; the
    // static back-pointer would otherwise dangle at a parked instance.
    release_overlay();

    // Clear EVERY base-class callback. A recycled view that keeps a stale
    // std::function fires it into freed/torn-down closure state on the next
    // interaction — the exact use-after-free this reset exists to prevent
    // (Codex must-fix #5). Subclass callbacks are the subclass override's job.
    on_click = nullptr;
    on_pointer_event = nullptr;
    on_drag = nullptr;
    on_pointer_move = nullptr;
    on_gesture_cb = nullptr;
    on_context_menu = nullptr;
    on_drop = nullptr;
    on_hover_enter = nullptr;
    on_hover_leave = nullptr;
    on_overlay_dismissed = nullptr;
    on_global_click = nullptr;
    on_global_key = nullptr;
}

void View::set_window_host(WindowHost* host) {
    window_host_ = host;
    for (auto& child : children_) {
        child->set_window_host(host);
    }
}

void View::set_plugin_view_host(PluginViewHost* host) {
    plugin_view_host_ = host;
    for (auto& child : children_) {
        child->set_plugin_view_host(host);
    }
}

void View::set_host_params(HostParamSurface* surface) {
    host_params_ = surface;
    for (auto& child : children_) {
        child->set_host_params(surface);
    }
}

void View::set_host_actions(HostActionSurface* surface) {
    host_actions_ = surface;
    for (auto& child : children_) {
        child->set_host_actions(surface);
    }
}

void View::add_child(std::unique_ptr<View> child) {
    child->parent_ = this;
    child->set_window_host(window_host_);
    child->set_plugin_view_host(plugin_view_host_);
    child->set_host_params(host_params_);
    child->set_host_actions(host_actions_);
    children_.push_back(std::move(child));
    children_.back()->on_attached();
    // Structural change: this view's (and its cached ancestors') recording no
    // longer includes the new child. Stale them so the next frame re-records.
    invalidate_subtree_caches_up();
    // If this parent can already reach a FrameClock, tell the newly-grafted
    // subtree so a self-subscribing descendant (a live Meter built offline)
    // attaches to the already-present clock instead of silently missing it.
    if (frame_clock()) {
        children_.back()->notify_frame_clock_changed();
    }
}

std::unique_ptr<View> View::remove_child(View* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
        [child](const auto& p) { return p.get() == child; });
    if (it == children_.end()) return nullptr;

    std::vector<GestureRecognizer*> removed_recognizers;
    auto collect_removed = [&](auto&& self, View& node) -> void {
        for (auto& recognizer : node.gesture_recognizers_) {
            if (recognizer)
                removed_recognizers.push_back(recognizer.get());
        }
        for (auto& descendant : node.children_)
            self(self, *descendant);
    };
    collect_removed(collect_removed, *child);

    for (View* root = this; root; root = root->parent_) {
        if (root->gesture_arbiter_)
            root->gesture_arbiter_->reset();
    }
    auto scrub_relationships = [&](auto&& self, View& node) -> void {
        for (auto& recognizer : node.gesture_recognizers_) {
            if (!recognizer) continue;
            for (auto* removed : removed_recognizers) {
                if (removed && removed != recognizer.get())
                    recognizer->remove_relationships_to(*removed);
            }
        }
        for (auto& descendant : node.children_)
            self(self, *descendant);
    };
    scrub_relationships(scrub_relationships, *this);

    child->on_detached();
    child->set_window_host(nullptr);
    child->set_plugin_view_host(nullptr);
    child->set_host_params(nullptr);
    child->set_host_actions(nullptr);
    child->parent_ = nullptr;
    // The removed subtree can no longer reach this parent's clock. Notify it so
    // self-subscribing descendants (a live Meter that never got its own
    // on_detached — remove_child only fires that on the removed root) drop their
    // subscription now instead of lingering until the next tick.
    child->notify_frame_clock_changed();
    auto owned = std::move(*it);
    children_.erase(it);
    // A node was detached: invalidate every external liveness cache keyed on the
    // structure generation (see View::structure_generation()).
    g_view_structure_generation.fetch_add(1, std::memory_order_relaxed);
    // Structural change: this view's (and its cached ancestors') recording still
    // includes the now-removed child. Stale them so the next frame re-records.
    invalidate_subtree_caches_up();
    return owned;
}

std::uint64_t View::structure_generation() noexcept {
    return g_view_structure_generation.load(std::memory_order_relaxed);
}

bool View::children_in_z_order() const {
    // True when children_ is already non-decreasing in z_index(), i.e. a
    // stable_sort by z would be the identity and paint/hit-test can iterate
    // children_ directly without allocating a sorted copy. A single linear
    // scan, no allocation.
    for (std::size_t i = 1; i < children_.size(); ++i) {
        if (children_[i]->z_index() < children_[i - 1]->z_index()) return false;
    }
    return true;
}

std::vector<View*> View::sorted_children_by_z_index() const {
    std::vector<View*> result;
    result.reserve(children_.size());
    for (const auto& child : children_) result.push_back(child.get());
    // Stable sort so siblings with equal z_index() retain insertion order
    // (CSS painting-order rule).
    std::stable_sort(result.begin(), result.end(),
        [](const View* a, const View* b) {
            return a->z_index() < b->z_index();
        });
    return result;
}

View* View::hit_test(Point local_point) {
    if (!visible_ || !enabled_ || !hit_testable_) return nullptr;

    // React Native pointerEvents:
    //   none      — neither this view nor children intercept events.
    //   box_none  — this view is invisible to hit-testing but children
    //               can still receive events (descend, but never return self).
    //   box_only  — this view receives events; children do NOT
    //               (skip the descent below, then check own bounds).
    //   auto_     — default behavior.
    if (pointer_events_ == PointerEvents::none) return nullptr;

    // Check children topmost-first. With z-index honored,
    // "topmost" means highest z_index — and at equal z, latest insertion
    // — so iterate the z-sorted paint order in reverse. Without this,
    // a high-z popover could render on top yet have clicks fall through
    // to siblings beneath it.
    if (pointer_events_ != PointerEvents::box_only) {
        // Test one child; returns the hit (or nullptr to keep looking).
        auto try_child = [&](View* child) -> View* {
            if (!child->visible_) return nullptr;

            Point child_point = {local_point.x - child->bounds_.x,
                                local_point.y - child->bounds_.y};

            // For overflow:visible, expand the hit area on all four sides
            // to include content that extends beyond the child's bounds
            // (e.g. dropdowns/popovers that grow downward, leftward, etc.).
            // The 500px slack is symmetric so popovers that extend in any
            // direction get hit-tested correctly.
            bool in_bounds = child->local_bounds().contains(child_point);
            if (!in_bounds && child->overflow() == Overflow::visible) {
                auto lb = child->local_bounds();
                in_bounds = child_point.x >= lb.x - 500 &&
                            child_point.x <= lb.x + lb.width + 500 &&
                            child_point.y >= lb.y - 500 &&
                            child_point.y <= lb.y + lb.height + 500;
            }

            if (in_bounds) {
                if (auto* hit = child->hit_test(child_point)) return hit;
            }
            return nullptr;
        };

        // Topmost-first = highest z, latest insertion at equal z → reverse of
        // the z-sorted order. When children are already in z-order (the common
        // case), reverse-walking children_ is identical to reversing the
        // stable-sorted copy, so skip the per-hit-test allocation — mirrors
        // paint_all's fast path (children_in_z_order()).
        if (children_in_z_order()) {
            for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
                if (auto* hit = try_child(it->get())) return hit;
            }
        } else {
            auto paint_order = sorted_children_by_z_index();
            for (auto it = paint_order.rbegin(); it != paint_order.rend(); ++it) {
                if (auto* hit = try_child(*it)) return hit;
            }
        }
    }

    // No child was hit — return this view if the point is within bounds.
    // box_none suppresses self-targeting even when a child miss falls back
    // here, matching RN's "container is just a layout pass-through" mode.
    if (pointer_events_ == PointerEvents::box_none) return nullptr;

    if (local_bounds().contains(local_point))
        return this;

    return nullptr;
}

// ── Overlay paint queue ──────────────────────────────────────────────────────

namespace {
// True when `v` is not part of a multi-node tree and therefore has no root of
// its own to own the interaction state. Such a widget shares the process-global
// fallback with every other detached widget (see interaction() below).
bool is_detached_widget(const View* v) {
    return v->parent() == nullptr && v->child_count() == 0;
}

// The single process-global fallback interaction block — used by detached
// widgets and, for the overlay paint queue, by legacy `overlay_queue()` callers.
View::RootInteractionState& fallback_interaction() {
    static View::RootInteractionState state;
    return state;
}
} // namespace

std::vector<View::OverlayRequest>& View::overlay_queue() {
    // Legacy process-global entry point. Now backed by the fallback interaction
    // block so a detached widget that enqueues via interaction().overlay_queue
    // and a legacy overlay_queue() caller share ONE queue. Root-aware code
    // (parented ComboBox paint, the standalone inspector idle) enqueues onto the
    // owning root's queue via interaction() instead; paint_overlays drains the
    // painting root's queue.
    return fallback_interaction().overlay_queue;
}

// Inspector hooks — set by the inspector module via function pointers
// to avoid circular dependency (view → inspect).
static std::function<void(canvas::Canvas&, View*)> s_inspector_paint_hook;
static std::function<bool(const KeyEvent&)> s_inspector_key_hook;
// Mouse/text/cursor hooks carry the event's root View, mirroring the paint
// hook's painting_root, so the installed hook can gate to the inspected canvas
// root and ignore a secondary window's events.
static std::function<bool(const MouseEvent&, View*)> s_inspector_mouse_hook;
static std::function<bool(const TextInputEvent&, View*)> s_inspector_text_hook;

void View::set_inspector_paint_hook(
    std::function<void(canvas::Canvas&, View*)> hook) {
    s_inspector_paint_hook = std::move(hook);
}
void View::set_inspector_key_hook(std::function<bool(const KeyEvent&)> hook) {
    s_inspector_key_hook = std::move(hook);
}
void View::set_inspector_mouse_hook(
    std::function<bool(const MouseEvent&, View*)> hook) {
    s_inspector_mouse_hook = std::move(hook);
}
bool View::call_inspector_key_hook(const KeyEvent& e) {
    return s_inspector_key_hook ? s_inspector_key_hook(e) : false;
}
bool View::call_inspector_mouse_hook(const MouseEvent& e, View* event_root) {
    return s_inspector_mouse_hook ? s_inspector_mouse_hook(e, event_root)
                                  : false;
}
void View::set_inspector_text_hook(
    std::function<bool(const TextInputEvent&, View*)> hook) {
    s_inspector_text_hook = std::move(hook);
}
bool View::call_inspector_text_hook(const TextInputEvent& e, View* event_root) {
    return s_inspector_text_hook ? s_inspector_text_hook(e, event_root) : false;
}

static std::function<int(const MouseEvent&, View*)> s_inspector_cursor_hook;
void View::set_inspector_cursor_hook(
    std::function<int(const MouseEvent&, View*)> hook) {
    s_inspector_cursor_hook = std::move(hook);
}
int View::call_inspector_cursor_hook(const MouseEvent& e, View* event_root) {
    return s_inspector_cursor_hook ? s_inspector_cursor_hook(e, event_root)
                                   : -1;
}

// Generalized overlay-click routing.
View* View::active_overlay_ = nullptr;

// Global input-focus slot. Auto-cleared by ~View() when the focused widget is
// destroyed, preventing use-after-free in the platform window host's keyDown
// handler.
View* View::focused_input_ = nullptr;

// Dismiss-path release. Pulls the slot, then fires the dismissed View's
// `on_overlay_dismissed` callback so React state can sync. Order matters:
// clear the slot first so a callback that calls claim_overlay() on a
// replacement popover doesn't immediately get nulled out by our subsequent
// clear.
void View::dismiss_active_overlay() {
    // Static entry point (ESC / outside-click) has no root in hand, so it acts
    // on the process-global shim mirror: the most-recently-claimed overlay. Clear
    // BOTH the mirror and the victim's root-owned slot (S11) BEFORE the callback,
    // so a replacement popover the callback claims survives our clears.
    View* victim = active_overlay_;
    if (!victim) return;
    active_overlay_ = nullptr;
    if (RootInteractionState* s = victim->existing_interaction();
        s && s->active_overlay == victim)
        s->active_overlay = nullptr;
    if (victim->on_overlay_dismissed) {
        victim->on_overlay_dismissed();
    }
}

// Recursively expand a child's painted-bounds contribution
// up through any `overflow:visible` descendants. Returns the bounding
// rect (in window coords) of `v` and every transitive descendant whose
// chain back to `v` is entirely overflow:visible. A descendant inside
// an `overflow:hidden` ancestor is clipped, so it stops contributing.
//
// `parent_abs_x` / `parent_abs_y` are the absolute window-coord origin
// of `v->parent()`. The function consumes those, applies `v`'s own
// `bounds().x/y`, and recurses. Declared in view.hpp so ScrollView::hit_test
// (a separate TU) can share the exact same extent math.
void accumulate_overflow_extent(const View* v,
                                float parent_abs_x,
                                float parent_abs_y,
                                float& min_x,
                                float& min_y,
                                float& max_x,
                                float& max_y) {
    if (!v) return;
    const float abs_x = parent_abs_x + v->bounds().x;
    const float abs_y = parent_abs_y + v->bounds().y;
    const auto lb = v->local_bounds();
    if (abs_x < min_x) min_x = abs_x;
    if (abs_y < min_y) min_y = abs_y;
    if (abs_x + lb.width > max_x) max_x = abs_x + lb.width;
    if (abs_y + lb.height > max_y) max_y = abs_y + lb.height;
    // Only recurse through children whose own overflow is visible —
    // that's the CSS rule. An `overflow:hidden` child clips its own
    // descendants, so they don't contribute painted pixels above us.
    if (v->overflow() != View::Overflow::visible) return;
    for (size_t i = 0; i < v->child_count(); ++i) {
        accumulate_overflow_extent(v->child_at(i), abs_x, abs_y,
                                   min_x, min_y, max_x, max_y);
    }
}

bool View::overlay_contains(Point window_pt) const {
    // Walk up to compute absolute origin in window/root coords. Same
    // arithmetic the mac window-host uses for ComboBox::active_popup_.
    float abs_x = 0.0f, abs_y = 0.0f;
    const View* v = this;
    while (v) {
        abs_x += v->bounds().x;
        abs_y += v->bounds().y;
        v = v->parent();
    }
    const float w = local_bounds().width;
    const float h = local_bounds().height;
    // Fast-path: own painted rect contains the point.
    if (window_pt.x >= abs_x && window_pt.x <= abs_x + w &&
        window_pt.y >= abs_y && window_pt.y <= abs_y + h) {
        return true;
    }

    // Extend the hit area to include the painted bounding box of any
    // `overflow:visible` descendants. CSS `overflow:visible`
    // semantics: a child painting outside the parent is still
    // visible/clickable. Without this, a popover positioned via
    // `position:absolute; top: 28; right: 0` extends LEFTWARD beyond
    // its short trigger button — clicks on the leftward cells then
    // miss `overlay_contains` and fall through to whatever sibling
    // happens to occupy that pixel.
    //
    // Only meaningful when this overlay itself has overflow:visible
    // (otherwise its own clip rect bounds the painted pixels).
    if (overflow() != Overflow::visible) return false;

    // Compute parent_abs_{x,y}: this->bounds().x/y were already added
    // by the walk above, so subtract them to get the parent origin.
    const float parent_abs_x = abs_x - bounds().x;
    const float parent_abs_y = abs_y - bounds().y;
    float min_x = abs_x, min_y = abs_y;
    float max_x = abs_x + w, max_y = abs_y + h;
    accumulate_overflow_extent(this, parent_abs_x, parent_abs_y,
                               min_x, min_y, max_x, max_y);
    return window_pt.x >= min_x && window_pt.x <= max_x &&
           window_pt.y >= min_y && window_pt.y <= max_y;
}

void View::paint_overlays(canvas::Canvas& canvas, View* painting_root) {
    // Drain the queue owned by the root being painted (S11): a parented
    // ComboBox enqueues its dropdown onto its own root's queue, so a second
    // hosted editor's paint pass never draws editor A's overlays. When the root
    // is unknown (nullptr — legacy/headless callers) fall back to the shared
    // process-global queue. `overlay_queue()` is that same fallback block, so a
    // detached widget's enqueue is drained here too.
    auto& queue = painting_root ? painting_root->interaction().overlay_queue
                                : overlay_queue();
    for (auto& req : queue) {
        if (req.paint_fn) req.paint_fn(canvas);
    }
    queue.clear();

    // Inspector paint hook — called after all overlays, topmost layer.
    // `painting_root` is forwarded so the hook can gate to the inspected root:
    // the in-canvas overlay must paint its selection box / handles / drop
    // indicators only on that root, never into the floating InspectorWindow's
    // own root at the overlay's root coordinates.
    if (s_inspector_paint_hook) {
        s_inspector_paint_hook(canvas, painting_root);
    }
}

// ── In-tree drag source ──────────────────────────────────────────────────────
//
// The drag record lives on the ROOT so every view in the tree agrees on which
// drag is in flight, and so a second tree in the same process cannot see it.

namespace {
View* tree_root(View* v) {
    while (v != nullptr && v->parent() != nullptr) v = v->parent();
    return v;
}
} // namespace

bool View::start_drag(DropData data) {
    View* root = tree_root(this);
    if (root == nullptr) return false;
    // An empty payload is a no-op, not a drag: a receiver that gets handed
    // nothing has no way to decline meaningfully.
    if (data.file_paths.empty() && data.text.empty() && data.custom_data.empty())
        return false;
    root->active_drag_ = std::make_unique<ActiveDrag>();
    root->active_drag_->data = std::move(data);
    root->active_drag_->source = this;
    root->active_drag_->active = true;
    return true;
}

void View::drag_to(Point root_pos) {
    View* root = tree_root(this);
    if (root == nullptr || !root->active_drag_ || !root->active_drag_->active) return;
    auto& d = *root->active_drag_;
    // enter and move share one entry point: dispatch_drag_enter is documented as
    // idempotent, and it is what maintains the hover/leave transitions.
    dispatch_drag_enter(*root, d.session, d.data, root_pos);
}

bool View::drop_here(Point root_pos) {
    View* root = tree_root(this);
    if (root == nullptr || !root->active_drag_ || !root->active_drag_->active) return false;
    auto d = std::move(root->active_drag_);
    root->active_drag_.reset();          // the drag is over before the receiver runs,
                                         // so a receiver may start a new one
    return dispatch_drop(*root, d->session, d->data, root_pos);
}

void View::cancel_drag() {
    View* root = tree_root(this);
    if (root == nullptr || !root->active_drag_) return;
    dispatch_drag_exit(*root, root->active_drag_->session);
    root->active_drag_.reset();
}

bool View::drag_active() const {
    const View* root = this;
    while (root->parent_ != nullptr) root = root->parent_;
    return root->active_drag_ != nullptr && root->active_drag_->active;
}

// ── Per-root interaction state (S11) ─────────────────────────────────────────
//
// See the RootInteractionState comment in view.hpp. The state lives on the tree
// root (like `active_drag_`) so two hosted editors never share focus / overlay /
// popup slots. A DETACHED widget — one with no parent AND no children, so it is
// not part of any tree that could own the state — resolves to a single
// process-global fallback. That fallback preserves the pre-S11 single-focus /
// single-popup behavior for unhosted widgets and is what the headless
// characterization smoke (unparented widgets) observes through the shim mirrors.
// (is_detached_widget / fallback_interaction are defined above, next to
// overlay_queue() which shares the fallback block's queue.)

View::RootInteractionState& View::interaction() {
    if (is_detached_widget(this)) return fallback_interaction();
    View* root = tree_root(this);
    if (!root->interaction_state_)
        root->interaction_state_ = std::make_unique<RootInteractionState>();
    return *root->interaction_state_;
}

const View::RootInteractionState& View::interaction() const {
    return const_cast<View*>(this)->interaction();
}

View::RootInteractionState* View::existing_interaction() {
    if (is_detached_widget(this)) return &fallback_interaction();
    View* root = tree_root(this);
    return root->interaction_state_.get();  // nullptr if never allocated
}

void View::claim_input_focus() {
    interaction().focused_input = this;
    focused_input_ = this;  // process-global shim mirror
}

void View::release_input_focus() {
    if (RootInteractionState* s = existing_interaction();
        s && s->focused_input == this)
        s->focused_input = nullptr;
    if (focused_input_ == this) focused_input_ = nullptr;  // shim mirror
}

void View::claim_overlay() {
    interaction().active_overlay = this;
    active_overlay_ = this;  // process-global shim mirror
}

void View::release_overlay() {
    if (RootInteractionState* s = existing_interaction();
        s && s->active_overlay == this)
        s->active_overlay = nullptr;
    if (active_overlay_ == this) active_overlay_ = nullptr;  // shim mirror
}

void View::set_painter(std::shared_ptr<WidgetPainter> p) {
    painter_ = std::move(p);
    request_repaint();
}

WidgetPainter* View::effective_painter() const {
    for (const View* v = this; v != nullptr; v = v->parent_)
        if (v->painter_) return v->painter_.get();
    return nullptr;
}

namespace {
void invalidate_layout_subtree(View& v) {
    v.invalidate_layout();
    for (size_t i = 0; i < v.child_count(); ++i) invalidate_layout_subtree(*v.child_at(i));
}
}  // namespace

void View::set_metrics(std::shared_ptr<WidgetMetrics> m) {
    metrics_ = std::move(m);
    // A metric change is a SIZE change, so the subtree must re-measure, not
    // just repaint. This is the whole reason metrics and painters are separate
    // objects: installing a painter is a paint invalidation, installing metrics
    // is a layout invalidation.
    invalidate_layout_subtree(*this);
    request_repaint();
}

WidgetMetrics* View::effective_metrics() const {
    for (const View* v = this; v != nullptr; v = v->parent_)
        if (v->metrics_) return v->metrics_.get();
    return nullptr;
}

Color View::resolve_color(const std::string& name, Color fallback) const {
    auto c = theme_.color(name);
    if (c.has_value()) return c.value();
    if (parent_) return parent_->resolve_color(name, fallback);
    return fallback;
}

float View::resolve_dimension(const std::string& name, float fallback) const {
    auto d = theme_.dimension(name);
    if (d.has_value()) return d.value();
    if (parent_) return parent_->resolve_dimension(name, fallback);
    return fallback;
}

// ── CSS-style typography inheritance ─────────────────────────────────────
//
// Each inheritable_*() walks the chain own → parent → … → root, returning
// the first ancestor that has a value. nullopt means no one in the chain
// set the field, so the caller falls back to the theme/widget default.

std::optional<Color> View::inheritable_text_color() const {
    if (inh_text_color_.has_value()) return inh_text_color_;
    if (parent_) return parent_->inheritable_text_color();
    return std::nullopt;
}

std::optional<float> View::inheritable_font_size() const {
    if (inh_font_size_.has_value()) return inh_font_size_;
    if (parent_) return parent_->inheritable_font_size();
    return std::nullopt;
}

std::optional<float> View::inheritable_letter_spacing() const {
    if (inh_letter_spacing_.has_value()) return inh_letter_spacing_;
    if (parent_) return parent_->inheritable_letter_spacing();
    return std::nullopt;
}

std::optional<int> View::inheritable_font_weight() const {
    if (inh_font_weight_.has_value()) return inh_font_weight_;
    if (parent_) return parent_->inheritable_font_weight();
    return std::nullopt;
}

std::optional<std::string> View::inheritable_font_family() const {
    if (inh_font_family_.has_value()) return inh_font_family_;
    if (parent_) return parent_->inheritable_font_family();
    return std::nullopt;
}

std::optional<int> View::inheritable_text_align() const {
    if (inh_text_align_.has_value()) return inh_text_align_;
    if (parent_) return parent_->inheritable_text_align();
    return std::nullopt;
}

// ── Pointer capture ─────────────────────────────────────────────────────

GestureRecognizer& View::add_gesture_recognizer(
        std::unique_ptr<GestureRecognizer> recognizer) {
    if (!recognizer)
        throw std::invalid_argument("add_gesture_recognizer requires a recognizer");
    recognizer->set_owner(this);
    gesture_recognizers_.push_back(std::move(recognizer));
    return *gesture_recognizers_.back();
}

void View::clear_gesture_recognizers() {
    std::vector<GestureRecognizer*> removed_recognizers;
    removed_recognizers.reserve(gesture_recognizers_.size());
    for (auto& recognizer : gesture_recognizers_) {
        if (recognizer)
            removed_recognizers.push_back(recognizer.get());
    }

    for (View* root = this; root; root = root->parent_) {
        if (root->gesture_arbiter_)
            root->gesture_arbiter_->reset();
    }

    if (auto* root = root_for_gesture_relationship_cleanup(this)) {
        auto scrub_relationships = [&](auto&& self, View& node) -> void {
            for (auto& recognizer : node.gesture_recognizers_) {
                if (!recognizer) continue;
                for (auto* removed : removed_recognizers) {
                    if (removed && removed != recognizer.get())
                        recognizer->remove_relationships_to(*removed);
                }
            }
            for (auto& descendant : node.children_)
                self(self, *descendant);
        };
        scrub_relationships(scrub_relationships, *root);
    }

    gesture_recognizers_.clear();
}

GestureRecognizer* View::gesture_recognizer_at(size_t index) {
    if (index >= gesture_recognizers_.size()) return nullptr;
    return gesture_recognizers_[index].get();
}

const GestureRecognizer* View::gesture_recognizer_at(size_t index) const {
    if (index >= gesture_recognizers_.size()) return nullptr;
    return gesture_recognizers_[index].get();
}

bool View::dispatch_gesture_pointer_event(const MouseEvent& root_event,
                                          double timestamp_seconds) {
    if (!gesture_arbiter_)
        gesture_arbiter_ = std::make_unique<GestureArbiter>();
    return gesture_arbiter_->handle_pointer_event(*this, root_event,
                                                  timestamp_seconds);
}

void View::advance_gesture_recognizers(double timestamp_seconds) {
    if (gesture_arbiter_)
        gesture_arbiter_->advance_time(*this, timestamp_seconds);
}

bool View::has_time_driven_gestures() const {
    return gesture_arbiter_ && gesture_arbiter_->wants_time_updates();
}

void View::set_pointer_capture(int pointer_id) {
    if (!has_pointer_capture(pointer_id))
        captured_pointers_.push_back(pointer_id);
}

void View::release_pointer_capture(int pointer_id) {
    auto it = std::find(captured_pointers_.begin(), captured_pointers_.end(), pointer_id);
    if (it != captured_pointers_.end())
        captured_pointers_.erase(it);
}

bool View::has_pointer_capture(int pointer_id) const {
    return std::find(captured_pointers_.begin(), captured_pointers_.end(), pointer_id)
           != captured_pointers_.end();
}

// ── Hover ───────────────────────────────────────────────────────────────

void View::set_hovered(bool h) {
    if (hovered_ == h) return;
    hovered_ = h;
    if (h) {
        on_mouse_enter();
        if (on_hover_enter) on_hover_enter();
    } else {
        on_mouse_leave();
        if (on_hover_leave) on_hover_leave();
    }
}

FrameClock* View::frame_clock() const {
    if (frame_clock_) return frame_clock_;
    if (parent_) return parent_->frame_clock();
    return nullptr;
}

void View::set_frame_clock(FrameClock* clock) {
    frame_clock_ = clock;
    // Hosts build the tree first and install the clock afterward, so any
    // descendant that self-subscribes on a reachable clock must be told the
    // clock is now available — otherwise a Meter built before hosting would
    // silently never subscribe.
    notify_frame_clock_changed();
}

void View::notify_frame_clock_changed() {
    sync_value_bindings();
    on_frame_clock_changed();
    for (auto& child : children_) {
        if (child) child->notify_frame_clock_changed();
    }
}

// ── Live host→view value sources ────────────────────────────────────────────

void View::sync_value_bindings() {
    for (FrameClockBinding* b = value_binding_head_; b; b = b->next_) b->refresh();
}

void View::register_value_binding(FrameClockBinding* b) {
    b->next_ = value_binding_head_;
    value_binding_head_ = b;
}

void View::unregister_value_binding(FrameClockBinding* b) {
    for (FrameClockBinding** slot = &value_binding_head_; *slot; slot = &(*slot)->next_) {
        if (*slot != b) continue;
        *slot = b->next_;
        b->next_ = nullptr;
        return;
    }
}

void View::set_meter_source(std::shared_ptr<MeterSource> source, int channel) {
    // Unbinding a view that never bound anything: nothing to allocate or drop.
    if (!source && !value_bindings_) return;
    if (!value_bindings_) value_bindings_ = std::make_unique<ViewValueBindings>(*this);
    value_bindings_->meter.set_source(std::move(source), channel);
}

bool View::has_meter_source() const {
    return value_bindings_ && value_bindings_->meter.has_source();
}

int View::meter_source_channel() const {
    return value_bindings_ ? value_bindings_->meter.channel() : 0;
}

const MeterFrame& View::meter_frame() const {
    // Paint-safe: a cached copy, or a shared all-zero frame when nothing is
    // bound. Constant-initialized, so reading it costs no thread-safe-static
    // guard (which would be a lock on the paint path).
    static constexpr MeterFrame kNoFrame{};
    return value_bindings_ ? value_bindings_->meter.frame() : kNoFrame;
}

void View::set_scalar_source(std::shared_ptr<ScalarSource> source) {
    if (!source && !value_bindings_) return;
    if (!value_bindings_) value_bindings_ = std::make_unique<ViewValueBindings>(*this);
    value_bindings_->scalar.set_source(std::move(source));
}

bool View::has_scalar_source() const {
    return value_bindings_ && value_bindings_->scalar.has_source();
}

float View::scalar_value() const {
    return value_bindings_ ? value_bindings_->scalar.value() : 0.0f;
}

void View::request_repaint() {
    // A repaint request is the canonical "my content changed" signal, so it is
    // also where the subtree scene cache stales: clear this view and every
    // cached ancestor (their recordings include this view). Cheap no-op when no
    // view anywhere caches. Covers widget mutations, set_theme(), and JS-bridge
    // setters, all of which funnel through here.
    invalidate_subtree_caches_up();
    // set_window_host / set_plugin_view_host propagate to children on
    // add_child, so any attached view sees its own host pointer and we
    // never need to walk the parent chain. No host attached: silent
    // no-op — paint is already on the way for the initial mount, or
    // there's no surface to paint to yet.
    //
    // Route through WindowHost::mark_dirty(), the canonical "set a dirty flag,
    // repaint on the next vblank" path. When a RenderLoop is attached this
    // coalesces N change notifications in one frame into a single vsync-paced
    // repaint; otherwise mark_dirty() degrades to a direct repaint().
    if (window_host_) {
        window_host_->mark_dirty();
    } else if (plugin_view_host_) {
        plugin_view_host_->repaint();
    }
}

void View::request_repaint(const Rect& local_dirty) {
    // Stale the subtree scene cache up the ancestor chain (see request_repaint()
    // above). Done here too because the bounded-success path below calls
    // mark_dirty() directly rather than request_repaint(), so it would not
    // otherwise reach the invalidation.
    invalidate_subtree_caches_up();
    // Bounded invalidation is only wired for the window-host path; the
    // plugin-view-host path (and no host) has no sub-region invalidator, so
    // fall back to a full repaint there.
    if (!window_host_) {
        request_repaint();
        return;
    }
    // Map local_dirty (this view's local space) to root/window space by summing
    // each ancestor's origin (paint applies canvas.translate(bounds_.x,
    // bounds_.y) descending the tree). The plain offset only holds when nothing
    // on the chain moves or spreads this view's pixels past that mapping, so
    // conservatively escalate to a full repaint when:
    //   - the view or an ancestor carries a render transform (affine), or
    //   - the view or an ancestor carries a pixel-spreading filter (blur), or
    //   - an ancestor translates its children's paint (a scrolled ScrollView),
    //     which the offset walk cannot model.
    // Escalating never under-invalidates; it only forgoes the optimization.
    float off_x = 0.0f, off_y = 0.0f;
    for (const View* v = this; v; v = v->parent()) {
        if (v->has_render_transform() || v->has_filter_effect()) {
            request_repaint();
            return;
        }
        // Child-paint offsets (scroll) come from ancestors, not from this view
        // painting itself; a container's own offset does not move its own box.
        if (v != this && v->applies_child_paint_offset()) {
            request_repaint();
            return;
        }
        off_x += v->bounds_.x;
        off_y += v->bounds_.y;
    }
    window_host_->mark_dirty(Rect{off_x + local_dirty.x, off_y + local_dirty.y,
                                  local_dirty.width, local_dirty.height});
}

bool View::start_file_drag(const FileDragRequest& request) {
    if (request.file_paths.empty()) return false;

    // Prefer the plugin host's own outbound-drag backend when it has one. The
    // Windows (OLE) and Linux (XDND) hosts implement start_file_drag() because
    // the drag needs host-owned native state (HWND, or Display* + Xdnd atoms)
    // that the free begin_file_drag(native_view, …) function below cannot see.
    // macOS leaves the host method at its default (false) and is served by the
    // free NSDraggingSession backend, so this falls through there.
    if (plugin_view_host_ && plugin_view_host_->start_file_drag(request))
        return true;

    // Reach the native view of whichever host this tree is attached to (host
    // pointers propagate on add_child, same as request_repaint). The window
    // host exposes its content NSView; the plugin view host's handle IS its
    // NSView. No host attached, or a platform whose host has no native view →
    // no drag. macOS plugin + standalone window hosts land here.
    void* native_view = nullptr;
    if (window_host_) {
        native_view = window_host_->native_content_view_handle();
    } else if (plugin_view_host_) {
        native_view = plugin_view_host_->native_handle();
    }
    if (native_view) return begin_file_drag(native_view, request);

    // Hostless platforms (Android: the tree is a bare root View with no
    // Window/PluginViewHost) fall back to the process-global drag backend the
    // platform layer registered — Android's is a JNI up-call into Kotlin's
    // View.startDragAndDrop. Returns false when no backend is registered.
    return invoke_file_drag_backend(request);
}

void View::simulate_hover(Point root_pos) {
    // Clear hover on all children first via a simple recursive walk
    std::function<void(View*)> clear_hover = [&](View* v) {
        if (v->hovered_) v->set_hovered(false);
        for (size_t i = 0; i < v->child_count(); ++i)
            clear_hover(v->child_at(i));
    };
    clear_hover(this);

    // Set hover on the hit target
    auto* target = hit_test(root_pos);
    if (pulp::view::motion::input_recording_enabled()) {
        const std::string id = target ? target->id() : std::string();
        std::vector<std::pair<std::string, double>> coords;
        coords.emplace_back("x", static_cast<double>(root_pos.x));
        coords.emplace_back("y", static_cast<double>(root_pos.y));
        pulp::view::motion::record_simulated_input("hover", id, std::move(coords));
    }
    if (target) {
        target->set_hovered(true);
        // Also deliver a positioned hover sample so a widget can track which
        // sub-region of itself the pointer is over (e.g. the
        // inspector ToolStrip's per-button tooltip, which set_hovered() alone
        // can't drive because on_mouse_enter carries no coordinate). Convert
        // the root-space point into the target's local space by subtracting
        // its accumulated bounds origin up the parent chain.
        float ox = 0.0f, oy = 0.0f;
        for (View* v = target; v; v = v->parent()) {
            ox += v->bounds().x;
            oy += v->bounds().y;
        }
        target->on_hover_move(Point{root_pos.x - ox, root_pos.y - oy});
    }
}

float View::intrinsic_height() const {
    // Containers: sum visible children's heights + gaps (CSS auto height behavior)
    if (children_.empty()) return 0;

    // column_reverse is still a column-axis container for the auto-height
    // calculation; only true row containers skip child-summed height.
    bool is_col = (flex_.direction == FlexDirection::column ||
                   flex_.direction == FlexDirection::column_reverse);
    if (!is_col) return 0;  // Row containers don't auto-height from children

    float total = 0;
    float gap = flex_.effective_gap(flex_.direction);
    int count = 0;
    for (auto& child : children_) {
        if (!child->visible_) continue;
        auto& cf = child->flex();
        float h = cf.preferred_height;
        if (h <= 0) h = child->intrinsic_height();
        total += h + cf.margin_t() + cf.margin_b();
        if (count > 0) total += gap;
        ++count;
    }

    // Add padding
    float pt = flex_.padding_top >= 0 ? flex_.padding_top : flex_.padding;
    float pb = flex_.padding_bottom >= 0 ? flex_.padding_bottom : flex_.padding;
    return total + pt + pb;
}

#ifdef PULP_HAS_YOGA
void yoga_layout(View& root); // implemented in yoga_layout.cpp
#endif

void layout_grid(View& parent); // implemented in grid_layout.cpp

void View::layout_children() {
    // Frame-pipeline layout pass. With Yoga the root call lays out the whole
    // subtree in one shot, so this reads as one span per frame; grid / custom
    // subtrees that recurse show as nested layout spans.
    PULP_TRACE_SCOPE_NAMED("layout", "layout_children");

    if (children_.empty()) return;

    // Dispatch to grid layout if layout mode is grid
    if (layout_mode_ == LayoutMode::grid) {
        layout_grid(*this);
        return;
    }

#ifdef PULP_HAS_YOGA
    // Use Yoga for flexbox layout (correct margin:auto, flex-wrap, absolute positioning)
    yoga_layout(*this);
    return;
#endif

    auto area = local_bounds();

    // Per-side padding
    float pt = flex_.padding_top >= 0 ? flex_.padding_top : flex_.padding;
    float pr = flex_.padding_right >= 0 ? flex_.padding_right : flex_.padding;
    float pb = flex_.padding_bottom >= 0 ? flex_.padding_bottom : flex_.padding;
    float pl = flex_.padding_left >= 0 ? flex_.padding_left : flex_.padding;
    area = {area.x + pl, area.y + pt, area.width - pl - pr, area.height - pt - pb};

    // row_reverse is still a row-axis container; only the visual order of
    // children is reversed.
    bool is_row = (flex_.direction == FlexDirection::row ||
                   flex_.direction == FlexDirection::row_reverse);
    float main_size = is_row ? area.width : area.height;
    float cross_size = is_row ? area.height : area.width;
    float gap = flex_.effective_gap(flex_.direction);

    // ── Collect visible children, sorted by order ────────────────────
    struct ChildEntry { View* view; int order; };
    std::vector<ChildEntry> ordered;
    for (auto& child : children_) {
        if (!child->visible_) continue;
        ordered.push_back({child.get(), child->flex().order});
    }
    // Stable sort by order (preserves source order for equal values)
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const ChildEntry& a, const ChildEntry& b) { return a.order < b.order; });

    int visible_count = static_cast<int>(ordered.size());
    if (visible_count == 0) return;

    // ── Pass 1: Measure children (flex_basis → preferred → intrinsic) ──
    float total_fixed = 0;
    float total_flex_grow = 0;
    float total_flex_shrink = 0;
    float total_margins = 0;

    for (auto& entry : ordered) {
        auto& cf = entry.view->flex();
        // Main-axis margins
        float margin_before = is_row ? cf.margin_l() : cf.margin_t();
        float margin_after = is_row ? cf.margin_r() : cf.margin_b();
        total_margins += margin_before + margin_after;

        if (cf.flex_grow > 0) {
            total_flex_grow += cf.flex_grow;
        } else {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            float max_val = is_row ? cf.max_width : cf.max_height;
            float size = std::max(basis, min_val);
            if (max_val > 0) size = std::min(size, max_val);
            total_fixed += size;
            total_flex_shrink += cf.flex_shrink;
        }
    }

    float total_gaps = visible_count > 1 ? gap * (visible_count - 1) : 0;
    float remaining = main_size - total_fixed - total_gaps - total_margins;

    // ── Pass 2: Compute child sizes ───────────────────────────────────
    struct ChildLayout { View* view; float main_size; float cross_size;
                         float margin_before; float margin_after;
                         float cross_margin_before; float cross_margin_after; };
    std::vector<ChildLayout> layouts;
    layouts.reserve(static_cast<size_t>(visible_count));

    for (auto& entry : ordered) {
        auto& cf = entry.view->flex();
        float child_main;
        float mb = is_row ? cf.margin_l() : cf.margin_t();
        float ma = is_row ? cf.margin_r() : cf.margin_b();
        float cmb = is_row ? cf.margin_t() : cf.margin_l();
        float cma = is_row ? cf.margin_b() : cf.margin_r();

        if (cf.flex_grow > 0 && remaining > 0) {
            child_main = total_flex_grow > 0 ? remaining * (cf.flex_grow / total_flex_grow) : 0;
        } else if (cf.flex_grow == 0 && remaining < 0 && cf.flex_shrink > 0 && total_flex_shrink > 0) {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            float base = std::max(basis, min_val);
            float shrink_amount = (-remaining) * (cf.flex_shrink / total_flex_shrink);
            child_main = std::max(min_val, base - shrink_amount);
        } else {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            child_main = std::max(basis, min_val);
        }

        float max_main = is_row ? cf.max_width : cf.max_height;
        if (max_main > 0) child_main = std::min(child_main, max_main);

        // Cross-axis sizing — respect align_self override
        FlexAlign align = (cf.align_self != FlexAlign::auto_) ? cf.align_self : flex_.align_items;
        float cross_min = is_row ? cf.min_height : cf.min_width;
        float cross_preferred = is_row ? cf.preferred_height : cf.preferred_width;
        float cross_intrinsic = is_row ? entry.view->intrinsic_height() : entry.view->intrinsic_width();
        float cross_max = is_row ? cf.max_height : cf.max_width;
        float avail_cross = cross_size - cmb - cma;
        float child_cross;

        if (align == FlexAlign::stretch) {
            child_cross = avail_cross;
        } else {
            child_cross = cross_preferred > 0 ? cross_preferred : cross_intrinsic;
            child_cross = std::max(child_cross, cross_min);
            if (child_cross <= 0) child_cross = avail_cross;
        }
        if (cross_max > 0) child_cross = std::min(child_cross, cross_max);

        layouts.push_back({entry.view, child_main, child_cross, mb, ma, cmb, cma});
    }

    // ── Pass 3: Justify content ──────────────────────────────────────
    float total_content = 0;
    for (auto& l : layouts)
        total_content += l.main_size + l.margin_before + l.margin_after;

    float free_space = std::max(0.0f, main_size - total_content - total_gaps);
    float pos = is_row ? area.x : area.y;
    float extra_gap = 0;

    switch (flex_.justify_content) {
        case FlexJustify::start: break;
        case FlexJustify::center: pos += free_space * 0.5f; break;
        case FlexJustify::end_: pos += free_space; break;
        case FlexJustify::space_between:
            if (visible_count > 1) extra_gap = free_space / (visible_count - 1);
            break;
        case FlexJustify::space_around:
            if (visible_count > 0) {
                float around = free_space / visible_count;
                pos += around * 0.5f; extra_gap = around;
            }
            break;
        case FlexJustify::space_evenly:
            if (visible_count > 0) {
                float even = free_space / (visible_count + 1);
                pos += even; extra_gap = even;
            }
            break;
    }

    // ── Pass 4: Position children ────────────────────────────────────
    for (size_t i = 0; i < layouts.size(); ++i) {
        auto& l = layouts[i];
        auto& cf = l.view->flex();

        pos += l.margin_before;

        // Cross-axis position — respect align_self
        FlexAlign align = (cf.align_self != FlexAlign::auto_) ? cf.align_self : flex_.align_items;
        float cross_pos = (is_row ? area.y : area.x) + l.cross_margin_before;
        float avail_cross = cross_size - l.cross_margin_before - l.cross_margin_after;

        switch (align) {
            case FlexAlign::start:
            case FlexAlign::stretch:
            case FlexAlign::auto_:
                break;
            case FlexAlign::center:
                cross_pos += (avail_cross - l.cross_size) * 0.5f;
                break;
            case FlexAlign::end:
                cross_pos += avail_cross - l.cross_size;
                break;
            // Baseline alignment in the manual (non-Yoga) layout fallback
            // approximates as start since glyph baseline metrics aren't
            // surfaced here. Yoga's YGAlignBaseline path is the correct
            // rendering when PULP_HAS_YOGA is on (the default).
            case FlexAlign::baseline:
                break;
        }

        Rect child_bounds;
        if (is_row) {
            child_bounds = {pos, cross_pos, l.main_size, l.cross_size};
        } else {
            child_bounds = {cross_pos, pos, l.cross_size, l.main_size};
        }

        l.view->set_bounds(child_bounds);
        l.view->layout_children();
        pos += l.main_size + l.margin_after + gap + extra_gap;
    }
}

} // namespace pulp::view
