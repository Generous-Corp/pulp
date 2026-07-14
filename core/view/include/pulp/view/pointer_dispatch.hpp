#pragma once

// Platform-agnostic pointer routing shared by the window hosts.
//
// The coordinate walk and the context-menu lookup used to live inside the macOS
// hosts, which meant they could only be exercised by running a real NSView. They
// are pure view-tree logic, so they live here and each platform host calls in.

#include <pulp/view/view.hpp>

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

}  // namespace pulp::view
