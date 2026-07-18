#pragma once

// Platform-agnostic pointer routing shared by the window hosts.
//
// The coordinate walk and the context-menu lookup used to live inside the macOS
// hosts, which meant they could only be exercised by running a real NSView. They
// are pure view-tree logic, so they live here and each platform host calls in.

#include <pulp/view/view.hpp>

#include <functional>

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

}  // namespace pulp::view
