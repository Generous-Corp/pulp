#pragma once

// Platform-agnostic pointer routing shared by the window hosts.
//
// The coordinate walk and the context-menu lookup used to live inside the macOS
// hosts, which meant they could only be exercised by running a real NSView. They
// are pure view-tree logic, so they live here and each platform host calls in.

#include <pulp/view/view.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace pulp::view {

/// Convert `root_pos` (root-local, i.e. window space) into `target`'s
/// local-pre-scale coordinates, peeling off each ancestor's bounds offset,
/// `set_scale` transform, and ScrollView scroll offset along the way.
/// `target` must be `root` or a descendant of it.
Point point_to_local(Point root_pos, View* target, View* root);

/// Route a right-click at `root_pos` to the view under it. Returns true when a
/// view had an `on_context_menu` handler and it was invoked.
///
/// Only the hit view is consulted — the callback does not bubble to ancestors.
bool dispatch_context_menu(View& root, Point root_pos);

/// Deliver one drag tick of an in-flight gesture to the captured `target`.
///
/// ── Delivery contract (asserted by test_pointer_dispatch.cpp) ─────────────
/// The target receives, in this order, EXACTLY ONCE each:
///   1. the MODERN channel — `on_mouse_event(MouseEvent)` with
///      `phase == MousePhase::drag`, carrying `modifiers`, `click_count`,
///      `pointer_type` and `pressure`;
///   2. the LEGACY channel — `on_mouse_drag(Point)` then `on_drag(Point)`,
///      which carry a bare local Point and are kept for compatibility;
///   3. ancestor bubbling — `on_drag` on each ancestor that declares one,
///      re-localized to that ancestor.
///
/// Modern-before-legacy is the documented order across every phase, so a widget
/// that latches modifiers in `on_mouse_event` has them in hand before its legacy
/// handler runs. Before this existed the hosts delivered a drag on the legacy
/// channel ONLY, so a view could read modifiers on press and on release but not
/// DURING a drag — making "hold Shift to fine-adjust", the most common plug-in
/// knob idiom, inexpressible.
///
/// `root_pt` is in root-view space; each channel is handed the point localized
/// to its own receiver. No-op when `target` is null or no longer in the tree.
void deliver_mouse_drag(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count = 1,
                        PointerType pointer_type = PointerType::mouse,
                        float pressure = 0.5f);

/// Host hooks the portable wheel router calls back into. Kept as a struct so
/// a new hook can be threaded in without re-touching every call site.
struct WheelHost {
    /// Invoked after a wheel event mutates the tree (a scroll pane scrolled, a
    /// value widget stepped, a combo popup scrolled) so the host can schedule a
    /// repaint. The standalone window host wires this to `-setNeedsDisplay:`;
    /// the plugin host drives its own CVDisplayLink frame pump and passes an
    /// empty callback (no-op). Empty is always safe.
    std::function<void()> request_repaint;
};

/// Route one wheel/scroll tick to the view tree rooted at `root`.
///
/// This is the mouse-wheel verb shared by the macOS standalone and plugin
/// hosts. Before it existed the identical routing lived inline in both, and
/// drifted. The precedence, asserted by test_pointer_dispatch.cpp, is:
///   1. an open ComboBox popup whose (flip/scroll/clamp-aware) rect contains
///      `root_pt` consumes the wheel to scroll its item list;
///   2. with no hit-testable view under the point, the nearest wheel-scroll
///      container under the cursor scrolls (so an empty scroll-pane background
///      still scrolls without a click first);
///   3. a value widget under the cursor (`wants_wheel_value`) steps its value,
///      taking precedence over any enclosing ScrollView;
///   4. otherwise the wheel bubbles up: the nearest `wants_wheel_scroll`
///      ancestor scrolls and stops the walk, while each ancestor that
///      registered `on_pointer_event` also receives the event (W3C wheel
///      bubble; handlers self-filter on `MouseEvent::is_wheel`);
///   5. with no consumer, the deepest hit receives the event for any default.
///
/// `root_pt` is in root-view space (design-viewport inverse already applied by
/// the host). `scroll_delta_y` is Pulp-convention (already negated from
/// NSEvent's bottom-up `scrollingDeltaY`). `host.request_repaint` fires once
/// after any terminal dispatch that mutated the tree.
void deliver_mouse_wheel(View& root, Point root_pt,
                         float scroll_delta_x, float scroll_delta_y,
                         const WheelHost& host);

/// Deliver a press to an already-resolved `target` (the host has done the
/// hit_test, any combo/overlay pre-routing, and the focus protocol before
/// calling in — those steps diverge per platform/host and stay host-side).
///
/// ── Delivery contract (asserted by test_pointer_dispatch.cpp) ─────────────
/// The target receives, in this order:
///   1. the MODERN channel — `on_mouse_event(MouseEvent)` with
///      `phase == MousePhase::press`, carrying `modifiers` and `click_count`;
///   2. the LEGACY channel — `on_mouse_down(Point)` (bare local point);
///   3. when `bubble` is true, the W3C pointerdown bubble — every ancestor with
///      an `on_pointer_event` handler receives a copy re-localized to its own
///      space (a wrap-div around a canvas child that wins hit_test still sees
///      the press).
///
/// Liveness is re-checked between every hop: a modern handler can unmount the
/// tree (a React state flip that frees `target`), so no later channel derefs a
/// freed view. Returns true when `target` is still in the tree after delivery,
/// false when it was null on entry or unmounted during delivery — the caller
/// clears its captured drag-target slot on false.
///
/// `bubble` defaults to true (the normal press path and the plugin host).
/// The standalone host's overlay-click path passes false to preserve its
/// historical no-bubble behavior; see the call site for the rationale.
bool deliver_mouse_down(View& root, View* target, Point root_pt,
                        uint16_t modifiers, int click_count = 1,
                        bool bubble = true);

/// Host hooks the portable mouse-up router calls back into. Like WheelHost,
/// kept as a struct so the click-firing seam can gain hooks without re-touching
/// every call site.
struct MouseUpHost {
    /// Invoked exactly when the release lands on the SAME view the press
    /// captured (`released == target`, the click-suppression rule: a press on A
    /// that releases over B is NOT a click). `click_handler` is the nearest
    /// `on_click` up the chain from the press target (target inclusive),
    /// resolved and captured BEFORE `on_mouse_up` runs so it survives the target
    /// unmounting during up delivery; it may be empty. `clicked_id` / `modifiers`
    /// carry the immediate hit's id and key state for an optional global-click
    /// report.
    ///
    /// The standalone host defers this behind its `_deferredClickAlive` liveness
    /// token and additionally fires `View::on_global_click`; the plugin host
    /// invokes `click_handler` synchronously and ignores the global-click report
    /// (it has no such path in a DAW editor). Empty is safe — the click is
    /// simply dropped.
    std::function<void(const std::function<void()>& click_handler,
                       const std::string& clicked_id,
                       uint16_t modifiers)>
        fire_click;
};

/// Deliver a release for an in-flight gesture captured on `target`.
///
/// ── Delivery contract (asserted by test_pointer_dispatch.cpp) ─────────────
///   1. resolve `released = root.hit_test(root_pt)` and capture the nearest
///      `on_click` up from `target` BEFORE any delivery;
///   2. the LEGACY channel — `on_mouse_up(Point)` (bare local point);
///   3. the MODERN channel — `on_mouse_event(MouseEvent)` with
///      `phase == MousePhase::release`;
///   4. the W3C pointerup bubble to every `on_pointer_event` ancestor in its
///      own space;
///   5. if `released == target`, `host.fire_click(click_handler, id, modifiers)`
///      — the same-target click-suppression rule.
///
/// Up is LEGACY-before-modern (opposite of drag) because that is the order both
/// mac hosts have always delivered it. Liveness is re-checked before the modern
/// and bubble derefs; the fire-click DECISION uses the captured pointers and
/// runs regardless of an intervening unmount (the standalone token guards the
/// deferred call, the plugin call is synchronous). No-op when `target` is null
/// or not in the tree.
void deliver_mouse_up(View& root, View* target, Point root_pt,
                      uint16_t modifiers, int click_count,
                      const MouseUpHost& host);

}  // namespace pulp::view
